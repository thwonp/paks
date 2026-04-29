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

static void render_cover_art(SDL_Surface *screen, SDL_Surface *art)
{
    if (!art) return;
    int x = (screen->w - COVER_SIZE) / 2;
    int y = PADDING;
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
    render_cover_art(screen, art);

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

static void render_playing_screen(SDL_Surface *screen,
                                   const PlexTrack *track)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    SDL_Surface *art = plex_art_get();
    render_cover_art(screen, art);

    int text_y = COVER_SIZE + PADDING * 2;

    SDL_Color white = COLOR_WHITE;
    SDL_Color gray  = { 0xAA, 0xAA, 0xAA, 0xFF };

    TTF_Font *title_font = is_brick ? Fonts_getLarge() : Fonts_getTitle();
    render_text_centered(screen, title_font,        white, track->title,  text_y);
    text_y += TTF_FontHeight(title_font) + SCALE1(4);
    render_text_centered(screen, Fonts_getArtist(), gray,  track->artist, text_y);
    text_y += TTF_FontHeight(Fonts_getArtist()) + SCALE1(4);

    render_text_centered(screen, Fonts_getAlbum(), gray, track->album, text_y);
    text_y += TTF_FontHeight(Fonts_getAlbum()) + PADDING;

    /* Playback progress bar */
    int pos_ms = Player_getPosition();
    int dur_ms = Player_getDuration();
    float frac = (dur_ms > 0) ? ((float)pos_ms / (float)dur_ms) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    uint32_t bg_col = SDL_MapRGB(screen->format, 0x40, 0x40, 0x40);
    uint32_t fg_col = SDL_MapRGB(screen->format, 0x22, 0x88, 0xFF);
    render_progress_bar(screen, text_y, frac, bg_col, fg_col);
    text_y += PROGRESS_H + SCALE1(4);

    /* M:SS / M:SS */
    char pos_str[16], dur_str[16], time_label[40];
    format_time(pos_str, pos_ms);
    format_time(dur_str, dur_ms);
    snprintf(time_label, sizeof(time_label), "%s / %s", pos_str, dur_str);
    render_text_centered(screen, Fonts_getSmall(), gray, time_label, text_y);
    text_y += TTF_FontHeight(Fonts_getSmall()) + PADDING;

    /* Button hints at bottom (convey pause state instead of a separate indicator) */
    PlayerState ps = Player_getState();
    int hint_y = screen->h - TTF_FontHeight(Fonts_getSmall()) - PADDING;
    const char *hints = (ps == PLAYER_STATE_PAUSED)
        ? "[A] Play   [<] Prev  [>] Next  [B] Back"
        : "[A] Pause  [<] Prev  [>] Next  [B] Back";
    render_text_centered(screen, Fonts_getSmall(), gray, hints, hint_y);

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
 * has already been called).  Updates is_local_file, ext, temp_path,
 * starts a download or plays the local file directly.
 * Caller must have stopped/joined any previous download and called
 * Player_stop() before invoking this.
 * ------------------------------------------------------------------ */

static void load_next_track(PlexQueue *queue, bool *is_local_file,
                             char *ext, int ext_size,
                             char *temp_path, int temp_path_size,
                             DownloadCtx *dl_ctx, pthread_t *dl_thread,
                             bool *download_pending, bool *scrobbled,
                             uint32_t *last_timeline_ms,
                             PlayerScreenState *screen_state,
                             int transcode)
{
    *is_local_file = (queue->tracks[queue->current_index].local_path[0] != '\0');

    if (*is_local_file) {
        strncpy(temp_path, queue->tracks[queue->current_index].local_path,
                temp_path_size - 1);
        temp_path[temp_path_size - 1] = '\0';
        ext[0] = '\0';
    } else {
        if (transcode)
            strncpy(ext, "opus", ext_size - 1);
        else
            extract_ext(queue->tracks[queue->current_index].media_key, ext, ext_size);
        ext[ext_size - 1] = '\0';
        build_temp_path(ext, temp_path, temp_path_size);
    }

    *download_pending = false;

    if (*is_local_file) {
        Player_setFileGrowing(false);
        if (Player_load(temp_path) == 0) {
            Player_play();
            *screen_state = PLAYER_SCREEN_PLAYING;
        } else {
            PLEX_LOG_ERROR("[Player] Player_load failed (offline): %s\n", temp_path);
            *screen_state = PLAYER_SCREEN_ERROR;
        }
    } else {
        start_download(dl_ctx, dl_thread, queue, temp_path);
        *screen_state = PLAYER_SCREEN_DOWNLOADING;
    }

    *scrobbled        = false;
    *last_timeline_ms = SDL_GetTicks();
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

    /* Build temp file path (or use local path for offline tracks) */
    char ext[16];
    char temp_path[768];
    bool is_local_file = (queue->tracks[queue->current_index].local_path[0] != '\0');
    if (is_local_file) {
        strncpy(temp_path, queue->tracks[queue->current_index].local_path,
                sizeof(temp_path) - 1);
        temp_path[sizeof(temp_path) - 1] = '\0';
        ext[0] = '\0';
    } else {
        if (cfg->stream_bitrate_kbps > 0)
            strncpy(ext, "opus", sizeof(ext) - 1);
        else
            extract_ext(queue->tracks[queue->current_index].media_key, ext, sizeof(ext));
        ext[sizeof(ext) - 1] = '\0';
        build_temp_path(ext, temp_path, sizeof(temp_path));
    }

    PlayerScreenState screen_state = PLAYER_SCREEN_DOWNLOADING;

    /* Scrobble tracking */
    bool     scrobbled          = false;
    uint32_t last_timeline_ms   = 0;

    /* Progressive playback: true while playing a file that's still downloading */
    bool download_pending = false;

    /* Re-entry with background active: skip download, resume UI */
    if (Background_getActive() == BG_MUSIC && Player_getState() != PLAYER_STATE_STOPPED) {
        screen_state     = PLAYER_SCREEN_PLAYING;
        scrobbled        = false;
        last_timeline_ms = SDL_GetTicks();
        plex_art_fetch(cfg, queue->tracks[queue->current_index].thumb);
        /* fall through to main loop with PLAYER_SCREEN_PLAYING */
    } else {
        /* Normal entry: fetch cover art and start download.
         * Clear any stale BG_MUSIC state immediately so that if the user
         * cancels the download (B-press) or hits an error, browse will not
         * show a stale "Now Playing" item. */
        Background_setActive(BG_NONE);
        plex_art_fetch(cfg, queue->tracks[queue->current_index].thumb);
    }

    int           dirty = 1;
    int           show_setting = 0;
    pthread_t     dl_thread;
    DownloadCtx   dl_ctx;
    memset(&dl_ctx, 0, sizeof(dl_ctx));
    if (screen_state == PLAYER_SCREEN_DOWNLOADING) {
        if (is_local_file) {
            PLEX_LOG("[Player] Loading offline file: %s\n", temp_path);
            Player_setFileGrowing(false);
            if (Player_load(temp_path) == 0) {
                Player_play();
                scrobbled        = false;
                last_timeline_ms = SDL_GetTicks();
                screen_state     = PLAYER_SCREEN_PLAYING;
            } else {
                PLEX_LOG_ERROR("[Player] Player_load failed (offline): %s\n", temp_path);
                screen_state = PLAYER_SCREEN_ERROR;
            }
        } else {
            start_download(&dl_ctx, &dl_thread, queue, temp_path);
        }
    }

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
            if (PAD_isPressed(BTN_MENU) && PAD_justPressed(BTN_SELECT)) {
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

        /* ---- Art async update ---- */
        if (plex_art_is_fetching()) dirty = 1;

        /* ====================================================
         * DOWNLOADING state
         * ==================================================== */
        if (screen_state == PLAYER_SCREEN_DOWNLOADING) {

            /* Check for thread completion */
            if (dl_ctx.download_done) {
                pthread_join(dl_thread, NULL);
                download_pending = false;

                /* Load and play */
                Player_stop();
                if (Player_load(temp_path) == 0) {
                    if (cfg->stream_bitrate_kbps > 0) {
                        int dur = queue->tracks[queue->current_index].duration_ms;
                        Player_setTotalFrames((int64_t)((dur / 1000.0) * 48000.0));
                    }
                    Player_play();
                    scrobbled        = false;
                    last_timeline_ms = SDL_GetTicks();
                    screen_state     = PLAYER_SCREEN_PLAYING;
                    dirty            = 1;
                } else {
                    if (!is_local_file) remove(temp_path);
                    PLEX_LOG_ERROR("[Player] Player_load failed: %s\n", temp_path);
                    screen_state = PLAYER_SCREEN_ERROR;
                    dirty        = 1;
                }
                goto render;
            }

            if (dl_ctx.download_failed) {
                pthread_join(dl_thread, NULL);
                screen_state = PLAYER_SCREEN_ERROR;
                dirty        = 1;
                goto render;
            }

            /* Try to start playback early once enough bytes are on disk.
             * MP3/FLAC/WAV/M4A: total_frames is header-derived, safe on partial files.
             * Opus (transcoded): total_frames is overridden via Player_setTotalFrames using
             * Plex-provided duration, so it is also safe for early-start. */
            if (!download_pending
                    && (strcasecmp(ext, "mp3") == 0
                        || strcasecmp(ext, "flac") == 0
                        || strcasecmp(ext, "wav") == 0
                        || strcasecmp(ext, "m4a") == 0
                        || strcasecmp(ext, "opus") == 0)) {
                struct stat st;
                if (stat(temp_path, &st) == 0 && st.st_size >= PREBUFFER_BYTES) {
                    Player_stop();
                    if (Player_load(temp_path) == 0) {
                        if (cfg->stream_bitrate_kbps > 0) {
                            int dur = queue->tracks[queue->current_index].duration_ms;
                            Player_setTotalFrames((int64_t)((dur / 1000.0) * 48000.0));
                        }
                        Player_setFileGrowing(true);
                        Player_play();
                        download_pending    = true;
                        scrobbled           = false;
                        last_timeline_ms    = SDL_GetTicks();
                        screen_state        = PLAYER_SCREEN_PLAYING;
                        dirty               = 1;
                        goto render;
                    }
                    /* Player_load failed (e.g. M4A moov at end) — fall through to full download */
                }
            }

            dirty = 1;  /* keep updating progress bar */

            /* Input during download */
            if (PAD_justPressed(BTN_B)) {
                if (!is_local_file) {
                    dl_ctx.should_cancel = true;
                    pthread_join(dl_thread, NULL);
                    remove(temp_path);
                }
                Player_stop();
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_BROWSE;
            }
            /* Start long-press to quit */
            if (PAD_justPressed(BTN_START)) {
                if (!is_local_file) {
                    dl_ctx.should_cancel = true;
                    pthread_join(dl_thread, NULL);
                    remove(temp_path);
                }
                Player_stop();
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_QUIT;
            }
        }

        /* ====================================================
         * PLAYING state
         * ==================================================== */
        else if (screen_state == PLAYER_SCREEN_PLAYING) {
            /* Background download finished while we were already playing */
            if (download_pending) {
                if (dl_ctx.download_done) {
                    pthread_join(dl_thread, NULL);
                    Player_setFileGrowing(false);
                    download_pending = false;
                } else if (dl_ctx.download_failed) {
                    pthread_join(dl_thread, NULL);
                    Player_setFileGrowing(false);
                    download_pending = false;
                    /* Track may stop early if download failed; user can press Next */
                }
            }

            Player_update();

            /* Auto-advance on track end */
            if (Player_getState() == PLAYER_STATE_STOPPED) {
                Background_setActive(BG_NONE);
                if (download_pending) {
                    dl_ctx.should_cancel = true;
                    Player_setFileGrowing(false);
                    pthread_join(dl_thread, NULL);
                    download_pending = false;
                }
                Player_stop();
                if (!is_local_file) remove(temp_path);

                if (plex_queue_has_next()) {
                    plex_queue_next(cfg);

                    plex_art_clear();
                    plex_art_fetch(cfg, queue->tracks[queue->current_index].thumb);

                    load_next_track(queue, &is_local_file,
                                    ext, sizeof(ext),
                                    temp_path, sizeof(temp_path),
                                    &dl_ctx, &dl_thread,
                                    &download_pending, &scrobbled,
                                    &last_timeline_ms, &screen_state,
                                    cfg->stream_bitrate_kbps > 0);
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
                if (now_ms - last_timeline_ms >= TIMELINE_INTERVAL_MS) {
                    if (dur_ms > 0) {
                        fire_scrobble(cfg,
                                      queue->tracks[queue->current_index].rating_key,
                                      "playing", pos_ms, dur_ms, false);
                    }
                    last_timeline_ms = now_ms;
                }

                if (!scrobbled && dur_ms > 0 &&
                    (float)pos_ms / (float)dur_ms >= SCROBBLE_THRESHOLD) {
                    fire_scrobble(cfg,
                                  queue->tracks[queue->current_index].rating_key,
                                  NULL, 0, 0, true);
                    scrobbled = true;
                }
            }

            dirty = 1;  /* keep time display fresh */

            /* Input during playback */
            if (PAD_justPressed(BTN_A)) {
                Player_togglePause();
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                if (download_pending) {
                    dl_ctx.should_cancel = true;
                    Player_setFileGrowing(false);
                    pthread_join(dl_thread, NULL);
                    download_pending = false;
                }
                /* Keep playing in background, return to browse */
                Background_setActive(BG_MUSIC);
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_BROWSE;
            }
            else if (PAD_justPressed(BTN_START)) {
                /* Full quit — stop playback and clean up */
                fire_scrobble(cfg,
                              queue->tracks[queue->current_index].rating_key,
                              "stopped", Player_getPosition(),
                              Player_getDuration(), false);
                Background_setActive(BG_NONE);
                if (download_pending) {
                    dl_ctx.should_cancel = true;
                    Player_setFileGrowing(false);
                    pthread_join(dl_thread, NULL);
                    download_pending = false;
                }
                Player_stop();
                if (!is_local_file) remove(temp_path);
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_QUIT;
            }
            else if (PAD_justPressed(BTN_LEFT) || PAD_justPressed(BTN_L1)) {
                if (plex_queue_has_prev()) {
                    fire_scrobble(cfg,
                                  queue->tracks[queue->current_index].rating_key,
                                  "stopped", Player_getPosition(),
                                  Player_getDuration(), false);
                    Background_setActive(BG_NONE);

                    /* Capture old path info before advancing queue. */
                    bool old_is_local = is_local_file;
                    char old_temp_path[768];
                    strncpy(old_temp_path, temp_path, sizeof(old_temp_path) - 1);
                    old_temp_path[sizeof(old_temp_path) - 1] = '\0';

                    /* Advance queue and build new paths so we can render first. */
                    plex_queue_prev(cfg);

                    /* Update is_local_file and temp_path for new track temporarily
                     * so the render call shows the correct title. */
                    is_local_file = (queue->tracks[queue->current_index].local_path[0] != '\0');
                    if (is_local_file) {
                        strncpy(temp_path,
                                queue->tracks[queue->current_index].local_path,
                                sizeof(temp_path) - 1);
                        temp_path[sizeof(temp_path) - 1] = '\0';
                        ext[0] = '\0';
                    } else {
                        if (cfg->stream_bitrate_kbps > 0)
                            strncpy(ext, "opus", sizeof(ext) - 1);
                        else
                            extract_ext(queue->tracks[queue->current_index].media_key,
                                        ext, sizeof(ext));
                        ext[sizeof(ext) - 1] = '\0';
                        build_temp_path(ext, temp_path, sizeof(temp_path));
                    }

                    plex_art_clear();
                    plex_art_fetch(cfg, queue->tracks[queue->current_index].thumb);

                    /* Render the new track's downloading/loading screen immediately. */
                    render_downloading(screen,
                                       &queue->tracks[queue->current_index], 0);

                    /* Now do the blocking stop/join behind the visible frame. */
                    if (download_pending) {
                        dl_ctx.should_cancel = true;
                        Player_setFileGrowing(false);
                        pthread_join(dl_thread, NULL);
                        download_pending = false;
                    }
                    Player_stop();
                    if (!old_is_local) remove(old_temp_path);

                    load_next_track(queue, &is_local_file,
                                    ext, sizeof(ext),
                                    temp_path, sizeof(temp_path),
                                    &dl_ctx, &dl_thread,
                                    &download_pending, &scrobbled,
                                    &last_timeline_ms, &screen_state,
                                    cfg->stream_bitrate_kbps > 0);
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_RIGHT) || PAD_justPressed(BTN_R1)) {
                if (plex_queue_has_next()) {
                    fire_scrobble(cfg,
                                  queue->tracks[queue->current_index].rating_key,
                                  "stopped", Player_getPosition(),
                                  Player_getDuration(), false);
                    Background_setActive(BG_NONE);

                    /* Capture old path info before advancing queue. */
                    bool old_is_local = is_local_file;
                    char old_temp_path[768];
                    strncpy(old_temp_path, temp_path, sizeof(old_temp_path) - 1);
                    old_temp_path[sizeof(old_temp_path) - 1] = '\0';

                    /* Advance queue and build new paths so we can render first. */
                    plex_queue_next(cfg);

                    /* Update is_local_file and temp_path for new track temporarily
                     * so the render call shows the correct title. */
                    is_local_file = (queue->tracks[queue->current_index].local_path[0] != '\0');
                    if (is_local_file) {
                        strncpy(temp_path,
                                queue->tracks[queue->current_index].local_path,
                                sizeof(temp_path) - 1);
                        temp_path[sizeof(temp_path) - 1] = '\0';
                        ext[0] = '\0';
                    } else {
                        if (cfg->stream_bitrate_kbps > 0)
                            strncpy(ext, "opus", sizeof(ext) - 1);
                        else
                            extract_ext(queue->tracks[queue->current_index].media_key,
                                        ext, sizeof(ext));
                        ext[sizeof(ext) - 1] = '\0';
                        build_temp_path(ext, temp_path, sizeof(temp_path));
                    }

                    plex_art_clear();
                    plex_art_fetch(cfg, queue->tracks[queue->current_index].thumb);

                    /* Render the new track's downloading/loading screen immediately. */
                    render_downloading(screen,
                                       &queue->tracks[queue->current_index], 0);

                    /* Now do the blocking stop/join behind the visible frame. */
                    if (download_pending) {
                        dl_ctx.should_cancel = true;
                        Player_setFileGrowing(false);
                        pthread_join(dl_thread, NULL);
                        download_pending = false;
                    }
                    Player_stop();
                    if (!old_is_local) remove(old_temp_path);

                    load_next_track(queue, &is_local_file,
                                    ext, sizeof(ext),
                                    temp_path, sizeof(temp_path),
                                    &dl_ctx, &dl_thread,
                                    &download_pending, &scrobbled,
                                    &last_timeline_ms, &screen_state,
                                    cfg->stream_bitrate_kbps > 0);
                    dirty = 1;
                }
            }
            /* Bonus: seek ±30s (Player_seek() exists in player.h) */
            else if (PAD_justPressed(BTN_UP)) {
                Player_seek(Player_getPosition() + 30000);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_DOWN)) {
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
                plex_art_fetch(cfg, queue->tracks[queue->current_index].thumb);
                is_local_file = (queue->tracks[queue->current_index].local_path[0] != '\0');
                if (is_local_file) {
                    strncpy(temp_path, queue->tracks[queue->current_index].local_path,
                            sizeof(temp_path) - 1);
                    temp_path[sizeof(temp_path) - 1] = '\0';
                    ext[0] = '\0';
                    Player_setFileGrowing(false);
                    if (Player_load(temp_path) == 0) {
                        Player_play();
                        scrobbled        = false;
                        last_timeline_ms = SDL_GetTicks();
                        screen_state     = PLAYER_SCREEN_PLAYING;
                    } else {
                        PLEX_LOG_ERROR("[Player] Player_load retry failed (offline): %s\n",
                                       temp_path);
                        /* stay in ERROR state */
                    }
                } else {
                    if (cfg->stream_bitrate_kbps > 0)
                        strncpy(ext, "opus", sizeof(ext) - 1);
                    else
                        extract_ext(queue->tracks[queue->current_index].media_key,
                                    ext, sizeof(ext));
                    ext[sizeof(ext) - 1] = '\0';
                    build_temp_path(ext, temp_path, sizeof(temp_path));
                    start_download(&dl_ctx, &dl_thread, queue, temp_path);
                    screen_state = PLAYER_SCREEN_DOWNLOADING;
                }
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                if (!is_local_file) remove(temp_path);
                Player_stop();
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_BROWSE;
            }
        }

render:
        /* ---- Render ---- */
        if (dirty) {
            if (screen_state == PLAYER_SCREEN_DOWNLOADING) {
                render_downloading(screen,
                                   &queue->tracks[queue->current_index],
                                   (int)dl_ctx.progress_pct);
            } else if (screen_state == PLAYER_SCREEN_PLAYING) {
                render_playing_screen(screen,
                                      &queue->tracks[queue->current_index]);
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
