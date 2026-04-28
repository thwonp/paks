#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>

#include "api.h"
#include "plex_log.h"
#include "background.h"
#include "defines.h"
#include "module_browse.h"
#include "module_settings.h"
#include "module_common.h"
#include "plex_api.h"
#include "plex_art.h"
#include "plex_config.h"
#include "plex_models.h"
#include "plex_queue.h"
#include "plex_downloads.h"
#include "ui_fonts.h"
#include "ui_utils.h"

/* =========================================================================
 * Internal state
 * ========================================================================= */

typedef enum {
    BROWSE_LIBRARIES,
    BROWSE_ARTISTS,
    BROWSE_ALBUMS,
    BROWSE_TRACKS,
    BROWSE_NET_ERROR,
    BROWSE_ARTISTS_PAGE,   /* append-only artist page load; NOT a visible state */
} BrowseState;

typedef enum {
    LOAD_IDLE    = 0,  /* not started or reset */
    LOAD_RUNNING = 1,  /* thread in flight */
    LOAD_DONE    = 2,  /* thread finished, results not yet collected */
    LOAD_READY   = 3,  /* results collected, normal display active */
    LOAD_ERROR   = 4,  /* network error */
} LoadState;

/* Width fraction for the list panel (70%) */
#define LIST_PANEL_FRAC 70

/* Right panel art padding */
#define ART_PANEL_PAD SCALE1(8)

/* How close the cursor must be to the end of loaded artists to trigger a page fetch */
#define ARTIST_PAGE_LOOKAHEAD 5

/* =========================================================================
 * Async worker
 * ========================================================================= */

static struct {
    pthread_t        thread;
    pthread_mutex_t  lock;           /* protects status field */
    LoadState        status;         /* current worker status */
    volatile bool    cancel;         /* set by main thread to discard result */
    bool             thread_started; /* true if thread created and not yet joined */
    /* discriminator */
    BrowseState      type;
    /* cfg copy (main thread writes before kick; worker reads once at start) */
    char             server_url[PLEX_MAX_URL];
    char             token[PLEX_MAX_STR];
    /* inputs (only the relevant one is used per type) */
    int              library_id;
    int              artist_rating_key;
    int              album_rating_key;
    int              artist_offset;   /* offset for BROWSE_ARTISTS_PAGE worker */
    /* output pointers — point to module static arrays; worker writes, main reads after DONE */
    PlexLibrary     *libs;
    int             *lib_count;
    PlexArtist      *artists;
    PlexPage         artists_page;   /* written by worker, read by main on DONE */
    PlexAlbum       *albums;
    int             *album_count;
    PlexTrack       *tracks;
    int             *track_count;
} s_load = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void *browse_load_worker(void *arg)
{
    (void)arg;
    /* Copy cfg fields off the shared struct before releasing the lock */
    PlexConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    pthread_mutex_lock(&s_load.lock);
    strncpy(cfg.server_url, s_load.server_url, sizeof(cfg.server_url) - 1);
    strncpy(cfg.token,      s_load.token,      sizeof(cfg.token) - 1);
    BrowseState type = s_load.type;
    pthread_mutex_unlock(&s_load.lock);

    int rc = 0;
    switch (type) {
        case BROWSE_LIBRARIES:
            rc = plex_api_get_libraries(&cfg, s_load.libs, s_load.lib_count);
            break;
        case BROWSE_ARTISTS: {
            PlexPage page;
            memset(&page, 0, sizeof(page));
            rc = plex_api_get_artists(&cfg, s_load.library_id, 0, PLEX_MAX_ITEMS,
                                      s_load.artists, &page);
            pthread_mutex_lock(&s_load.lock);
            s_load.artists_page = page;
            pthread_mutex_unlock(&s_load.lock);
            break;
        }
        case BROWSE_ALBUMS:
            rc = plex_api_get_albums(&cfg, s_load.artist_rating_key,
                                     s_load.albums, s_load.album_count);
            break;
        case BROWSE_TRACKS:
            rc = plex_api_get_tracks(&cfg, s_load.album_rating_key,
                                     s_load.tracks, s_load.track_count);
            break;
        case BROWSE_ARTISTS_PAGE: {
            PlexPage page;
            memset(&page, 0, sizeof(page));
            int offset = s_load.artist_offset;
            int cap    = PLEX_MAX_ITEMS - offset;
            if (cap > 0) {
                rc = plex_api_get_artists(&cfg, s_load.library_id, offset, cap,
                                          s_load.artists + offset, &page);
            }
            pthread_mutex_lock(&s_load.lock);
            s_load.artists_page = page;
            pthread_mutex_unlock(&s_load.lock);
            break;
        }
        default:
            rc = -1;
    }

    pthread_mutex_lock(&s_load.lock);
    if (s_load.cancel || rc != 0)
        s_load.status = LOAD_ERROR;
    else
        s_load.status = LOAD_DONE;
    pthread_mutex_unlock(&s_load.lock);

    return NULL;
}

/* Populate s_load, (re-)join any existing thread, spawn a new one. */
static void browse_load_kick(BrowseState type, const PlexConfig *cfg,
                             int library_id, int artist_rating_key, int album_rating_key,
                             PlexLibrary *libs, int *lib_count,
                             PlexArtist *artists,
                             PlexAlbum *albums, int *album_count,
                             PlexTrack *tracks, int *track_count)
{
    /* Join any previous thread (handles cancelled loads that have since finished) */
    if (s_load.thread_started) {
        pthread_join(s_load.thread, NULL);
        s_load.thread_started = false;
    }

    pthread_mutex_lock(&s_load.lock);
    s_load.cancel         = false;
    s_load.type           = type;
    strncpy(s_load.server_url, cfg->server_url, sizeof(s_load.server_url) - 1);
    strncpy(s_load.token,      cfg->token,      sizeof(s_load.token) - 1);
    s_load.server_url[sizeof(s_load.server_url) - 1] = '\0';
    s_load.token[sizeof(s_load.token) - 1]           = '\0';
    s_load.library_id        = library_id;
    s_load.artist_rating_key = artist_rating_key;
    s_load.album_rating_key  = album_rating_key;
    s_load.libs        = libs;       s_load.lib_count   = lib_count;
    s_load.artists     = artists;
    s_load.albums      = albums;     s_load.album_count = album_count;
    s_load.tracks      = tracks;     s_load.track_count = track_count;
    s_load.status      = LOAD_RUNNING;
    pthread_mutex_unlock(&s_load.lock);

    if (pthread_create(&s_load.thread, NULL, browse_load_worker, NULL) == 0) {
        s_load.thread_started = true;
    } else {
        s_load.thread_started = false;
        pthread_mutex_lock(&s_load.lock);
        s_load.status = LOAD_ERROR;
        pthread_mutex_unlock(&s_load.lock);
    }
}

/* Read current worker status (thread-safe). */
static LoadState browse_load_poll(void)
{
    pthread_mutex_lock(&s_load.lock);
    LoadState st = s_load.status;
    pthread_mutex_unlock(&s_load.lock);
    return st;
}

/* Join the worker thread (call after seeing DONE or ERROR). */
static void browse_load_join(void)
{
    if (s_load.thread_started) {
        pthread_join(s_load.thread, NULL);
        s_load.thread_started = false;
    }
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void format_duration(int ms, char *buf, int size)
{
    int total_sec = ms / 1000;
    int minutes   = total_sec / 60;
    int seconds   = total_sec % 60;
    snprintf(buf, size, "%d:%02d", minutes, seconds);
}

static void render_loading(SDL_Surface *screen, bool show_cancel)
{
    static const char *labels[] = {"Loading", "Loading.", "Loading..", "Loading..."};
    int phase = (SDL_GetTicks() / 500) % 4;
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
    int hw = screen->w, hh = screen->h;
    SDL_Surface *msg = TTF_RenderUTF8_Blended(Fonts_getMedium(), labels[phase], COLOR_WHITE);
    if (msg) {
        SDL_BlitSurface(msg, NULL, screen,
                        &(SDL_Rect){(hw - msg->w) / 2, (hh - msg->h) / 2});
        SDL_FreeSurface(msg);
    }
    if (show_cancel)
        GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
    GFX_flip(screen);
}

static void render_net_error(SDL_Surface *screen)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
    int hw = screen->w;
    int hh = screen->h;
    SDL_Surface *msg = TTF_RenderUTF8_Blended(Fonts_getMedium(), "Network error.",
                                               (SDL_Color){0xE5, 0x45, 0x45, 0xFF});
    if (msg) {
        SDL_BlitSurface(msg, NULL, screen,
                        &(SDL_Rect){(hw - msg->w) / 2, hh / 2 - SCALE1(20)});
        SDL_FreeSurface(msg);
    }
    SDL_Surface *hint = TTF_RenderUTF8_Blended(Fonts_getSmall(),
                                                "Press A to retry, B to go back",
                                                COLOR_LIGHT_TEXT);
    if (hint) {
        SDL_BlitSurface(hint, NULL, screen,
                        &(SDL_Rect){(hw - hint->w) / 2, hh / 2 + SCALE1(10)});
        SDL_FreeSurface(hint);
    }
    GFX_blitButtonGroup((char*[]){"A", "RETRY", "B", "BACK", NULL}, 1, screen, 1);
    GFX_flip(screen);
}

/*
 * Render cover art into the right panel area.
 * art_surface may be NULL (shows empty panel).
 */
static void render_art_panel(SDL_Surface *screen, SDL_Surface *art,
                              const char *title_line1, const char *title_line2)
{
    int hw = screen->w;
    int hh = screen->h;
    int panel_x = (hw * LIST_PANEL_FRAC) / 100;
    int panel_w = hw - panel_x;
    int text_y;
    int text_max_w;

    /* Dark panel background */
    SDL_FillRect(screen,
                 &(SDL_Rect){panel_x, 0, panel_w, hh},
                 SDL_MapRGB(screen->format, 0x1a, 0x1a, 0x1a));

    if (art) {
        /* Scale art to fit within the panel */
        int max_dim = panel_w - ART_PANEL_PAD * 2;
        if (max_dim > hh / 2) max_dim = hh / 2;
        if (max_dim > 0) {
            SDL_Surface *scaled = SDL_CreateRGBSurface(0, max_dim, max_dim, 32,
                                                        art->format->Rmask,
                                                        art->format->Gmask,
                                                        art->format->Bmask,
                                                        art->format->Amask);
            if (scaled) {
                SDL_Rect dst = {0, 0, max_dim, max_dim};
                SDL_BlitScaled(art, NULL, scaled, &dst);
                int art_x = panel_x + (panel_w - max_dim) / 2;
                int art_y = ART_PANEL_PAD;
                SDL_BlitSurface(scaled, NULL, screen, &(SDL_Rect){art_x, art_y});
                SDL_FreeSurface(scaled);
            }
        }
        text_y = hh / 2 + ART_PANEL_PAD;
    } else {
        text_y = hh / 3;
    }

    text_max_w = panel_w - ART_PANEL_PAD * 2;

    if (title_line1 && title_line1[0]) {
        SDL_Surface *t = TTF_RenderUTF8_Blended(Fonts_getMedium(), title_line1, COLOR_WHITE);
        if (t) {
            int tx = panel_x + (panel_w - (t->w < text_max_w ? t->w : text_max_w)) / 2;
            SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){tx, text_y});
            text_y += t->h + SCALE1(4);
            SDL_FreeSurface(t);
        }
    }
    if (title_line2 && title_line2[0]) {
        SDL_Surface *t = TTF_RenderUTF8_Blended(Fonts_getSmall(), title_line2,
                                                  COLOR_LIGHT_TEXT);
        if (t) {
            int tx = panel_x + (panel_w - (t->w < text_max_w ? t->w : text_max_w)) / 2;
            SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){tx, text_y});
            SDL_FreeSurface(t);
        }
    }
}

/*
 * Render a standard screen with a list on the left panel and art on the right.
 * total_items includes a possible loading sentinel.
 */
static void render_browse_screen(SDL_Surface *screen,
                                  const char *header,
                                  int selected, int *scroll,
                                  int total_items,
                                  const char *art_line1, const char *art_line2,
                                  SDL_Surface *art,
                                  /* item label callback */
                                  void (*get_label)(int idx, char *buf, int size,
                                                    void *userdata),
                                  void *userdata,
                                  const char *btn_a_label,
                                  const char *btn_b_label,
                                  int show_panel)
{
    int hw = screen->w;
    int list_w = show_panel ? (hw * LIST_PANEL_FRAC) / 100 : hw;

    /* Background */
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    render_screen_header(screen, header, 0);

    /* Restrict list rendering to left panel width */
    ListLayout layout = calc_list_layout(screen);
    int orig_max_width = layout.max_width;
    layout.max_width = list_w - SCALE1(BUTTON_PADDING * 2);
    if (layout.max_width > orig_max_width) layout.max_width = orig_max_width;

    adjust_list_scroll(selected, scroll, layout.items_per_page);

    for (int i = *scroll; i < total_items && i < *scroll + layout.items_per_page; i++) {
        int y = layout.list_y + (i - *scroll) * layout.item_h;
        bool sel = (i == selected);

        char label[512];
        get_label(i, label, sizeof(label), userdata);

        char truncated[512];
        ListItemPos pos = render_list_item_pill(screen, &layout, label,
                                                truncated, y, sel, 0);
        render_list_item_text(screen, NULL, truncated, Fonts_getMedium(),
                              pos.text_x, pos.text_y, layout.max_width, sel);
    }

    render_scroll_indicators(screen, *scroll, layout.items_per_page, total_items);

    /* Art panel (only for BROWSE_ALBUMS) */
    if (show_panel)
        render_art_panel(screen, art, art_line1, art_line2);

    /* Button hints */
    if (btn_a_label && btn_b_label) {
        GFX_blitButtonGroup((char*[]){
            "A", (char*)btn_a_label,
            "B", (char*)btn_b_label,
            NULL}, 1, screen, 1);
    } else if (btn_b_label) {
        GFX_blitButtonGroup((char*[]){"B", (char*)btn_b_label, NULL}, 1, screen, 1);
    }
}

/*
 * Render the "Quit PlexMusic?" confirmation dialog overlay on top of the
 * current screen contents (call after the underlying screen has been drawn).
 */
static void render_quit_confirm_dialog(SDL_Surface *screen)
{
    int box_w = SCALE1(280);
    int box_h = SCALE1(80);

    DialogBox dlg = render_dialog_box(screen, box_w, box_h);

    SDL_Surface *msg = TTF_RenderUTF8_Blended(
        Fonts_getMedium(), "Quit PlexMusic?", COLOR_WHITE);
    if (msg) {
        int tx = dlg.box_x + (dlg.box_w - msg->w) / 2;
        int ty = dlg.box_y + SCALE1(12);
        SDL_BlitSurface(msg, NULL, screen, &(SDL_Rect){tx, ty});
        SDL_FreeSurface(msg);
    }

    SDL_Surface *hint = TTF_RenderUTF8_Blended(
        Fonts_getSmall(), "[A] Quit  [B] Cancel", COLOR_LIGHT_TEXT);
    if (hint) {
        int tx = dlg.box_x + (dlg.box_w - hint->w) / 2;
        int ty = dlg.box_y + box_h - SCALE1(28);
        SDL_BlitSurface(hint, NULL, screen, &(SDL_Rect){tx, ty});
        SDL_FreeSurface(hint);
    }
}

/* =========================================================================
 * Label callbacks
 * ========================================================================= */

typedef struct {
    PlexLibrary *libs;
    int count;
    bool bg_active;
    const char *now_playing_label; /* e.g. "▶ Track - Artist" */
} LibLabelCtx;

static void lib_get_label(int i, char *buf, int size, void *ud)
{
    LibLabelCtx *ctx = (LibLabelCtx *)ud;
    if (ctx->bg_active) {
        /* Items: 0..count-1 = libraries, count = now playing, count+1 = settings */
        if (i == ctx->count) {
            snprintf(buf, size, "%s", ctx->now_playing_label);
        } else if (i == ctx->count + 1) {
            snprintf(buf, size, "[Settings]");
        } else {
            snprintf(buf, size, "%s", ctx->libs[i].title);
        }
    } else {
        /* Items: 0..count-1 = libraries, count = settings */
        if (i == ctx->count) {
            snprintf(buf, size, "[Settings]");
        } else {
            snprintf(buf, size, "%s", ctx->libs[i].title);
        }
    }
}

typedef struct {
    PlexArtist *artists;
    int loaded;
    int total;
    bool page_loading;
} ArtistLabelCtx;

static void artist_get_label(int i, char *buf, int size, void *ud)
{
    ArtistLabelCtx *ctx = (ArtistLabelCtx *)ud;
    if (i < ctx->loaded) {
        snprintf(buf, size, "%s", ctx->artists[i].title);
    } else {
        /* Only shown when page_loading; i == ctx->loaded */
        snprintf(buf, size, "(Loading...)");
    }
}

typedef struct {
    PlexAlbum *albums;
    int count;
    bool show_status;   /* true in streaming mode — show [↓]/[✓] indicators */
} AlbumLabelCtx;

static void album_get_label(int i, char *buf, int size, void *ud)
{
    AlbumLabelCtx *ctx = (AlbumLabelCtx *)ud;
    const PlexAlbum *a = &ctx->albums[i];

    char base[PLEX_MAX_STR + 16];
    if (a->year[0])
        snprintf(base, sizeof(base), "%s (%s)", a->title, a->year);
    else
        snprintf(base, sizeof(base), "%s", a->title);

    if (ctx->show_status) {
        DlStatus st = plex_downloads_album_status(a->rating_key);
        if (st == DL_STATUS_DONE)
            snprintf(buf, size, "%s \xe2\x9c\x93", base);        /* ✓ */
        else if (st == DL_STATUS_DOWNLOADING || st == DL_STATUS_QUEUED)
            snprintf(buf, size, "%s \xe2\x86\x93", base);        /* ↓ */
        else
            snprintf(buf, size, "%s", base);
    } else {
        snprintf(buf, size, "%s", base);
    }
}

typedef struct {
    PlexTrack *tracks;
    int count;
} TrackLabelCtx;

static void track_get_label(int i, char *buf, int size, void *ud)
{
    TrackLabelCtx *ctx = (TrackLabelCtx *)ud;
    char dur[16];
    format_duration(ctx->tracks[i].duration_ms, dur, sizeof(dur));
    if (ctx->tracks[i].track_number > 0)
        snprintf(buf, size, "%d. %s  %s",
                 ctx->tracks[i].track_number, ctx->tracks[i].title, dur);
    else
        snprintf(buf, size, "%s  %s", ctx->tracks[i].title, dur);
}

/* =========================================================================
 * Public entry points
 * ========================================================================= */

/* File-scope flag so module_browse_reset() can reach it. */
static bool s_browse_initialized = false;

void module_browse_reset(void)
{
    if (s_load.thread_started) {
        s_load.cancel = true;
        pthread_join(s_load.thread, NULL);
        s_load.thread_started = false;
        pthread_mutex_lock(&s_load.lock);
        s_load.status = LOAD_IDLE;
        pthread_mutex_unlock(&s_load.lock);
    }
    s_browse_initialized = false;
}

AppModule module_browse_run(SDL_Surface *screen)
{
    PLEX_LOG("[DIAG] module_browse_run entered\n");
    /* ------------------------------------------------------------------ */
    /* Static state — persists between re-entries from the player module  */
    /* ------------------------------------------------------------------ */
    /* initialized is file-scope so module_browse_reset() can clear it */
    static BrowseState   state             = BROWSE_LIBRARIES;

    /* Libraries */
    static PlexLibrary   libs[16];
    static int           lib_count         = 0;
    static int           lib_selected      = 0;
    static int           lib_scroll        = 0;
    static int           lib_music_count   = 0; /* filtered count */
    static int           lib_music_idx[16];     /* map filtered → original */
    static LoadState     libs_ls           = LOAD_IDLE;

    /* Artists */
    static PlexArtist    artists[PLEX_MAX_ITEMS];
    static int           artists_loaded    = 0;
    static int           artists_total     = 0;
    static int           artist_selected   = 0;
    static int           artist_scroll     = 0;
    static LoadState     artists_ls        = LOAD_IDLE;
    static bool          artists_page_loading = false;

    /* Albums */
    static PlexAlbum     albums[PLEX_MAX_ITEMS];
    static int           album_count       = 0;
    static int           album_selected    = 0;
    static int           album_scroll      = 0;
    static LoadState     albums_ls         = LOAD_IDLE;

    /* Tracks */
    static PlexTrack     tracks[PLEX_MAX_ITEMS];
    static int           track_count       = 0;
    static int           track_selected    = 0;
    static int           track_scroll      = 0;
    static LoadState     tracks_ls         = LOAD_IDLE;

    /* Selected keys carried between levels */
    static int           selected_library_id       = 0;
    static int           selected_artist_rating_key = 0;
    static int           selected_album_rating_key  = 0;
    static char          selected_album_thumb[PLEX_MAX_URL] = "";

    /* Art fetch state: last thumb that was requested */
    static char          last_art_thumb[PLEX_MAX_URL] = "";

    /* Net error state */
    static BrowseState net_error_from = BROWSE_LIBRARIES; /* state that failed  */
    static BrowseState net_error_back = BROWSE_LIBRARIES; /* parent to go to on B */

    /* ------------------------------------------------------------------ */
    /* Per-frame locals                                                    */
    /* ------------------------------------------------------------------ */
    int dirty = 1;
    int show_setting = 0;
    bool quit_confirm_active = false;

    PlexConfig *mutable_cfg = plex_config_get_mutable();
    const PlexConfig *cfg   = mutable_cfg;
    PLEX_LOG("[DIAG] cfg obtained: server=%s\n", cfg ? cfg->server_url : "(null)");

    /* ------------------------------------------------------------------ */
    /* First-time initialisation                                           */
    /* ------------------------------------------------------------------ */
    if (!s_browse_initialized) {
        s_browse_initialized = true;
        state           = BROWSE_LIBRARIES;
        lib_count       = 0;
        lib_music_count = 0;
        lib_selected    = 0;
        lib_scroll      = 0;
        libs_ls         = LOAD_IDLE;
        artists_loaded  = 0;
        artists_total   = 0;
        artists_ls      = LOAD_IDLE;
        artist_selected = 0;
        artist_scroll   = 0;
        artists_page_loading = false;
        album_count     = 0;
        albums_ls       = LOAD_IDLE;
        album_selected  = 0;
        album_scroll    = 0;
        track_count     = 0;
        tracks_ls       = LOAD_IDLE;
        track_selected  = 0;
        track_scroll    = 0;
        last_art_thumb[0] = '\0';
        quit_confirm_active = false;
    }

    /* ------------------------------------------------------------------ */
    /* Main event loop                                                     */
    /* ------------------------------------------------------------------ */
    while (1) {
        GFX_startFrame();
        PAD_poll();

        /* Global input (Start long-press → quit dialog, volume, etc.) */
        GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0);
        if (global.should_quit) {
            return MODULE_QUIT;
        }
        if (global.input_consumed) {
            if (global.dirty) dirty = 1;
            GFX_sync();
            continue;
        }

        /* ==============================================================
         * BROWSE_LIBRARIES
         * ============================================================== */
        if (state == BROWSE_LIBRARIES) {

            /* Offline mode: skip library loading, go straight to artists */
            if (cfg->offline_mode) {
                artists_loaded = plex_downloads_get_artists(artists, PLEX_MAX_ITEMS);
                artists_total  = artists_loaded;
                artists_ls     = LOAD_READY;
                artist_selected = 0;
                artist_scroll   = 0;
                artists_page_loading = false;
                state = BROWSE_ARTISTS;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* Kick on first entry */
            if (libs_ls == LOAD_IDLE) {
                PLEX_LOG("[Browse] Loading libraries from: %s\n", cfg->server_url);
                memset(libs, 0, sizeof(libs));
                lib_count = 0;
                browse_load_kick(BROWSE_LIBRARIES, cfg,
                                 0, 0, 0,
                                 libs, &lib_count,
                                 NULL, NULL, NULL, NULL, NULL);
                libs_ls = LOAD_RUNNING;
            }

            /* Poll running load */
            if (libs_ls == LOAD_RUNNING) {
                LoadState ws = browse_load_poll();
                if (ws == LOAD_DONE || ws == LOAD_ERROR)
                    libs_ls = ws;
                else {
                    render_loading(screen, false);
                    GFX_sync();
                    continue;
                }
            }

            /* Collect results */
            if (libs_ls == LOAD_DONE) {
                browse_load_join();
                PLEX_LOG("[Browse] Got %d libraries (raw)\n", lib_count);
                lib_music_count = 0;
                for (int i = 0; i < lib_count; i++) {
                    if (strcmp(libs[i].type, "artist") == 0) {
                        lib_music_idx[lib_music_count++] = i;
                    }
                }
                PLEX_LOG("[Browse] Music library count: %d\n", lib_music_count);

                if (lib_music_count == 0) {
                    libs_ls = LOAD_IDLE;
                    net_error_from = BROWSE_LIBRARIES;
                    net_error_back = BROWSE_LIBRARIES;
                    state = BROWSE_NET_ERROR;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }

                /* Auto-select if only one music library */
                if (lib_music_count == 1) {
                    selected_library_id = libs[lib_music_idx[0]].section_id;
                    artists_loaded  = 0;
                    artists_total   = 0;
                    artists_ls      = LOAD_IDLE;
                    artist_selected = 0;
                    artist_scroll   = 0;
                    libs_ls = LOAD_READY;
                    state = BROWSE_ARTISTS;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }

                dirty = 1;
                libs_ls = LOAD_READY;
            }

            if (libs_ls == LOAD_ERROR) {
                browse_load_join();
                libs_ls = LOAD_IDLE;
                net_error_from = BROWSE_LIBRARIES;
                net_error_back = BROWSE_LIBRARIES;
                state = BROWSE_NET_ERROR;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* libs_ls == LOAD_READY — normal input + render */

            /* Total visible items: music libraries + "Now Playing" (if bg active) + Settings */
            bool bg_active = (Background_getActive() == BG_MUSIC);
            int lib_total_items = lib_music_count + (bg_active ? 2 : 1);
            int settings_idx   = bg_active ? lib_music_count + 1 : lib_music_count;
            int nowplaying_idx = lib_music_count; /* only valid when bg_active */

            /* Clamp selection in case bg state changed (e.g. track ended while browsing) */
            if (lib_selected >= lib_total_items)
                lib_selected = lib_total_items - 1;

            /* Build "Now Playing" label once per frame */
            char now_playing_label[PLEX_MAX_STR * 2 + 8] = "";
            if (bg_active) {
                PlexQueue *q = plex_queue_get();
                if (q && q->active && q->count > 0) {
                    snprintf(now_playing_label, sizeof(now_playing_label),
                             "\xe2\x96\xb6 %s - %s",
                             q->tracks[q->current_index].title,
                             q->tracks[q->current_index].artist);
                } else {
                    snprintf(now_playing_label, sizeof(now_playing_label),
                             "\xe2\x96\xb6 Now Playing");
                }
            }

            /* Input */
            if (quit_confirm_active) {
                if (PAD_justPressed(BTN_A)) return MODULE_QUIT;
                if (PAD_justPressed(BTN_B)) { quit_confirm_active = false; }
                dirty = 1;  /* always re-render when dialog is active */
            } else if (PAD_justRepeated(BTN_UP)) {
                lib_selected = (lib_selected > 0) ? lib_selected - 1 : lib_total_items - 1;
                dirty = 1;
            } else if (PAD_justRepeated(BTN_DOWN)) {
                lib_selected = (lib_selected < lib_total_items - 1) ? lib_selected + 1 : 0;
                dirty = 1;
            } else if (PAD_justPressed(BTN_A)) {
                if (lib_selected == settings_idx) {
                    /* Settings item selected */
                    return MODULE_SETTINGS;
                } else if (bg_active && lib_selected == nowplaying_idx) {
                    /* Now Playing item — return to player without re-downloading */
                    return MODULE_PLAYER;
                } else if (lib_music_count > 0 && lib_selected < lib_music_count) {
                    int real_idx = lib_music_idx[lib_selected];
                    selected_library_id = libs[real_idx].section_id;
                    artists_loaded  = 0;
                    artists_total   = 0;
                    artists_ls      = LOAD_IDLE;
                    artist_selected = 0;
                    artist_scroll   = 0;
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    state = BROWSE_ARTISTS;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
            } else if (PAD_justPressed(BTN_B)) {
                quit_confirm_active = true;
                dirty = 1;
            } else if (PAD_justPressed(BTN_SELECT)) {
                /* Cancel any running load */
                if (s_load.thread_started) {
                    s_load.cancel = true;
                    pthread_join(s_load.thread, NULL);
                    s_load.thread_started = false;
                    pthread_mutex_lock(&s_load.lock);
                    s_load.status = LOAD_IDLE;
                    pthread_mutex_unlock(&s_load.lock);
                }
                /* Switch to offline */
                mutable_cfg->offline_mode = true;
                plex_config_save(mutable_cfg);
                /* Reset artist state so offline load fires on entry */
                artists_loaded = 0;
                artists_total  = 0;
                artists_ls     = LOAD_IDLE;
                artist_selected = 0;
                artist_scroll   = 0;
                artists_page_loading = false;
                last_art_thumb[0] = '\0';
                plex_art_clear();
                /* Go directly to offline artists */
                artists_loaded = plex_downloads_get_artists(artists, PLEX_MAX_ITEMS);
                artists_total  = artists_loaded;
                artists_ls     = LOAD_READY;
                state = BROWSE_ARTISTS;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* Render */
            if (dirty) {
                LibLabelCtx lctx;
                /* Build a filtered list of music-library names */
                PlexLibrary music_libs[16];
                for (int i = 0; i < lib_music_count; i++)
                    music_libs[i] = libs[lib_music_idx[i]];
                lctx.libs             = music_libs;
                lctx.count            = lib_music_count;
                lctx.bg_active        = bg_active;
                lctx.now_playing_label = now_playing_label;

                /* art_line1: show library name for library items, empty for others */
                const char *art_line1 = "";
                if (lib_selected < lib_music_count && lib_music_count > 0)
                    art_line1 = music_libs[lib_selected].title;

                render_browse_screen(screen, "Music Libraries",
                                     lib_selected, &lib_scroll,
                                     lib_total_items,
                                     art_line1,
                                     NULL, plex_art_get(),
                                     lib_get_label, &lctx,
                                     "SELECT", "QUIT", 0);
                /* Extra hint for offline mode toggle */
                GFX_blitButtonGroup((char*[]){"SELECT", "OFFLINE", NULL}, 0, screen, 0);
                if (quit_confirm_active) {
                    render_quit_confirm_dialog(screen);
                }
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_ARTISTS
         * ============================================================== */
        } else if (state == BROWSE_ARTISTS) {

            /* Kick on first entry */
            if (artists_ls == LOAD_IDLE) {
                PLEX_LOG("[Browse] Loading artists for library_id=%d\n", selected_library_id);
                browse_load_kick(BROWSE_ARTISTS, cfg,
                                 selected_library_id, 0, 0,
                                 NULL, NULL, artists, NULL, NULL, NULL, NULL);
                artists_ls = LOAD_RUNNING;
            }

            /* Poll running load */
            if (artists_ls == LOAD_RUNNING) {
                LoadState ws = browse_load_poll();
                if (ws == LOAD_DONE || ws == LOAD_ERROR)
                    artists_ls = ws;
                else {
                    render_loading(screen, true);
                    if (PAD_justPressed(BTN_B)) {
                        s_load.cancel = true;
                        artists_ls = LOAD_IDLE;
                        /* Back to libraries; keep lib data valid to avoid join on next frame */
                        lib_selected    = 0;
                        lib_scroll      = 0;
                        last_art_thumb[0] = '\0';
                        plex_art_clear();
                        state = BROWSE_LIBRARIES;
                        dirty = 1;
                    }
                    GFX_sync();
                    continue;
                }
            }

            /* Collect results */
            if (artists_ls == LOAD_DONE) {
                browse_load_join();
                artists_loaded = s_load.artists_page.count;
                artists_total  = s_load.artists_page.total;
                PLEX_LOG("[Browse] Got %d / %d artists\n", artists_loaded, artists_total);

                dirty = 1;
                artists_ls = LOAD_READY;
                GFX_sync();
                continue;
            }

            if (artists_ls == LOAD_ERROR) {
                browse_load_join();
                artists_ls = LOAD_IDLE;
                net_error_from = BROWSE_ARTISTS;
                net_error_back = BROWSE_LIBRARIES;
                state = BROWSE_NET_ERROR;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* artists_ls == LOAD_READY — normal input + render */

            /* Collect pagination results */
            if (artists_page_loading) {
                LoadState ws = browse_load_poll();
                if (ws == LOAD_DONE) {
                    browse_load_join();
                    int new_count = s_load.artists_page.count;
                    artists_loaded += new_count;
                    if (artists_loaded > PLEX_MAX_ITEMS) artists_loaded = PLEX_MAX_ITEMS;
                    if (new_count == 0) artists_total = artists_loaded; /* server lied */
                    artists_page_loading = false;
                    dirty = 1;
                } else if (ws == LOAD_ERROR) {
                    browse_load_join();
                    artists_page_loading = false;
                    /* silent fail — user can retry by scrolling */
                }
            }

            /* Count visible items (artists + optional "(Loading...)" sentinel) */
            int visible_items = artists_loaded + (artists_page_loading ? 1 : 0);

            /* Trigger background page fetch when cursor nears end */
            if (!artists_page_loading
                && artists_loaded < artists_total
                && artists_loaded < PLEX_MAX_ITEMS
                && artist_selected >= artists_loaded - ARTIST_PAGE_LOOKAHEAD) {
                s_load.artist_offset = artists_loaded;
                browse_load_kick(BROWSE_ARTISTS_PAGE, cfg,
                                 selected_library_id, 0, 0,
                                 NULL, NULL, artists, NULL, NULL, NULL, NULL);
                artists_page_loading = true;
            }

            /* Input */
            if (PAD_justRepeated(BTN_UP)) {
                int prev = artist_selected;
                artist_selected = (artist_selected > 0) ? artist_selected - 1
                                                         : visible_items - 1;
                if (artist_selected != prev) {
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_DOWN)) {
                int prev = artist_selected;
                artist_selected = (artist_selected < visible_items - 1)
                                      ? artist_selected + 1 : 0;
                if (artist_selected != prev) {
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_A)) {
                if (artist_selected < artists_loaded) {
                    /* Cancel any in-flight page load before navigating away */
                    if (artists_page_loading) {
                        s_load.cancel = true;
                        artists_page_loading = false;
                    }
                    selected_artist_rating_key = artists[artist_selected].rating_key;
                    albums_ls      = LOAD_IDLE;
                    album_selected = 0;
                    album_scroll   = 0;
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    state = BROWSE_ALBUMS;
                    dirty = 1;
                }
                /* artist_selected == artists_loaded is the "(Loading...)" sentinel — do nothing */
            } else if (PAD_justPressed(BTN_B)) {
                if (cfg->offline_mode) {
                    return MODULE_QUIT;
                }
                /* Cancel any in-flight page load */
                if (artists_page_loading) {
                    s_load.cancel = true;
                    artists_page_loading = false;
                    /* old thread joined by the next browse_load_kick for BROWSE_LIBRARIES */
                }
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_LIBRARIES;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_SELECT) && cfg->offline_mode) {
                mutable_cfg->offline_mode = false;
                plex_config_save(mutable_cfg);
                /* Reset libs so they reload from network */
                lib_count       = 0;
                lib_music_count = 0;
                lib_selected    = 0;
                lib_scroll      = 0;
                libs_ls         = LOAD_IDLE;
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_LIBRARIES;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* Render */
            if (dirty) {
                ArtistLabelCtx actx;
                actx.artists      = artists;
                actx.loaded       = artists_loaded;
                actx.total        = artists_total;
                actx.page_loading = artists_page_loading;

                const char *art_line1 = (artist_selected < artists_loaded)
                    ? artists[artist_selected].title : NULL;

                const char *artists_header = cfg->offline_mode ? "Artists (Offline)" : "Artists";
                render_browse_screen(screen, artists_header,
                                     artist_selected, &artist_scroll,
                                     visible_items,
                                     art_line1, NULL, plex_art_get(),
                                     artist_get_label, &actx,
                                     "SELECT", "BACK", 0);
                if (cfg->offline_mode)
                    GFX_blitButtonGroup((char*[]){"SELECT", "ONLINE", NULL}, 0, screen, 0);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_ALBUMS
         * ============================================================== */
        } else if (state == BROWSE_ALBUMS) {

            /* Kick on first entry */
            if (albums_ls == LOAD_IDLE) {
                if (cfg->offline_mode) {
                    album_count = plex_downloads_get_albums_for_artist(
                                      selected_artist_rating_key, albums, PLEX_MAX_ITEMS);
                    albums_ls = LOAD_READY;
                    if (album_count > 0 && albums[0].thumb[0]) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb), "%s", albums[0].thumb);
                        plex_art_fetch(cfg, albums[0].thumb);
                    }
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                browse_load_kick(BROWSE_ALBUMS, cfg,
                                 0, selected_artist_rating_key, 0,
                                 NULL, NULL, NULL, albums, &album_count, NULL, NULL);
                albums_ls = LOAD_RUNNING;
            }

            /* Poll running load */
            if (albums_ls == LOAD_RUNNING) {
                LoadState ws = browse_load_poll();
                if (ws == LOAD_DONE || ws == LOAD_ERROR)
                    albums_ls = ws;
                else {
                    render_loading(screen, true);
                    if (PAD_justPressed(BTN_B)) {
                        s_load.cancel = true;
                        albums_ls = LOAD_IDLE;
                        last_art_thumb[0] = '\0';
                        plex_art_clear();
                        state = BROWSE_ARTISTS;
                        dirty = 1;
                    }
                    GFX_sync();
                    continue;
                }
            }

            /* Collect results */
            if (albums_ls == LOAD_DONE) {
                browse_load_join();
                /* album_count already written by worker via pointer */
                if (album_count > 0 && albums[0].thumb[0]) {
                    snprintf(last_art_thumb, sizeof(last_art_thumb), "%s", albums[0].thumb);
                    plex_art_fetch(cfg, albums[0].thumb);
                }
                dirty = 1;
                albums_ls = LOAD_READY;
                GFX_sync();
                continue;
            }

            if (albums_ls == LOAD_ERROR) {
                browse_load_join();
                albums_ls = LOAD_IDLE;
                net_error_from = BROWSE_ALBUMS;
                net_error_back = BROWSE_ARTISTS;
                state = BROWSE_NET_ERROR;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* albums_ls == LOAD_READY — normal input + render */

            /* Input */
            if (PAD_justRepeated(BTN_UP)) {
                int prev = album_selected;
                album_selected = (album_selected > 0) ? album_selected - 1
                                                       : album_count - 1;
                if (album_selected != prev) {
                    if (albums[album_selected].thumb[0] &&
                        strcmp(albums[album_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", albums[album_selected].thumb);
                        plex_art_fetch(cfg, albums[album_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_DOWN)) {
                int prev = album_selected;
                album_selected = (album_selected < album_count - 1)
                                      ? album_selected + 1 : 0;
                if (album_selected != prev) {
                    if (albums[album_selected].thumb[0] &&
                        strcmp(albums[album_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", albums[album_selected].thumb);
                        plex_art_fetch(cfg, albums[album_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_A) && album_count > 0) {
                selected_album_rating_key = albums[album_selected].rating_key;
                PLEX_LOG("[Browse] Album selected: id=%d title=%s offline=%d\n",
                         selected_album_rating_key, albums[album_selected].title,
                         cfg->offline_mode);
                snprintf(selected_album_thumb, sizeof(selected_album_thumb),
                         "%s", albums[album_selected].thumb);
                tracks_ls      = LOAD_IDLE;
                track_selected = 0;
                track_scroll   = 0;
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_TRACKS;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_B)) {
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_ARTISTS;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_Y) && !cfg->offline_mode && album_count > 0) {
                plex_downloads_queue_album(mutable_cfg,
                    albums[album_selected].rating_key,
                    albums[album_selected].title,
                    artists[artist_selected].rating_key,
                    artists[artist_selected].title,
                    albums[album_selected].thumb);
                dirty = 1;
            }

            /* Poll art async */
            if (plex_art_is_fetching()) dirty = 1;

            /* Render */
            if (dirty) {
                AlbumLabelCtx alctx;
                alctx.albums       = albums;
                alctx.count        = album_count;
                alctx.show_status  = !cfg->offline_mode;

                const char *art_line1 = (album_count > 0)
                    ? albums[album_selected].title : NULL;
                const char *art_line2 = (album_count > 0 && albums[album_selected].year[0])
                    ? albums[album_selected].year : NULL;

                render_browse_screen(screen, "Albums",
                                     album_selected, &album_scroll,
                                     album_count,
                                     art_line1, art_line2, plex_art_get(),
                                     album_get_label, &alctx,
                                     "SELECT", "BACK", 1);
                if (!cfg->offline_mode)
                    GFX_blitButtonGroup((char*[]){"Y", "DOWNLOAD", NULL}, 0, screen, 0);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_TRACKS
         * ============================================================== */
        } else if (state == BROWSE_TRACKS) {

            /* Kick on first entry */
            if (tracks_ls == LOAD_IDLE) {
                if (cfg->offline_mode) {
                    track_count = plex_downloads_get_tracks_for_album(
                                      selected_album_rating_key, tracks, PLEX_MAX_ITEMS);
                    PLEX_LOG("[Browse] Offline tracks for album %d: %d tracks\n",
                             selected_album_rating_key, track_count);
                    tracks_ls = LOAD_READY;
                    /* Art is already loaded from album selection */
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                browse_load_kick(BROWSE_TRACKS, cfg,
                                 0, 0, selected_album_rating_key,
                                 NULL, NULL, NULL, NULL, NULL, tracks, &track_count);
                tracks_ls = LOAD_RUNNING;
            }

            /* Poll running load */
            if (tracks_ls == LOAD_RUNNING) {
                LoadState ws = browse_load_poll();
                if (ws == LOAD_DONE || ws == LOAD_ERROR)
                    tracks_ls = ws;
                else {
                    render_loading(screen, true);
                    if (PAD_justPressed(BTN_B)) {
                        s_load.cancel = true;
                        tracks_ls = LOAD_IDLE;
                        last_art_thumb[0] = '\0';
                        plex_art_clear();
                        if (album_selected < album_count && albums[album_selected].thumb[0]) {
                            snprintf(last_art_thumb, sizeof(last_art_thumb),
                                     "%s", albums[album_selected].thumb);
                            plex_art_fetch(cfg, albums[album_selected].thumb);
                        }
                        state = BROWSE_ALBUMS;
                        dirty = 1;
                    }
                    GFX_sync();
                    continue;
                }
            }

            /* Collect results */
            if (tracks_ls == LOAD_DONE) {
                browse_load_join();
                /* track_count already written by worker via pointer */

                /* Fetch album art once — same for all tracks */
                if (selected_album_thumb[0] &&
                    strcmp(selected_album_thumb, last_art_thumb) != 0) {
                    snprintf(last_art_thumb, sizeof(last_art_thumb),
                             "%s", selected_album_thumb);
                    plex_art_fetch(cfg, selected_album_thumb);
                }
                dirty = 1;
                tracks_ls = LOAD_READY;
                GFX_sync();
                continue;
            }

            if (tracks_ls == LOAD_ERROR) {
                browse_load_join();
                tracks_ls = LOAD_IDLE;
                net_error_from = BROWSE_TRACKS;
                net_error_back = BROWSE_ALBUMS;
                state = BROWSE_NET_ERROR;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* tracks_ls == LOAD_READY — normal input + render */

            /* Input */
            if (PAD_justRepeated(BTN_UP)) {
                track_selected = (track_selected > 0)
                    ? track_selected - 1 : track_count - 1;
                dirty = 1;
            } else if (PAD_justRepeated(BTN_DOWN)) {
                track_selected = (track_selected < track_count - 1)
                    ? track_selected + 1 : 0;
                dirty = 1;
            } else if (PAD_justPressed(BTN_A) && track_count > 0) {
                Background_setActive(BG_NONE);
                plex_queue_set(cfg, tracks, track_count, track_selected);
                return MODULE_PLAYER;
            } else if (PAD_justPressed(BTN_B)) {
                last_art_thumb[0] = '\0';
                plex_art_clear();
                /* Restore art for the currently selected album */
                if (album_selected < album_count && albums[album_selected].thumb[0]) {
                    snprintf(last_art_thumb, sizeof(last_art_thumb),
                             "%s", albums[album_selected].thumb);
                    plex_art_fetch(cfg, albums[album_selected].thumb);
                }
                state = BROWSE_ALBUMS;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* Poll art async */
            if (plex_art_is_fetching()) dirty = 1;

            /* Render */
            if (dirty) {
                TrackLabelCtx tctx;
                tctx.tracks = tracks;
                tctx.count  = track_count;

                /* Album title + year as art metadata */
                char art_line2[PLEX_MAX_STR + 8] = "";
                /* Find album name from track info if available */
                const char *art_line1 = (track_count > 0) ? tracks[0].album : NULL;
                if (track_count > 0 && album_count > 0) {
                    art_line1 = albums[album_selected].title;
                    if (albums[album_selected].year[0])
                        snprintf(art_line2, sizeof(art_line2), "%s",
                                 albums[album_selected].year);
                }

                render_browse_screen(screen, "Tracks",
                                     track_selected, &track_scroll,
                                     track_count,
                                     art_line1,
                                     art_line2[0] ? art_line2 : NULL,
                                     plex_art_get(),
                                     track_get_label, &tctx,
                                     "PLAY", "BACK", 0);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_NET_ERROR
         * ============================================================== */
        } else if (state == BROWSE_NET_ERROR) {

            if (PAD_justPressed(BTN_A)) {
                /* Retry: reset the load guard for the failed state */
                switch (net_error_from) {
                    case BROWSE_LIBRARIES: libs_ls    = LOAD_IDLE; break;
                    case BROWSE_ARTISTS:   artists_ls = LOAD_IDLE; break;
                    case BROWSE_ALBUMS:    albums_ls  = LOAD_IDLE; break;
                    case BROWSE_TRACKS:    tracks_ls  = LOAD_IDLE; break;
                    default: break;
                }
                state = net_error_from;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_B)) {
                if (net_error_back == BROWSE_LIBRARIES) {
                    return MODULE_QUIT;
                }
                last_art_thumb[0] = '\0';
                plex_art_clear();
                switch (net_error_back) {
                    case BROWSE_ARTISTS:
                        break;
                    case BROWSE_ALBUMS:
                        if (album_selected < album_count &&
                            albums[album_selected].thumb[0]) {
                            snprintf(last_art_thumb, sizeof(last_art_thumb),
                                     "%s", albums[album_selected].thumb);
                            plex_art_fetch(cfg, albums[album_selected].thumb);
                        }
                        break;
                    default:
                        break;
                }
                state = net_error_back;
                dirty = 1;
                GFX_sync();
                continue;
            }

            if (dirty) {
                render_net_error(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }
        }
    }
}
