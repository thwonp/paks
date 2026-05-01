#define _GNU_SOURCE
#include "module_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "api.h"
#include "msettings.h"
#include "module_common.h"
#include "plex_log.h"
#include "background.h"
#include "defines.h"
#include "player.h"
#include "plex_api.h"
#include "plex_art.h"
#include "plex_config.h"
#include "plex_models.h"
#include "plex_net.h"
#include "plex_favorites.h"
#include "plex_queue.h"
#include "ui_fonts.h"
#include "ui_utils.h"

extern void PLAT_enableBacklight(int enable);

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

#define TIMELINE_INTERVAL_MS  5000   /* scrobble "playing" every 5 s */
#define SCROBBLE_THRESHOLD    0.90f  /* fire scrobble at 90% playback */
#define PREBUFFER_BYTES  (512 * 1024)   /* 512 KB — enough header + ~1 s of FLAC */

/* ------------------------------------------------------------------
 * Internal screen states (separate from PlayerState in player.h)
 * ------------------------------------------------------------------ */

typedef enum {
    PLAYER_SCREEN_DOWNLOADING,  /* downloading track to temp file */
    PLAYER_SCREEN_PLAYING,      /* track loaded and playing/paused */
    PLAYER_SCREEN_ERROR,        /* download or playback error */
} PlayerScreenState;

/* ------------------------------------------------------------------
 * Sleep state
 * ------------------------------------------------------------------ */

static bool   s_screen_sleeping   = false;
static Uint32 s_last_activity_ms  = 0;

/* ------------------------------------------------------------------
 * Scrobble fire-and-forget infrastructure
 * ------------------------------------------------------------------ */

typedef struct {
    char server_url[PLEX_MAX_URL];
    char token[PLEX_MAX_STR];
    int  rating_key;
    char state[16];       /* "playing" or "stopped"; empty string for scrobble mark */
    int  pos_ms;
    int  dur_ms;
    bool is_mark;         /* true → call plex_api_scrobble; false → plex_api_timeline */
} ScrobbleCtx;

static void *scrobble_worker(void *arg)
{
    ScrobbleCtx *ctx = (ScrobbleCtx *)arg;
    /* reconstruct a minimal PlexConfig on the stack for the API calls */
    PlexConfig tmp;
    memset(&tmp, 0, sizeof(tmp));
    strncpy(tmp.server_url, ctx->server_url, sizeof(tmp.server_url) - 1);
    strncpy(tmp.token,      ctx->token,      sizeof(tmp.token) - 1);

    if (ctx->is_mark)
        plex_api_scrobble(&tmp, ctx->rating_key);
    else
        plex_api_timeline(&tmp, ctx->rating_key, ctx->state, ctx->pos_ms, ctx->dur_ms);

    free(ctx);
    return NULL;
}

static void fire_scrobble(const PlexConfig *cfg, int rating_key,
                          const char *state, int pos_ms, int dur_ms, bool is_mark)
{
    ScrobbleCtx *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    strncpy(ctx->server_url, cfg->server_url, sizeof(ctx->server_url) - 1);
    strncpy(ctx->token,      cfg->token,      sizeof(ctx->token) - 1);
    ctx->server_url[sizeof(ctx->server_url) - 1] = '\0';
    ctx->token[sizeof(ctx->token) - 1]           = '\0';
    ctx->rating_key = rating_key;
    ctx->pos_ms     = pos_ms;
    ctx->dur_ms     = dur_ms;
    ctx->is_mark    = is_mark;
    if (state) strncpy(ctx->state, state, sizeof(ctx->state) - 1);
    ctx->state[sizeof(ctx->state) - 1] = '\0';

    pthread_t t;
    if (pthread_create(&t, NULL, scrobble_worker, ctx) != 0) {
        free(ctx);
        return;
    }
    pthread_detach(t);
}

/* ------------------------------------------------------------------
 * Download worker context
 * ------------------------------------------------------------------ */

typedef struct {
    char              url[2048];
    char              filepath[768];
    volatile int      progress_pct;
    volatile bool     download_done;
    volatile bool     download_failed;
    volatile bool     should_cancel;
    PlexNetOptions    opts;
} DownloadCtx;

/* ------------------------------------------------------------------
 * Module-level playback state — survives module_player_run() returning
 * so that background playback and auto-advance work correctly.
 * ------------------------------------------------------------------ */

typedef struct {
    DownloadCtx  dl_ctx;
    pthread_t    dl_thread;
    bool         dl_thread_running;  /* true when thread is joinable */
    bool         download_pending;   /* file is still growing (progressive playback) */
    char         temp_path[768];
    char         ext[16];
    bool         is_local_file;
    bool         scrobbled;
    uint32_t     last_timeline_ms;
    int          transcode;          /* cfg->stream_bitrate_kbps > 0 at track-start time */
} PlayerModuleState;

static PlayerModuleState s_state;

static void *download_worker(void *arg)
{
    DownloadCtx *ctx = (DownloadCtx *)arg;

    int ret = plex_net_download_file(ctx->url, ctx->filepath,
                                     &ctx->progress_pct,
                                     &ctx->should_cancel,
                                     &ctx->opts);
    if (ctx->should_cancel) {
        /* cancelled — don't touch done/failed flags */
    } else if (ret >= 0) {
        ctx->download_done = true;
    } else {
        ctx->download_failed = true;
    }

    return NULL;
}

/* ------------------------------------------------------------------
 * Temp-file path helpers
 * ------------------------------------------------------------------ */

/* Extract extension from media_key (last '.' before any '?'). */
static void extract_ext(const char *media_key, char *out, int out_size)
{
    if (!media_key || media_key[0] == '\0') {
        snprintf(out, out_size, "mp3");
        return;
    }
    /* find last '.' before '?' */
    const char *q = strchr(media_key, '?');
    const char *dot = NULL;
    const char *p = media_key;
    while (*p && (!q || p < q)) {
        if (*p == '.') dot = p;
        p++;
    }
    if (!dot || *(dot + 1) == '\0') {
        snprintf(out, out_size, "mp3");
        return;
    }
    snprintf(out, out_size, "%s", dot + 1);
    /* strip any trailing query string that might have been included */
    char *qm = strchr(out, '?');
    if (qm) *qm = '\0';
}

/* Build temp file path into out_buf (out_size bytes). */
static void build_temp_path(const char *ext, char *out_buf, int out_size)
{
    const char *base = getenv("USERDATA_PATH");
    if (!base || base[0] == '\0') base = "/mnt/SDCARD/.userdata/tg5040";

    char parent[512];
    snprintf(parent, sizeof(parent), "%s/plexmusic", base);
    mkdir(parent, 0755);

    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "%s/plexmusic/tmp", base);
    mkdir(tmpdir, 0755);

    snprintf(out_buf, out_size, "%s/current_track.%s", tmpdir, ext);
}

/* ------------------------------------------------------------------
 * Rendering helpers
 * ------------------------------------------------------------------ */

#define COVER_SIZE    (is_brick ? SCALE1(100) : SCALE1(160))
#define PROGRESS_H    SCALE1(8)
#define PADDING       SCALE1(12)

static void render_cover_art(SDL_Surface *screen, SDL_Surface *art, int x, int y)
{
    if (!art) return;
    SDL_Rect src = { 0, 0, art->w, art->h };
    SDL_Rect dst = { x, y, COVER_SIZE, COVER_SIZE };
    SDL_BlitScaled(art, &src, screen, &dst);
}

static void render_progress_bar(SDL_Surface *screen, int y, float frac,
                                 uint32_t bg_color, uint32_t fg_color)
{
    int x = PADDING;
    int w = screen->w - 2 * PADDING;

    SDL_Rect bg = { x, y, w, PROGRESS_H };
    SDL_FillRect(screen, &bg, bg_color);

    if (frac > 0.0f) {
        int fw = (int)(w * frac);
        if (fw < 1) fw = 1;
        if (fw > w) fw = w;
        SDL_Rect fg = { x, y, fw, PROGRESS_H };
        SDL_FillRect(screen, &fg, fg_color);
    }
}

static void render_text_centered(SDL_Surface *screen, TTF_Font *font,
                                  SDL_Color color, const char *text, int y)
{
    if (!text || !text[0]) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Rect dst = { (screen->w - surf->w) / 2, y, surf->w, surf->h };
    SDL_BlitSurface(surf, NULL, screen, &dst);
    SDL_FreeSurface(surf);
}

static void render_text_left(SDL_Surface *screen, TTF_Font *font,
                              SDL_Color color, const char *text, int x, int y)
{
    if (!text || !text[0]) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_BlitSurface(surf, NULL, screen, &dst);
    SDL_FreeSurface(surf);
}

/* ------------------------------------------------------------------
 * Render: downloading state
 * ------------------------------------------------------------------ */

static void render_downloading(SDL_Surface *screen,
                                const PlexTrack *track,
                                int progress_pct)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    SDL_Surface *art = plex_art_get();
    int art_x = (screen->w - COVER_SIZE) / 2;
    render_cover_art(screen, art, art_x, PADDING);

    int text_y = COVER_SIZE + PADDING * 2;

    SDL_Color white = COLOR_WHITE;
    SDL_Color gray  = { 0xAA, 0xAA, 0xAA, 0xFF };

    TTF_Font *title_font = is_brick ? Fonts_getLarge() : Fonts_getTitle();
    render_text_centered(screen, title_font,        white, track->title,  text_y);
    text_y += TTF_FontHeight(title_font) + SCALE1(4);
    render_text_centered(screen, Fonts_getArtist(), gray,  track->artist, text_y);
    text_y += TTF_FontHeight(Fonts_getArtist()) + SCALE1(4);
    render_text_centered(screen, Fonts_getAlbum(),  gray,  track->album,  text_y);
    text_y += TTF_FontHeight(Fonts_getAlbum())  + PADDING;

    /* Progress bar */
    uint32_t bg_col = SDL_MapRGB(screen->format, 0x40, 0x40, 0x40);
    uint32_t fg_col = SDL_MapRGB(screen->format, 0x22, 0x88, 0xFF);
    float frac = (float)progress_pct / 100.0f;
    render_progress_bar(screen, text_y, frac, bg_col, fg_col);
    text_y += PROGRESS_H + SCALE1(4);

    /* Label */
    char label[32];
    if (progress_pct <= 0) {
        snprintf(label, sizeof(label), "Loading...");
    } else {
        snprintf(label, sizeof(label), "%d%%", progress_pct);
    }
    render_text_centered(screen, Fonts_getSmall(), gray, label, text_y);

    GFX_flip(screen);
}

/* ------------------------------------------------------------------
 * Render: playing state
 * ------------------------------------------------------------------ */

static void render_icon_row(SDL_Surface *screen, const PlexQueue *queue,
                             const PlexTrack *track, int icons_y)
{
    if (!queue) return;

    SDL_Color white = COLOR_WHITE;
    SDL_Color gray  = { 0xAA, 0xAA, 0xAA, 0xFF };

    int col_w = (screen->w - 2 * PADDING) / 3;
    int x0 = PADDING;
    int x1 = PADDING + col_w;
    int x2 = PADDING + col_w * 2;

    /* Shuffle */
    {
        const char *label = queue->shuffle ? "[L2] Shuffle: On" : "[L2] Shuffle: Off";
        SDL_Color   col   = queue->shuffle ? white : gray;
        render_text_left(screen, Fonts_getSmall(), col, label, x0, icons_y);
    }

    /* Repeat */
    {
        const char *label;
        SDL_Color   col;
        if (queue->repeat_mode == REPEAT_ALL) {
            label = "[R2] Repeat: All";
            col   = white;
        } else if (queue->repeat_mode == REPEAT_ONE) {
            label = "[R2] Repeat: One";
            col   = white;
        } else {
            label = "[R2] Repeat: Off";
            col   = gray;
        }
        render_text_left(screen, Fonts_getSmall(), col, label, x1, icons_y);
    }

    /* Favorite */
    {
        bool fav = track ? plex_favorites_contains(track->rating_key) : false;
        const char *label = fav ? "[Y] \xe2\x99\xa5" : "[Y] \xe2\x99\xa1";
        SDL_Color   col   = fav ? white : gray;
        render_text_left(screen, Fonts_getSmall(), col, label, x2, icons_y);
    }
}

static void render_playing_screen(SDL_Surface *screen,
                                   const PlexTrack *track)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    SDL_Surface *art = plex_art_get();
    int art_x = (screen->w - COVER_SIZE) / 2;
    render_cover_art(screen, art, art_x, PADDING);

    int text_y = COVER_SIZE + PADDING * 2;

    SDL_Color white = COLOR_WHITE;
    SDL_Color gray  = { 0xAA, 0xAA, 0xAA, 0xFF };

    TTF_Font *title_font = is_brick ? Fonts_getLarge() : Fonts_getTitle();
    render_text_centered(screen, title_font,        white, track->title,  text_y);
    text_y += TTF_FontHeight(title_font) + SCALE1(4);
    render_text_centered(screen, Fonts_getArtist(), gray,  track->artist, text_y);
    text_y += TTF_FontHeight(Fonts_getArtist()) + SCALE1(4);
    render_text_centered(screen, Fonts_getAlbum(),  gray,  track->album,  text_y);

    /* Playback progress bar — anchored from bottom */
    int pos_ms = Player_getPosition();
    int dur_ms = Player_getDuration();
    float frac = (dur_ms > 0) ? ((float)pos_ms / (float)dur_ms) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    uint32_t bg_col = SDL_MapRGB(screen->format, 0x40, 0x40, 0x40);
    uint32_t fg_col = SDL_MapRGB(screen->format, 0x22, 0x88, 0xFF);

    int bar_y    = screen->h - SCALE1(60);
    int time_y   = bar_y + PROGRESS_H + SCALE1(6);
    int icons_y  = bar_y - TTF_FontHeight(Fonts_getSmall()) - SCALE1(10);

    /* Horizontal icon row just above progress bar */
    render_icon_row(screen, plex_queue_get(), track, icons_y);

    render_progress_bar(screen, bar_y, frac, bg_col, fg_col);

    /* M:SS / M:SS */
    char pos_str[16], dur_str[16], time_label[40];
    format_time(pos_str, pos_ms);
    format_time(dur_str, dur_ms);
    snprintf(time_label, sizeof(time_label), "%s / %s", pos_str, dur_str);
    render_text_centered(screen, Fonts_getSmall(), gray, time_label, time_y);

    GFX_flip(screen);
}

/* ------------------------------------------------------------------
 * Render: error state
 * ------------------------------------------------------------------ */

static void render_error(SDL_Surface *screen)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
    SDL_Color white = COLOR_WHITE;
    SDL_Color gray  = { 0xAA, 0xAA, 0xAA, 0xFF };
    int cy = screen->h / 2 - TTF_FontHeight(Fonts_getMedium());
    render_text_centered(screen, Fonts_getMedium(), white,
                         "Failed to load track.", cy);
    cy += TTF_FontHeight(Fonts_getMedium()) + SCALE1(8);
    render_text_centered(screen, Fonts_getSmall(), gray,
                         "Press A to retry, B to go back.", cy);
    GFX_flip(screen);
}

/* ------------------------------------------------------------------
 * Start a download for current queue entry
 * ------------------------------------------------------------------ */

static void start_download(DownloadCtx *ctx, pthread_t *thread,
                            const PlexQueue *queue,
                            const char *temp_path)
{
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->url,      sizeof(ctx->url),      "%s", queue->stream_url);
    snprintf(ctx->filepath, sizeof(ctx->filepath),  "%s", temp_path);
    ctx->opts.method      = PLEX_HTTP_GET;
    ctx->opts.token       = NULL;  /* token is already in the URL */
    ctx->opts.timeout_sec = 60;    /* audio files can be large */

    pthread_create(thread, NULL, download_worker, ctx);
}

/* ------------------------------------------------------------------
 * Load the track at the current queue index (after plex_queue_next/prev
 * has already been called).  Updates s_state fields, starts a download
 * or plays the local file directly.
 * Caller must have stopped/joined any previous download and called
 * Player_stop() before invoking this.
 * screen_state may be NULL when called from background tick (no screen).
 * ------------------------------------------------------------------ */

static void load_next_track(PlexQueue *queue,
                             PlayerScreenState *screen_state)
{
    const PlexTrack *t = plex_queue_current_track();
    s_state.is_local_file = t ? (t->local_path[0] != '\0') : false;

    if (s_state.is_local_file && t) {
        strncpy(s_state.temp_path, t->local_path,
                sizeof(s_state.temp_path) - 1);
        s_state.temp_path[sizeof(s_state.temp_path) - 1] = '\0';
        s_state.ext[0] = '\0';
    } else if (t) {
        if (s_state.transcode)
            strncpy(s_state.ext, "opus", sizeof(s_state.ext) - 1);
        else
            extract_ext(t->media_key, s_state.ext, sizeof(s_state.ext));
        s_state.ext[sizeof(s_state.ext) - 1] = '\0';
        build_temp_path(s_state.ext, s_state.temp_path, sizeof(s_state.temp_path));
    }

    s_state.download_pending = false;

    if (s_state.is_local_file) {
        Player_setFileGrowing(false);
        if (Player_load(s_state.temp_path) == 0) {
            Player_play();
            if (screen_state) *screen_state = PLAYER_SCREEN_PLAYING;
        } else {
            PLEX_LOG_ERROR("[Player] Player_load failed (offline): %s\n", s_state.temp_path);
            if (screen_state) *screen_state = PLAYER_SCREEN_ERROR;
        }
    } else {
        memset(&s_state.dl_ctx, 0, sizeof(s_state.dl_ctx));
        start_download(&s_state.dl_ctx, &s_state.dl_thread, queue, s_state.temp_path);
        s_state.dl_thread_running = true;
        if (screen_state) *screen_state = PLAYER_SCREEN_DOWNLOADING;
    }

    s_state.scrobbled        = false;
    s_state.last_timeline_ms = SDL_GetTicks();
}

/* ------------------------------------------------------------------
 * Quit cleanup helper — cancel download, stop player, remove temp file.
 * ------------------------------------------------------------------ */

static void player_module_quit_cleanup(PlayerScreenState screen_state)
{
    if (s_state.dl_thread_running) {
        s_state.dl_ctx.should_cancel = true;
        Player_setFileGrowing(false);
        pthread_join(s_state.dl_thread, NULL);
        s_state.dl_thread_running = false;
        s_state.download_pending  = false;
    }
    Player_stop();
    if (!s_state.is_local_file && s_state.temp_path[0])
        remove(s_state.temp_path);
    Background_setActive(BG_NONE);
    (void)screen_state;
}

/* ------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------ */

AppModule module_player_run(SDL_Surface *screen)
{
    const PlexConfig *cfg   = plex_config_get_mutable();
    PlexQueue        *queue = plex_queue_get();

    if (!queue || !queue->active || queue->count == 0) {
        PLEX_LOG_ERROR("[Player] module_player_run: no active queue\n");
        return MODULE_BROWSE;
    }

    PlayerScreenState screen_state = PLAYER_SCREEN_DOWNLOADING;

    /* Re-entry with background active: skip download, resume UI */
    if (Background_getActive() == BG_MUSIC && Player_getState() != PLAYER_STATE_STOPPED) {
        screen_state              = PLAYER_SCREEN_PLAYING;
        s_state.scrobbled         = false;
        s_state.last_timeline_ms  = SDL_GetTicks();
        {
            const PlexTrack *t = plex_queue_current_track();
            if (t) plex_art_fetch(cfg, t->thumb);
        }
        /* s_state.download_pending is already correct from before B-press */
        /* fall through to main loop with PLAYER_SCREEN_PLAYING */
    } else {
        /* Normal entry: cancel any stale background download first */
        if (s_state.dl_thread_running) {
            s_state.dl_ctx.should_cancel = true;
            Player_setFileGrowing(false);
            pthread_join(s_state.dl_thread, NULL);
            s_state.dl_thread_running = false;
            if (!s_state.is_local_file) remove(s_state.temp_path);
            s_state.download_pending = false;
        }

        /* Fetch cover art and start download.
         * Clear any stale BG_MUSIC state immediately so that if the user
         * cancels the download (B-press) or hits an error, browse will not
         * show a stale "Now Playing" item. */
        Background_setActive(BG_NONE);
        {
            const PlexTrack *t = plex_queue_current_track();
            if (t) plex_art_fetch(cfg, t->thumb);
        }

        /* Populate s_state fields for the new track */
        s_state.transcode     = (cfg->stream_bitrate_kbps > 0);
        {
            const PlexTrack *t = plex_queue_current_track();
            s_state.is_local_file = t ? (t->local_path[0] != '\0') : false;
            if (s_state.is_local_file && t) {
                strncpy(s_state.temp_path, t->local_path,
                        sizeof(s_state.temp_path) - 1);
                s_state.temp_path[sizeof(s_state.temp_path) - 1] = '\0';
                s_state.ext[0] = '\0';
            } else if (t) {
                if (s_state.transcode)
                    strncpy(s_state.ext, "opus", sizeof(s_state.ext) - 1);
                else
                    extract_ext(t->media_key, s_state.ext, sizeof(s_state.ext));
                s_state.ext[sizeof(s_state.ext) - 1] = '\0';
                build_temp_path(s_state.ext, s_state.temp_path, sizeof(s_state.temp_path));
            }
        }

        s_state.download_pending   = false;
        s_state.dl_thread_running  = false;
        s_state.scrobbled          = false;
        s_state.last_timeline_ms   = 0;

        if (s_state.is_local_file) {
            PLEX_LOG("[Player] Loading offline file: %s\n", s_state.temp_path);
            Player_setFileGrowing(false);
            if (Player_load(s_state.temp_path) == 0) {
                Player_play();
                s_state.scrobbled        = false;
                s_state.last_timeline_ms = SDL_GetTicks();
                screen_state             = PLAYER_SCREEN_PLAYING;
            } else {
                PLEX_LOG_ERROR("[Player] Player_load failed (offline): %s\n", s_state.temp_path);
                screen_state = PLAYER_SCREEN_ERROR;
            }
        } else {
            memset(&s_state.dl_ctx, 0, sizeof(s_state.dl_ctx));
            start_download(&s_state.dl_ctx, &s_state.dl_thread, queue, s_state.temp_path);
            s_state.dl_thread_running = true;
        }
    }

    int dirty       = 1;
    int show_setting = 0;

    bool left_armed  = false;
    bool right_armed = false;
    bool left_seeked  = false;
    bool right_seeked = false;

    /* Sleep state init */
    s_screen_sleeping  = false;
    s_last_activity_ms = SDL_GetTicks();

    while (1) {
        GFX_startFrame();
        PAD_poll();

        /* ---- Screen sleep / wake ---- */
        if (cfg->screen_timeout > 0 && !s_screen_sleeping) {
            if (SDL_GetTicks() - s_last_activity_ms > (Uint32)(cfg->screen_timeout * 1000)) {
                SetRawBrightness(0);
                s_screen_sleeping = true;
            }
        }

        if (s_screen_sleeping) {
            GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0);
            if (global.should_quit) {
                PLAT_enableBacklight(1);
                s_screen_sleeping = false;
                return MODULE_QUIT;
            }
            bool woke = cfg->pocket_lock_enabled
                ? (PAD_isPressed(BTN_MENU) && PAD_justPressed(BTN_SELECT))
                : PAD_anyPressed();
            if (woke) {
                PLAT_enableBacklight(1);
                s_screen_sleeping  = false;
                s_last_activity_ms = SDL_GetTicks();
                dirty = 1;
            }
            GFX_sync();
            continue;  /* consume all other input while sleeping */
        }

        /* Reset idle timer on any input while awake */
        if (PAD_anyPressed()) s_last_activity_ms = SDL_GetTicks();

        /* Power management heartbeat — must run every awake frame */
        ModuleCommon_PWR_update(&dirty, &show_setting);

        /* Global input (MENU+START = quit, START = help/confirm) */
        {
            GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0);
            if (global.should_quit) {
                if (screen_state == PLAYER_SCREEN_PLAYING) {
                    const PlexTrack *t = plex_queue_current_track();
                    if (t) fire_scrobble(cfg, t->rating_key,
                                         "stopped", Player_getPosition(),
                                         Player_getDuration(), false);
                }
                player_module_quit_cleanup(screen_state);
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_QUIT;
            }
            if (global.input_consumed) {
                GFX_sync();
                continue;
            }
        }

        /* ---- Art async update ---- */
        if (plex_art_is_fetching()) dirty = 1;

        /* ====================================================
         * DOWNLOADING state
         * ==================================================== */
        if (screen_state == PLAYER_SCREEN_DOWNLOADING) {

            /* Check for thread completion */
            if (s_state.dl_ctx.download_done) {
                pthread_join(s_state.dl_thread, NULL);
                s_state.dl_thread_running = false;
                s_state.download_pending  = false;

                /* Load and play */
                Player_stop();
                if (Player_load(s_state.temp_path) == 0) {
                    if (s_state.transcode) {
                        const PlexTrack *t = plex_queue_current_track();
                        int dur = t ? t->duration_ms : 0;
                        if (dur > 0) Player_setTotalFrames((int64_t)((dur / 1000.0) * 48000.0));
                    }
                    Player_play();
                    s_state.scrobbled        = false;
                    s_state.last_timeline_ms = SDL_GetTicks();
                    screen_state             = PLAYER_SCREEN_PLAYING;
                    dirty                    = 1;
                } else {
                    if (!s_state.is_local_file) remove(s_state.temp_path);
                    PLEX_LOG_ERROR("[Player] Player_load failed: %s\n", s_state.temp_path);
                    screen_state = PLAYER_SCREEN_ERROR;
                    dirty        = 1;
                }
                goto render;
            }

            if (s_state.dl_ctx.download_failed) {
                pthread_join(s_state.dl_thread, NULL);
                s_state.dl_thread_running = false;
                screen_state = PLAYER_SCREEN_ERROR;
                dirty        = 1;
                goto render;
            }

            /* Try to start playback early once enough bytes are on disk.
             * MP3/FLAC/WAV/M4A: total_frames is header-derived, safe on partial files.
             * Opus (transcoded): total_frames is overridden via Player_setTotalFrames using
             * Plex-provided duration, so it is also safe for early-start. */
            if (!s_state.download_pending
                    && (strcasecmp(s_state.ext, "mp3") == 0
                        || strcasecmp(s_state.ext, "flac") == 0
                        || strcasecmp(s_state.ext, "wav") == 0
                        || strcasecmp(s_state.ext, "m4a") == 0
                        || strcasecmp(s_state.ext, "opus") == 0)) {
                struct stat st;
                if (stat(s_state.temp_path, &st) == 0 && st.st_size >= PREBUFFER_BYTES) {
                    Player_stop();
                    if (Player_load(s_state.temp_path) == 0) {
                        if (s_state.transcode) {
                            const PlexTrack *t = plex_queue_current_track();
                            int dur = t ? t->duration_ms : 0;
                            if (dur > 0) Player_setTotalFrames((int64_t)((dur / 1000.0) * 48000.0));
                        }
                        Player_setFileGrowing(true);
                        Player_play();
                        s_state.download_pending    = true;
                        s_state.scrobbled           = false;
                        s_state.last_timeline_ms    = SDL_GetTicks();
                        screen_state                = PLAYER_SCREEN_PLAYING;
                        dirty                       = 1;
                        goto render;
                    }
                    /* Player_load failed (e.g. M4A moov at end) — fall through to full download */
                }
            }

            dirty = 1;  /* keep updating progress bar */

            /* Input during download */
            if (PAD_justPressed(BTN_B)) {
                if (!s_state.is_local_file) {
                    s_state.dl_ctx.should_cancel = true;
                    pthread_join(s_state.dl_thread, NULL);
                    s_state.dl_thread_running = false;
                    remove(s_state.temp_path);
                }
                Player_stop();
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_BROWSE;
            }
        }

        /* ====================================================
         * PLAYING state
         * ==================================================== */
        else if (screen_state == PLAYER_SCREEN_PLAYING) {
            /* Background download finished while we were already playing */
            if (s_state.download_pending) {
                if (s_state.dl_ctx.download_done) {
                    pthread_join(s_state.dl_thread, NULL);
                    s_state.dl_thread_running = false;
                    Player_setFileGrowing(false);
                    s_state.download_pending = false;
                } else if (s_state.dl_ctx.download_failed) {
                    pthread_join(s_state.dl_thread, NULL);
                    s_state.dl_thread_running = false;
                    Player_setFileGrowing(false);
                    s_state.download_pending = false;
                    /* Track may stop early if download failed; user can press Next */
                }
            }

            Player_update();

            /* Auto-advance on track end */
            if (Player_getState() == PLAYER_STATE_STOPPED) {
                Background_setActive(BG_NONE);
                /* download_pending should be false here (handled above), but
                 * guard anyway */
                if (s_state.download_pending) {
                    s_state.dl_ctx.should_cancel = true;
                    Player_setFileGrowing(false);
                    pthread_join(s_state.dl_thread, NULL);
                    s_state.dl_thread_running = false;
                    s_state.download_pending  = false;
                }
                Player_stop();
                if (!s_state.is_local_file) remove(s_state.temp_path);

                if (queue->repeat_mode == REPEAT_ONE) {
                    /* Reload current track without advancing queue */
                    plex_art_clear();
                    const PlexTrack *t = plex_queue_current_track();
                    if (t) plex_art_fetch(cfg, t->thumb);
                    s_state.transcode = (cfg->stream_bitrate_kbps > 0);
                    load_next_track(queue, &screen_state);
                    dirty = 1;
                    goto render;
                }

                if (plex_queue_has_next()) {
                    plex_queue_next(cfg);

                    plex_art_clear();
                    {
                        const PlexTrack *t = plex_queue_current_track();
                        if (t) plex_art_fetch(cfg, t->thumb);
                    }

                    s_state.transcode = (cfg->stream_bitrate_kbps > 0);
                    load_next_track(queue, &screen_state);
                    dirty = 1;
                    goto render;
                } else {
                    /* End of queue — already stopped above; just return to browse */
                    if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                    return MODULE_BROWSE;
                }
            }

            /* Scrobbling */
            uint32_t now_ms  = SDL_GetTicks();
            int      pos_ms  = Player_getPosition();
            int      dur_ms  = Player_getDuration();

            if (Player_getState() == PLAYER_STATE_PLAYING) {
                const PlexTrack *scrobble_t = plex_queue_current_track();
                if (scrobble_t) {
                    if (now_ms - s_state.last_timeline_ms >= TIMELINE_INTERVAL_MS) {
                        if (dur_ms > 0) {
                            fire_scrobble(cfg, scrobble_t->rating_key,
                                          "playing", pos_ms, dur_ms, false);
                        }
                        s_state.last_timeline_ms = now_ms;
                    }

                    if (!s_state.scrobbled && dur_ms > 0 &&
                        (float)pos_ms / (float)dur_ms >= SCROBBLE_THRESHOLD) {
                        fire_scrobble(cfg, scrobble_t->rating_key,
                                      NULL, 0, 0, true);
                        s_state.scrobbled = true;
                    }
                }
            }

            dirty = 1;  /* keep time display fresh */

            /* Input during playback */
            if (PAD_justPressed(BTN_A)) {
                Player_togglePause();
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                /* Do NOT cancel download — let it continue in the background */
                /* Keep playing in background, return to browse */
                Background_setActive(BG_MUSIC);
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_BROWSE;
            }
            else if (PAD_justPressed(BTN_L2)) {
                plex_queue_toggle_shuffle();
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_R2)) {
                plex_queue_cycle_repeat();
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_Y)) {
                const PlexTrack *t = plex_queue_current_track();
                if (t) plex_favorites_toggle(t);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_L1)) {
                if (plex_queue_has_prev()) {
                    {
                        const PlexTrack *t = plex_queue_current_track();
                        if (t) fire_scrobble(cfg, t->rating_key,
                                             "stopped", Player_getPosition(),
                                             Player_getDuration(), false);
                    }
                    Background_setActive(BG_NONE);

                    /* Capture old path info before advancing queue. */
                    bool old_is_local = s_state.is_local_file;
                    char old_temp_path[768];
                    strncpy(old_temp_path, s_state.temp_path, sizeof(old_temp_path) - 1);
                    old_temp_path[sizeof(old_temp_path) - 1] = '\0';

                    /* Advance queue and update s_state paths for render. */
                    plex_queue_prev(cfg);

                    {
                        const PlexTrack *t = plex_queue_current_track();
                        s_state.is_local_file = t ? (t->local_path[0] != '\0') : false;
                        if (s_state.is_local_file && t) {
                            strncpy(s_state.temp_path, t->local_path,
                                    sizeof(s_state.temp_path) - 1);
                            s_state.temp_path[sizeof(s_state.temp_path) - 1] = '\0';
                            s_state.ext[0] = '\0';
                        } else if (t) {
                            if (cfg->stream_bitrate_kbps > 0)
                                strncpy(s_state.ext, "opus", sizeof(s_state.ext) - 1);
                            else
                                extract_ext(t->media_key, s_state.ext, sizeof(s_state.ext));
                            s_state.ext[sizeof(s_state.ext) - 1] = '\0';
                            build_temp_path(s_state.ext, s_state.temp_path, sizeof(s_state.temp_path));
                        }
                    }

                    plex_art_clear();
                    {
                        const PlexTrack *t = plex_queue_current_track();
                        if (t) plex_art_fetch(cfg, t->thumb);

                        /* Render the new track's downloading/loading screen immediately. */
                        if (t) render_downloading(screen, t, 0);
                    }

                    /* Now do the blocking stop/join behind the visible frame. */
                    if (s_state.download_pending) {
                        s_state.dl_ctx.should_cancel = true;
                        Player_setFileGrowing(false);
                        pthread_join(s_state.dl_thread, NULL);
                        s_state.dl_thread_running = false;
                        s_state.download_pending  = false;
                    }
                    Player_stop();
                    if (!old_is_local) remove(old_temp_path);

                    s_state.transcode = (cfg->stream_bitrate_kbps > 0);
                    load_next_track(queue, &screen_state);
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_R1)) {
                if (plex_queue_has_next()) {
                    {
                        const PlexTrack *t = plex_queue_current_track();
                        if (t) fire_scrobble(cfg, t->rating_key,
                                             "stopped", Player_getPosition(),
                                             Player_getDuration(), false);
                    }
                    Background_setActive(BG_NONE);

                    /* Capture old path info before advancing queue. */
                    bool old_is_local = s_state.is_local_file;
                    char old_temp_path[768];
                    strncpy(old_temp_path, s_state.temp_path, sizeof(old_temp_path) - 1);
                    old_temp_path[sizeof(old_temp_path) - 1] = '\0';

                    /* Advance queue and update s_state paths for render. */
                    plex_queue_next(cfg);

                    {
                        const PlexTrack *t = plex_queue_current_track();
                        s_state.is_local_file = t ? (t->local_path[0] != '\0') : false;
                        if (s_state.is_local_file && t) {
                            strncpy(s_state.temp_path, t->local_path,
                                    sizeof(s_state.temp_path) - 1);
                            s_state.temp_path[sizeof(s_state.temp_path) - 1] = '\0';
                            s_state.ext[0] = '\0';
                        } else if (t) {
                            if (cfg->stream_bitrate_kbps > 0)
                                strncpy(s_state.ext, "opus", sizeof(s_state.ext) - 1);
                            else
                                extract_ext(t->media_key, s_state.ext, sizeof(s_state.ext));
                            s_state.ext[sizeof(s_state.ext) - 1] = '\0';
                            build_temp_path(s_state.ext, s_state.temp_path, sizeof(s_state.temp_path));
                        }
                    }

                    plex_art_clear();
                    {
                        const PlexTrack *t = plex_queue_current_track();
                        if (t) plex_art_fetch(cfg, t->thumb);

                        /* Render the new track's downloading/loading screen immediately. */
                        if (t) render_downloading(screen, t, 0);
                    }

                    /* Now do the blocking stop/join behind the visible frame. */
                    if (s_state.download_pending) {
                        s_state.dl_ctx.should_cancel = true;
                        Player_setFileGrowing(false);
                        pthread_join(s_state.dl_thread, NULL);
                        s_state.dl_thread_running = false;
                        s_state.download_pending  = false;
                    }
                    Player_stop();
                    if (!old_is_local) remove(old_temp_path);

                    s_state.transcode = (cfg->stream_bitrate_kbps > 0);
                    load_next_track(queue, &screen_state);
                    dirty = 1;
                }
            }

            /* BTN_LEFT: tap = prev track, hold = seek -10 s per repeat tick */
            if (PAD_justPressed(BTN_LEFT)) {
                left_armed  = true;
                left_seeked = false;
            }
            if (PAD_justRepeated(BTN_LEFT) && left_armed && !PAD_justPressed(BTN_LEFT)) {
                Player_seek(Player_getPosition() - cfg->seek_interval_ms);
                left_seeked = true;
                dirty = 1;
            }
            if (PAD_justReleased(BTN_LEFT) && left_armed) {
                if (!left_seeked) {
                    if (plex_queue_has_prev()) {
                        {
                            const PlexTrack *t = plex_queue_current_track();
                            if (t) fire_scrobble(cfg, t->rating_key,
                                                 "stopped", Player_getPosition(),
                                                 Player_getDuration(), false);
                        }
                        Background_setActive(BG_NONE);

                        bool old_is_local = s_state.is_local_file;
                        char old_temp_path[768];
                        strncpy(old_temp_path, s_state.temp_path, sizeof(old_temp_path) - 1);
                        old_temp_path[sizeof(old_temp_path) - 1] = '\0';

                        plex_queue_prev(cfg);

                        {
                            const PlexTrack *t = plex_queue_current_track();
                            s_state.is_local_file = t ? (t->local_path[0] != '\0') : false;
                            if (s_state.is_local_file && t) {
                                strncpy(s_state.temp_path, t->local_path,
                                        sizeof(s_state.temp_path) - 1);
                                s_state.temp_path[sizeof(s_state.temp_path) - 1] = '\0';
                                s_state.ext[0] = '\0';
                            } else if (t) {
                                if (cfg->stream_bitrate_kbps > 0)
                                    strncpy(s_state.ext, "opus", sizeof(s_state.ext) - 1);
                                else
                                    extract_ext(t->media_key, s_state.ext, sizeof(s_state.ext));
                                s_state.ext[sizeof(s_state.ext) - 1] = '\0';
                                build_temp_path(s_state.ext, s_state.temp_path, sizeof(s_state.temp_path));
                            }
                        }

                        plex_art_clear();
                        {
                            const PlexTrack *t = plex_queue_current_track();
                            if (t) plex_art_fetch(cfg, t->thumb);
                            if (t) render_downloading(screen, t, 0);
                        }

                        if (s_state.download_pending) {
                            s_state.dl_ctx.should_cancel = true;
                            Player_setFileGrowing(false);
                            pthread_join(s_state.dl_thread, NULL);
                            s_state.dl_thread_running = false;
                            s_state.download_pending  = false;
                        }
                        Player_stop();
                        if (!old_is_local) remove(old_temp_path);

                        s_state.transcode = (cfg->stream_bitrate_kbps > 0);
                        load_next_track(queue, &screen_state);
                        dirty = 1;
                    }
                }
                left_armed = false;
            }

            /* BTN_RIGHT: tap = next track, hold = seek +10 s per repeat tick */
            if (PAD_justPressed(BTN_RIGHT)) {
                right_armed  = true;
                right_seeked = false;
            }
            if (PAD_justRepeated(BTN_RIGHT) && right_armed && !PAD_justPressed(BTN_RIGHT)) {
                Player_seek(Player_getPosition() + cfg->seek_interval_ms);
                right_seeked = true;
                dirty = 1;
            }
            if (PAD_justReleased(BTN_RIGHT) && right_armed) {
                if (!right_seeked) {
                    if (plex_queue_has_next()) {
                        {
                            const PlexTrack *t = plex_queue_current_track();
                            if (t) fire_scrobble(cfg, t->rating_key,
                                                 "stopped", Player_getPosition(),
                                                 Player_getDuration(), false);
                        }
                        Background_setActive(BG_NONE);

                        bool old_is_local = s_state.is_local_file;
                        char old_temp_path[768];
                        strncpy(old_temp_path, s_state.temp_path, sizeof(old_temp_path) - 1);
                        old_temp_path[sizeof(old_temp_path) - 1] = '\0';

                        plex_queue_next(cfg);

                        {
                            const PlexTrack *t = plex_queue_current_track();
                            s_state.is_local_file = t ? (t->local_path[0] != '\0') : false;
                            if (s_state.is_local_file && t) {
                                strncpy(s_state.temp_path, t->local_path,
                                        sizeof(s_state.temp_path) - 1);
                                s_state.temp_path[sizeof(s_state.temp_path) - 1] = '\0';
                                s_state.ext[0] = '\0';
                            } else if (t) {
                                if (cfg->stream_bitrate_kbps > 0)
                                    strncpy(s_state.ext, "opus", sizeof(s_state.ext) - 1);
                                else
                                    extract_ext(t->media_key, s_state.ext, sizeof(s_state.ext));
                                s_state.ext[sizeof(s_state.ext) - 1] = '\0';
                                build_temp_path(s_state.ext, s_state.temp_path, sizeof(s_state.temp_path));
                            }
                        }

                        plex_art_clear();
                        {
                            const PlexTrack *t = plex_queue_current_track();
                            if (t) plex_art_fetch(cfg, t->thumb);
                            if (t) render_downloading(screen, t, 0);
                        }

                        if (s_state.download_pending) {
                            s_state.dl_ctx.should_cancel = true;
                            Player_setFileGrowing(false);
                            pthread_join(s_state.dl_thread, NULL);
                            s_state.dl_thread_running = false;
                            s_state.download_pending  = false;
                        }
                        Player_stop();
                        if (!old_is_local) remove(old_temp_path);

                        s_state.transcode = (cfg->stream_bitrate_kbps > 0);
                        load_next_track(queue, &screen_state);
                        dirty = 1;
                    }
                }
                right_armed = false;
            }
            /* Seek ±30s */
            if (PAD_justPressed(BTN_UP)) {
                Player_seek(Player_getPosition() + 30000);
                dirty = 1;
            }
            if (PAD_justPressed(BTN_DOWN)) {
                Player_seek(Player_getPosition() - 30000);
                dirty = 1;
            }
        }

        /* ====================================================
         * ERROR state
         * ==================================================== */
        else if (screen_state == PLAYER_SCREEN_ERROR) {
            if (PAD_justPressed(BTN_A)) {
                /* Retry: restart download (or reload local file) for current track */
                plex_art_clear();
                {
                    const PlexTrack *t = plex_queue_current_track();
                    if (t) plex_art_fetch(cfg, t->thumb);
                    s_state.is_local_file = t ? (t->local_path[0] != '\0') : false;
                    if (s_state.is_local_file && t) {
                        strncpy(s_state.temp_path, t->local_path,
                                sizeof(s_state.temp_path) - 1);
                        s_state.temp_path[sizeof(s_state.temp_path) - 1] = '\0';
                        s_state.ext[0] = '\0';
                        Player_setFileGrowing(false);
                        if (Player_load(s_state.temp_path) == 0) {
                            Player_play();
                            s_state.scrobbled        = false;
                            s_state.last_timeline_ms = SDL_GetTicks();
                            screen_state             = PLAYER_SCREEN_PLAYING;
                        } else {
                            PLEX_LOG_ERROR("[Player] Player_load retry failed (offline): %s\n",
                                           s_state.temp_path);
                            /* stay in ERROR state */
                        }
                    } else if (t) {
                        if (s_state.transcode)
                            strncpy(s_state.ext, "opus", sizeof(s_state.ext) - 1);
                        else
                            extract_ext(t->media_key, s_state.ext, sizeof(s_state.ext));
                        s_state.ext[sizeof(s_state.ext) - 1] = '\0';
                        build_temp_path(s_state.ext, s_state.temp_path, sizeof(s_state.temp_path));
                        memset(&s_state.dl_ctx, 0, sizeof(s_state.dl_ctx));
                        start_download(&s_state.dl_ctx, &s_state.dl_thread, queue, s_state.temp_path);
                        s_state.dl_thread_running = true;
                        screen_state = PLAYER_SCREEN_DOWNLOADING;
                    }
                }
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                if (!s_state.is_local_file) remove(s_state.temp_path);
                Player_stop();
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_BROWSE;
            }
        }

render:
        /* ---- Render ---- */
        if (dirty) {
            const PlexTrack *render_t = plex_queue_current_track();
            if (screen_state == PLAYER_SCREEN_DOWNLOADING) {
                if (render_t) render_downloading(screen, render_t,
                                                 (int)s_state.dl_ctx.progress_pct);
            } else if (screen_state == PLAYER_SCREEN_PLAYING) {
                if (render_t) render_playing_screen(screen, render_t);
            } else {
                render_error(screen);
            }
            dirty = 0;
        }

        GFX_sync();
    }

    /* unreachable */
    Player_stop();
    return MODULE_BROWSE;
}

/* ------------------------------------------------------------------
 * Background tick — called once per frame from the browse module's
 * main loop while BG_MUSIC is active.  Must not block.
 * ------------------------------------------------------------------ */

void PlayerModule_backgroundTick(void)
{
    if (Background_getActive() != BG_MUSIC)
        return;

    /* Check if background download has finished */
    if (s_state.dl_thread_running && s_state.download_pending) {
        if (s_state.dl_ctx.download_done || s_state.dl_ctx.download_failed) {
            pthread_join(s_state.dl_thread, NULL);
            s_state.dl_thread_running = false;
            Player_setFileGrowing(false);
            s_state.download_pending = false;
        }
    }

    /* Scrobble timeline while playing in background */
    if (Player_getState() == PLAYER_STATE_PLAYING) {
        const PlexConfig *cfg = plex_config_get_mutable();
        PlexQueue        *queue = plex_queue_get();
        if (cfg && queue && queue->active && queue->count > 0) {
            const PlexTrack *t = plex_queue_current_track();
            if (t) {
                uint32_t now_ms = SDL_GetTicks();
                int      pos_ms = Player_getPosition();
                int      dur_ms = Player_getDuration();

                if (now_ms - s_state.last_timeline_ms >= TIMELINE_INTERVAL_MS) {
                    if (dur_ms > 0) {
                        fire_scrobble(cfg, t->rating_key,
                                      "playing", pos_ms, dur_ms, false);
                    }
                    s_state.last_timeline_ms = now_ms;
                }

                if (!s_state.scrobbled && dur_ms > 0 &&
                    (float)pos_ms / (float)dur_ms >= SCROBBLE_THRESHOLD) {
                    fire_scrobble(cfg, t->rating_key, NULL, 0, 0, true);
                    s_state.scrobbled = true;
                }
            }
        }
    }

    /* Auto-advance when track finishes and download is no longer pending */
    if (Player_getState() == PLAYER_STATE_STOPPED && !s_state.download_pending
            && !s_state.dl_thread_running) {
        const PlexConfig *cfg = plex_config_get_mutable();
        PlexQueue        *queue = plex_queue_get();

        if (!cfg || !queue || !queue->active || queue->count == 0) {
            Background_setActive(BG_NONE);
            return;
        }

        if (!s_state.is_local_file) remove(s_state.temp_path);

        if (queue->repeat_mode == REPEAT_ONE) {
            /* Reload current track in background */
            plex_art_clear();
            const PlexTrack *t = plex_queue_current_track();
            if (!t) { Background_setActive(BG_NONE); return; }
            plex_art_fetch(cfg, t->thumb);
            s_state.transcode = (cfg->stream_bitrate_kbps > 0);
            s_state.is_local_file = (t->local_path[0] != '\0');
            if (s_state.is_local_file) {
                strncpy(s_state.temp_path, t->local_path, sizeof(s_state.temp_path) - 1);
                s_state.temp_path[sizeof(s_state.temp_path) - 1] = '\0';
                s_state.ext[0] = '\0';
                Player_setFileGrowing(false);
                if (Player_load(s_state.temp_path) == 0) Player_play();
                else { Background_setActive(BG_NONE); return; }
                s_state.scrobbled = false;
                s_state.last_timeline_ms = SDL_GetTicks();
            } else {
                if (s_state.transcode)
                    strncpy(s_state.ext, "opus", sizeof(s_state.ext) - 1);
                else
                    extract_ext(t->media_key, s_state.ext, sizeof(s_state.ext));
                s_state.ext[sizeof(s_state.ext) - 1] = '\0';
                build_temp_path(s_state.ext, s_state.temp_path, sizeof(s_state.temp_path));
                memset(&s_state.dl_ctx, 0, sizeof(s_state.dl_ctx));
                start_download(&s_state.dl_ctx, &s_state.dl_thread, queue, s_state.temp_path);
                s_state.dl_thread_running = true;
                s_state.scrobbled = false;
                s_state.last_timeline_ms = SDL_GetTicks();
            }
            return;
        }

        if (plex_queue_has_next()) {
            plex_queue_next(cfg);

            plex_art_clear();
            {
                const PlexTrack *t = plex_queue_current_track();
                if (t) plex_art_fetch(cfg, t->thumb);
            }

            s_state.transcode = (cfg->stream_bitrate_kbps > 0);
            {
                const PlexTrack *t = plex_queue_current_track();
                s_state.is_local_file = t ? (t->local_path[0] != '\0') : false;

                if (s_state.is_local_file && t) {
                    strncpy(s_state.temp_path, t->local_path,
                            sizeof(s_state.temp_path) - 1);
                    s_state.temp_path[sizeof(s_state.temp_path) - 1] = '\0';
                    s_state.ext[0] = '\0';
                    Player_setFileGrowing(false);
                    if (Player_load(s_state.temp_path) == 0) {
                        Player_play();
                    } else {
                        PLEX_LOG_ERROR("[Player] BG auto-advance: Player_load failed (offline): %s\n",
                                       s_state.temp_path);
                        Background_setActive(BG_NONE);
                        return;
                    }
                    s_state.scrobbled        = false;
                    s_state.last_timeline_ms = SDL_GetTicks();
                } else if (t) {
                    if (s_state.transcode)
                        strncpy(s_state.ext, "opus", sizeof(s_state.ext) - 1);
                    else
                        extract_ext(t->media_key, s_state.ext, sizeof(s_state.ext));
                    s_state.ext[sizeof(s_state.ext) - 1] = '\0';
                    build_temp_path(s_state.ext, s_state.temp_path, sizeof(s_state.temp_path));

                    memset(&s_state.dl_ctx, 0, sizeof(s_state.dl_ctx));
                    start_download(&s_state.dl_ctx, &s_state.dl_thread, queue, s_state.temp_path);
                    s_state.dl_thread_running = true;
                    s_state.scrobbled         = false;
                    s_state.last_timeline_ms  = SDL_GetTicks();
                }
            }
        } else {
            /* End of queue */
            Background_setActive(BG_NONE);
        }
    }

    /* Progressive playback start in background: watch for prebuffer threshold
     * or full download completion while nothing is playing yet. */
    if (s_state.dl_thread_running && !s_state.download_pending &&
        Player_getState() == PLAYER_STATE_STOPPED) {

        if (s_state.dl_ctx.download_done) {
            /* Full download finished — load and play */
            pthread_join(s_state.dl_thread, NULL);
            s_state.dl_thread_running = false;
            Player_setFileGrowing(false);

            if (Player_load(s_state.temp_path) == 0) {
                if (s_state.transcode) {
                    const PlexTrack *t = plex_queue_current_track();
                    int dur = t ? t->duration_ms : 0;
                    if (dur > 0) Player_setTotalFrames((int64_t)((dur / 1000.0) * 48000.0));
                }
                Player_play();
                s_state.scrobbled        = false;
                s_state.last_timeline_ms = SDL_GetTicks();
            } else {
                PLEX_LOG_ERROR("[Player] BG: Player_load failed after full download: %s\n",
                               s_state.temp_path);
                if (!s_state.is_local_file) remove(s_state.temp_path);
                Background_setActive(BG_NONE);
                return;
            }
        } else if (s_state.dl_ctx.download_failed) {
            pthread_join(s_state.dl_thread, NULL);
            s_state.dl_thread_running = false;
            PLEX_LOG_ERROR("[Player] BG: download failed for background track\n");
            Background_setActive(BG_NONE);
        } else if (strcasecmp(s_state.ext, "mp3") == 0
                   || strcasecmp(s_state.ext, "flac") == 0
                   || strcasecmp(s_state.ext, "wav") == 0
                   || strcasecmp(s_state.ext, "m4a") == 0
                   || strcasecmp(s_state.ext, "opus") == 0) {
            /* Try progressive start once prebuffer threshold is reached */
            struct stat st;
            if (stat(s_state.temp_path, &st) == 0 && st.st_size >= PREBUFFER_BYTES) {
                Player_stop();
                if (Player_load(s_state.temp_path) == 0) {
                    if (s_state.transcode) {
                        const PlexTrack *t = plex_queue_current_track();
                        int dur = t ? t->duration_ms : 0;
                        if (dur > 0) Player_setTotalFrames((int64_t)((dur / 1000.0) * 48000.0));
                    }
                    Player_setFileGrowing(true);
                    Player_play();
                    s_state.download_pending = true;
                    s_state.scrobbled        = false;
                    s_state.last_timeline_ms = SDL_GetTicks();
                }
                /* If Player_load failed, wait for full download to complete */
            }
        }
    }
}
