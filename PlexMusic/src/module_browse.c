#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <ctype.h>

#include "api.h"
#include "plex_net.h"
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
#include "plex_favorites.h"
#include "ui_fonts.h"
#include "ui_utils.h"

/* =========================================================================
 * Internal state
 * ========================================================================= */

typedef enum {
    BROWSE_LIBRARIES,
    BROWSE_LIBRARY_PICKER, /* library fetch + selection (first launch / settings) */
    BROWSE_ARTISTS,
    BROWSE_ALBUMS,
    BROWSE_TRACKS,
    BROWSE_NET_ERROR,
    BROWSE_ARTISTS_PAGE,   /* append-only artist page load; NOT a visible state */
    BROWSE_ALL_ALBUMS,       /* flat all-albums list */
    BROWSE_ALL_ALBUMS_PAGE,  /* append-only page load; NOT a visible state */
    BROWSE_RECENT_ALBUMS,    /* recently added albums (online only) */
    BROWSE_FAVORITES,        /* favorite tracks list (online only) */
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
#define ARTIST_PAGE_LOOKAHEAD 45

/* =========================================================================
 * Async worker
 * ========================================================================= */

static struct {
    pthread_t        thread;
    pthread_mutex_t  lock;           /* protects status field */
    LoadState        status;         /* current worker status */
    volatile bool    cancel;         /* set by main thread to discard result */
    bool             thread_started; /* true if thread created and not yet joined */
    int              generation;     /* incremented on each kick; zombie threads check this */
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
    PlexPage         all_albums_page;   /* written by worker, read by main on DONE */
    int              all_albums_offset; /* offset for BROWSE_ALL_ALBUMS_PAGE worker */
    int              list_cap;          /* array capacity passed to worker for artists/all_albums pages */
} s_load = { .lock = PTHREAD_MUTEX_INITIALIZER };

static LoadState browse_load_poll(void);

static void *browse_load_worker(void *arg)
{
    (void)arg;
    /* Copy cfg fields off the shared struct before releasing the lock */
    PlexConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    int my_generation;
    pthread_mutex_lock(&s_load.lock);
    strncpy(cfg.server_url, s_load.server_url, sizeof(cfg.server_url) - 1);
    strncpy(cfg.token,      s_load.token,      sizeof(cfg.token) - 1);
    BrowseState type = s_load.type;
    my_generation    = s_load.generation;
    pthread_mutex_unlock(&s_load.lock);

    int rc = 0;
    switch (type) {
        case BROWSE_LIBRARIES:
        case BROWSE_LIBRARY_PICKER:
            rc = plex_api_get_libraries(&cfg, s_load.libs, s_load.lib_count);
            break;
        case BROWSE_ARTISTS: {
            PlexPage page;
            memset(&page, 0, sizeof(page));
            rc = plex_api_get_artists(&cfg, s_load.library_id, 0, s_load.list_cap,
                                      s_load.artists, &page);
            pthread_mutex_lock(&s_load.lock);
            if (my_generation == s_load.generation)
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
            int cap    = s_load.list_cap - offset;
            if (cap > 0) {
                rc = plex_api_get_artists(&cfg, s_load.library_id, offset, cap,
                                          s_load.artists + offset, &page);
            }
            pthread_mutex_lock(&s_load.lock);
            if (my_generation == s_load.generation)
                s_load.artists_page = page;
            pthread_mutex_unlock(&s_load.lock);
            break;
        }
        case BROWSE_ALL_ALBUMS: {
            PlexPage page;
            memset(&page, 0, sizeof(page));
            rc = plex_api_get_all_albums(&cfg, s_load.library_id, 0, s_load.list_cap,
                                         s_load.albums, &page);
            pthread_mutex_lock(&s_load.lock);
            if (my_generation == s_load.generation)
                s_load.all_albums_page = page;
            pthread_mutex_unlock(&s_load.lock);
            break;
        }
        case BROWSE_ALL_ALBUMS_PAGE: {
            PlexPage page;
            memset(&page, 0, sizeof(page));
            int offset = s_load.all_albums_offset;
            int cap    = s_load.list_cap - offset;
            if (cap > 0) {
                rc = plex_api_get_all_albums(&cfg, s_load.library_id, offset, cap,
                                             s_load.albums + offset, &page);
            }
            pthread_mutex_lock(&s_load.lock);
            if (my_generation == s_load.generation)
                s_load.all_albums_page = page;
            pthread_mutex_unlock(&s_load.lock);
            break;
        }
        case BROWSE_RECENT_ALBUMS:
            rc = plex_api_get_recent_albums(&cfg, s_load.library_id,
                                             s_load.list_cap,
                                             s_load.albums, s_load.album_count);
            break;
        default:
            rc = -1;
    }

    pthread_mutex_lock(&s_load.lock);
    if (my_generation == s_load.generation) {
        if (s_load.cancel || rc != 0)
            s_load.status = LOAD_ERROR;
        else
            s_load.status = LOAD_DONE;
    }
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
    /* Detach or join any previous thread */
    if (s_load.thread_started) {
        LoadState cur = browse_load_poll();
        if (cur == LOAD_RUNNING) {
            pthread_detach(s_load.thread);   /* let zombie die naturally */
        } else {
            pthread_join(s_load.thread, NULL);  /* already done — fast */
        }
        s_load.thread_started = false;
    }

    pthread_mutex_lock(&s_load.lock);
    s_load.cancel         = false;
    s_load.generation++;                 /* invalidates any zombie thread */
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

static int scroll_step(Uint32 held_since)
{
    if (!held_since) return 1;
    Uint32 ms = SDL_GetTicks() - held_since;
    if (ms < 1000) return 1;
    if (ms < 2000) return 3;
    if (ms < 3000) return 8;
    return 15;
}

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
                              const char *title_line1, const char *title_line2,
                              const char *title_line3)
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
            text_y += t->h + SCALE1(4);
            SDL_FreeSurface(t);
        }
    }
    if (title_line3 && title_line3[0]) {
        SDL_Surface *t = TTF_RenderUTF8_Blended(Fonts_getSmall(), title_line3,
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
                                  const char *art_line3,
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
        render_art_panel(screen, art, art_line1, art_line2, art_line3);

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

/*
 * Render the "Delete downloaded album?" confirmation dialog overlay on top of
 * the current screen contents (call after the underlying screen has been drawn).
 */
static void render_delete_confirm_dialog(SDL_Surface *screen)
{
    int box_w = SCALE1(300);
    int box_h = SCALE1(80);

    DialogBox dlg = render_dialog_box(screen, box_w, box_h);

    SDL_Surface *msg = TTF_RenderUTF8_Blended(
        Fonts_getMedium(), "Delete downloaded album?", COLOR_WHITE);
    if (msg) {
        int tx = dlg.box_x + (dlg.box_w - msg->w) / 2;
        int ty = dlg.box_y + SCALE1(12);
        SDL_BlitSurface(msg, NULL, screen, &(SDL_Rect){tx, ty});
        SDL_FreeSurface(msg);
    }

    SDL_Surface *hint = TTF_RenderUTF8_Blended(
        Fonts_getSmall(), "[A] Delete  [B] Cancel", COLOR_LIGHT_TEXT);
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

/* Home screen label callback: [Now Playing] / Artists / Albums / [Favorites] / [Recently Added] / Settings */
typedef struct {
    bool        bg_active;
    const char *now_playing_label;
    int         artists_idx;
    int         albums_idx;
    int         settings_idx;
    int         recent_idx;
    int         nowplay_idx;
    int         fav_idx;
} HomeLabelCtx;

static void home_get_label(int idx, char *buf, int size, void *ud)
{
    HomeLabelCtx *ctx = ud;
    if (ctx->nowplay_idx >= 0 && idx == ctx->nowplay_idx) { snprintf(buf, size, "%s", ctx->now_playing_label); return; }
    if (idx == ctx->artists_idx) { snprintf(buf, size, "Artists"); return; }
    if (idx == ctx->albums_idx)  { snprintf(buf, size, "Albums");  return; }
    if (ctx->fav_idx >= 0 && idx == ctx->fav_idx) {
        DlStatus st = plex_downloads_album_status(PLEX_FAVORITES_SYNC_ALBUM_ID);
        if (st == DL_STATUS_DOWNLOADING) {
            int completed = 0, total = 0;
            plex_downloads_album_progress(PLEX_FAVORITES_SYNC_ALBUM_ID,
                                          &completed, &total);
            if (total > 0)
                /* ○ U+25CB */
                snprintf(buf, size, "\xe2\x97\x8b %d/%d  Favorite Tracks",
                         completed, total);
            else
                /* ↓ U+2193 fallback before total is known */
                snprintf(buf, size, "\xe2\x86\x93  Favorite Tracks");
        } else {
            snprintf(buf, size, "Favorite Tracks");
        }
        return;
    }
    if (ctx->recent_idx >= 0 && idx == ctx->recent_idx) { snprintf(buf, size, "Recently Added"); return; }
    snprintf(buf, size, "Settings");
}

/* Library picker label callback: plain music library names */
static void libpick_get_label(int idx, char *buf, int size, void *ud)
{
    PlexLibrary (*music_libs)[16] = ud;
    snprintf(buf, size, "%s", (*music_libs)[idx].title);
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
        if (st == DL_STATUS_DONE) {
            /* ✓ U+2713 */
            snprintf(buf, size, "\xe2\x9c\x93  %s", base);
        } else if (st == DL_STATUS_DOWNLOADING) {
            int completed, total;
            if (plex_downloads_album_progress(a->rating_key, &completed, &total)
                    && total > 0)
                /* ○ U+25CB */
                snprintf(buf, size, "\xe2\x97\x8b %d/%d  %s", completed, total, base);
            else
                /* ↓ fallback if progress not yet available */
                snprintf(buf, size, "\xe2\x86\x93  %s", base);
        } else if (st == DL_STATUS_QUEUED) {
            snprintf(buf, size, "\xe2\x86\x93  %s", base);
        } else {
            snprintf(buf, size, "%s", base);
        }
    } else {
        snprintf(buf, size, "%s", base);
    }
}

typedef struct {
    PlexTrack *tracks;
    int        count;
    bool       show_favorites;  /* prepend ♥ for favorited tracks */
} TrackLabelCtx;

static void track_get_label(int i, char *buf, int size, void *ud)
{
    TrackLabelCtx *ctx = (TrackLabelCtx *)ud;
    char dur[16];
    format_duration(ctx->tracks[i].duration_ms, dur, sizeof(dur));

    bool fav = ctx->show_favorites &&
               plex_favorites_contains(ctx->tracks[i].rating_key);
    const char *heart = fav ? "\xe2\x99\xa5 " : "";

    if (ctx->tracks[i].track_number > 0)
        snprintf(buf, size, "%s%d. %s  %s",
                 heart, ctx->tracks[i].track_number,
                 ctx->tracks[i].title, dur);
    else
        snprintf(buf, size, "%s%s  %s",
                 heart, ctx->tracks[i].title, dur);
}

/* =========================================================================
 * Public entry points
 * ========================================================================= */

/* File-scope flag so module_browse_reset() can reach it. */
static bool s_browse_initialized = false;

/* Heap-allocated lists; sized from Plex totalSize on first page load. */
static PlexArtist  *s_artists        = NULL;
static int          s_artists_cap    = 0;
static PlexAlbum   *s_all_albums     = NULL;
static int          s_all_albums_cap = 0;

/* Favorites list — file-scope to avoid stack overflow (~1.3 MB) */
static PlexTrack s_fav_tracks[PLEX_MAX_TRACKS];
static int       s_fav_count = 0;

static bool pending_r2_jump     = false; /* deferred artist letter jump */
static bool pending_r2_jump_all = false; /* deferred all-albums year jump */

/* Set by module_browse_request_library_pick(); checked on every entry. */
static bool s_library_pick_requested = false;

void module_browse_request_library_pick(void) {
    s_library_pick_requested = true;
}

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
    /* Close any persistent keep-alive connection (safe after thread is joined) */
    plex_net_connection_close();
    if (s_artists)    { free(s_artists);    s_artists    = NULL; s_artists_cap    = 0; }
    if (s_all_albums) { free(s_all_albums); s_all_albums = NULL; s_all_albums_cap = 0; }
    s_browse_initialized = false;
    pending_r2_jump      = false;
    pending_r2_jump_all  = false;
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

    /* Artists — storage in file-scope s_artists heap pointer */
    static int           artists_loaded    = 0;
    static int           artists_total     = 0;
    static int           artist_selected   = 0;
    static int           artist_scroll     = 0;
    static LoadState     artists_ls        = LOAD_IDLE;
    static bool          artists_page_loading = false;

    /* Albums */
    static PlexAlbum     albums[PLEX_MAX_ARTIST_ALBUMS];
    static int           album_count       = 0;
    static int           album_selected    = 0;
    static int           album_scroll      = 0;
    static LoadState     albums_ls         = LOAD_IDLE;

    /* All Albums (flat view) — storage in file-scope s_all_albums heap pointer */
    static int           all_albums_loaded   = 0;
    static int           all_albums_total    = 0;
    static LoadState     all_albums_ls       = LOAD_IDLE;
    static int           all_album_selected  = 0;
    static int           all_album_scroll    = 0;
    static bool          all_albums_page_loading = false;

    /* Recently Added Albums (online only, single fetch) */
    static PlexAlbum     recent_albums[PLEX_MAX_ARTIST_ALBUMS];
    static int           recent_albums_count  = 0;
    static int           recent_album_selected = 0;
    static int           recent_album_scroll   = 0;
    static LoadState     recent_albums_ls      = LOAD_IDLE;

    /* Favorite Tracks (online only) */
    static int           fav_selected  = 0;
    static int           fav_scroll    = 0;

    /* Tracks */
    static PlexTrack     tracks[PLEX_MAX_TRACKS];
    static int           track_count       = 0;
    static int           track_selected    = 0;
    static int           track_scroll      = 0;
    static LoadState     tracks_ls         = LOAD_IDLE;

    /* Tracks back-navigation target */
    static BrowseState   tracks_back_state   = BROWSE_ALBUMS;

    /* Selected keys carried between levels */
    static int           selected_library_id       = 0;
    static int           selected_artist_rating_key = 0;
    static int           selected_album_rating_key  = 0;
    static char          selected_album_thumb[PLEX_MAX_URL] = "";

    /* Art fetch state: last thumb that was requested */
    static char          last_art_thumb[PLEX_MAX_URL] = "";

    /* Net error state */
    static BrowseState net_error_from  = BROWSE_LIBRARIES; /* state that failed  */
    static BrowseState net_error_back  = BROWSE_LIBRARIES; /* parent to go to on B */
    static int         net_auto_retries = 0;   /* auto-kick attempts before showing error */

    /* Hold-acceleration tracking */
    static Uint32 s_hold_up_since   = 0;
    static Uint32 s_hold_down_since = 0;

    /* ------------------------------------------------------------------ */
    /* Per-frame locals                                                    */
    /* ------------------------------------------------------------------ */
    int dirty = 1;
    int show_setting = 0;
    bool quit_confirm_active   = false;
    bool delete_confirm_active   = false;
    int  delete_confirm_album_id = -1;

    PlexConfig *mutable_cfg = plex_config_get_mutable();
    const PlexConfig *cfg   = mutable_cfg;
    PLEX_LOG("[DIAG] cfg obtained: server=%s\n", cfg ? cfg->server_url : "(null)");

    /* ------------------------------------------------------------------ */
    /* First-time initialisation                                           */
    /* ------------------------------------------------------------------ */
    if (!s_browse_initialized) {
        /* Defensive free in case reset() was skipped */
        if (s_artists)    { free(s_artists);    s_artists    = NULL; s_artists_cap    = 0; }
        if (s_all_albums) { free(s_all_albums); s_all_albums = NULL; s_all_albums_cap = 0; }

        /* Offline pre-alloc — done once here so plex_downloads_get_* has a buffer */
        if (cfg->offline_mode && s_artists == NULL) {
            s_artists     = malloc(PLEX_MAX_OFFLINE_ITEMS * sizeof(PlexArtist));
            s_artists_cap = s_artists ? PLEX_MAX_OFFLINE_ITEMS : 0;
        }
        if (cfg->offline_mode && s_all_albums == NULL) {
            s_all_albums     = malloc(PLEX_MAX_OFFLINE_ITEMS * sizeof(PlexAlbum));
            s_all_albums_cap = s_all_albums ? PLEX_MAX_OFFLINE_ITEMS : 0;
        }

        s_browse_initialized = true;
        state           = (cfg->library_id == 0) ? BROWSE_LIBRARY_PICKER : BROWSE_LIBRARIES;
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
        all_albums_loaded  = 0;
        all_albums_total   = 0;
        all_albums_ls      = LOAD_IDLE;
        all_album_selected = 0;
        all_album_scroll   = 0;
        all_albums_page_loading = false;
        recent_albums_count   = 0;
        recent_albums_ls      = LOAD_IDLE;
        recent_album_selected = 0;
        recent_album_scroll   = 0;
        pending_r2_jump     = false;
        pending_r2_jump_all = false;
        tracks_back_state  = BROWSE_ALBUMS;
        last_art_thumb[0] = '\0';
        quit_confirm_active = false;
    }

    /* ------------------------------------------------------------------ */
    /* Library pick request (from Settings) — fires on every re-entry     */
    /* ------------------------------------------------------------------ */
    if (s_library_pick_requested) {
        s_library_pick_requested = false;
        libs_ls         = LOAD_IDLE;
        lib_count       = 0;
        lib_music_count = 0;
        lib_selected    = 0;
        lib_scroll      = 0;
        state = BROWSE_LIBRARY_PICKER;
    }

    /* ------------------------------------------------------------------ */
    /* Main event loop                                                     */
    /* ------------------------------------------------------------------ */
    while (1) {
        GFX_startFrame();
        PAD_poll();

        Background_tick();

        /* Redraw every frame while a download is in progress */
        if (plex_downloads_is_active()) dirty = 1;

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

            /* Static home menu: Artists / Albums / [Recently Added] / [Now Playing] / Settings */

            bool bg_active = (Background_getActive() == BG_MUSIC);
            bool online    = !cfg->offline_mode;

            int artists_idx, albums_idx, fav_idx, recent_idx, nowplay_idx, settings_idx, home_item_count;

            if (bg_active) {
                nowplay_idx = 0; artists_idx = 1; albums_idx = 2;
                if (online) { fav_idx = 3; recent_idx = 4; settings_idx = 5; home_item_count = 6; }
                else        { fav_idx = 3; recent_idx = -1; settings_idx = 4; home_item_count = 5; }
            } else {
                nowplay_idx = -1; artists_idx = 0; albums_idx = 1;
                if (online) { fav_idx = 2; recent_idx = 3; settings_idx = 4; home_item_count = 5; }
                else        { fav_idx = 2; recent_idx = -1; settings_idx = 3; home_item_count = 4; }
            }

            /* Clamp selection in case bg state changed */
            if (lib_selected >= home_item_count)
                lib_selected = home_item_count - 1;

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
                if (PAD_justPressed(BTN_B)) { quit_confirm_active = false; dirty = 1; }
                dirty = 1;  /* always re-render when dialog is active */
            } else if (PAD_justRepeated(BTN_UP)) {
                lib_selected = (lib_selected > 0) ? lib_selected - 1 : 0;
                dirty = 1;
            } else if (PAD_justRepeated(BTN_DOWN)) {
                lib_selected = (lib_selected < home_item_count - 1) ? lib_selected + 1 : home_item_count - 1;
                dirty = 1;
            } else if (PAD_justPressed(BTN_A)) {
                if (lib_selected == settings_idx) {
                    return MODULE_SETTINGS;
                } else if (nowplay_idx >= 0 && lib_selected == nowplay_idx) {
                    return MODULE_PLAYER;
                } else if (recent_idx >= 0 && lib_selected == recent_idx) {
                    /* Recently Added (online only) */
                    selected_library_id   = cfg->library_id;
                    recent_albums_count   = 0;
                    recent_albums_ls      = LOAD_IDLE;
                    recent_album_selected = 0;
                    recent_album_scroll   = 0;
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    state = BROWSE_RECENT_ALBUMS;
                    dirty = 1;
                    GFX_sync();
                    continue;
                } else if (lib_selected == artists_idx) {
                    /* Artists */
                    if (cfg->library_id == 0) {
                        /* No library saved yet — go to picker */
                        libs_ls         = LOAD_IDLE;
                        lib_count       = 0;
                        lib_music_count = 0;
                        state = BROWSE_LIBRARY_PICKER;
                        dirty = 1;
                        GFX_sync();
                        continue;
                    } else {
                        selected_library_id = cfg->library_id;
                        artists_loaded  = 0;
                        artists_total   = 0;
                        artists_ls      = LOAD_IDLE;
                        artist_selected = 0;
                        artist_scroll   = 0;
                        tracks_back_state = BROWSE_ALBUMS;
                        last_art_thumb[0] = '\0';
                        plex_art_clear();
                        state = BROWSE_ARTISTS;
                        dirty = 1;
                        GFX_sync();
                        continue;
                    }
                } else if (lib_selected == albums_idx) {
                    /* Albums */
                    if (!cfg->offline_mode && cfg->library_id == 0) {
                        /* No library saved yet — go to picker */
                        libs_ls         = LOAD_IDLE;
                        lib_count       = 0;
                        lib_music_count = 0;
                        state = BROWSE_LIBRARY_PICKER;
                        dirty = 1;
                        GFX_sync();
                        continue;
                    } else {
                        selected_library_id = cfg->library_id;
                        all_albums_loaded  = 0;
                        all_albums_total   = 0;
                        all_albums_ls      = LOAD_IDLE;
                        all_album_selected = 0;
                        all_album_scroll   = 0;
                        all_albums_page_loading = false;
                        last_art_thumb[0] = '\0';
                        plex_art_clear();
                        state = BROWSE_ALL_ALBUMS;
                        dirty = 1;
                        GFX_sync();
                        continue;
                    }
                } else if (fav_idx >= 0 && lib_selected == fav_idx) {
                    if (!cfg->offline_mode) {
                        s_fav_count = plex_favorites_get(s_fav_tracks, PLEX_MAX_TRACKS);
                    } else {
                        /* Offline: downloaded favorites ∩ favorites.json, in-place filter */
                        int all = plex_downloads_get_tracks_for_album(
                                      PLEX_FAVORITES_SYNC_ALBUM_ID, s_fav_tracks, PLEX_MAX_TRACKS);
                        int n = 0;
                        for (int i = 0; i < all; i++) {
                            if (plex_favorites_contains(s_fav_tracks[i].rating_key))
                                s_fav_tracks[n++] = s_fav_tracks[i];
                        }
                        s_fav_count = n;
                    }
                    fav_selected  = 0;
                    fav_scroll    = 0;
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    state = BROWSE_FAVORITES;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
            } else if (PAD_justPressed(BTN_Y) && !cfg->offline_mode
                       && fav_idx >= 0 && lib_selected == fav_idx) {
                s_fav_count = plex_favorites_get(s_fav_tracks, PLEX_MAX_TRACKS);
                plex_downloads_sync_favorites(cfg, s_fav_tracks, s_fav_count);
                dirty = 1;
            } else if (PAD_justPressed(BTN_B)) {
                quit_confirm_active = true;
                dirty = 1;
            } else if (PAD_justPressed(BTN_SELECT)) {
                if (!cfg->offline_mode) {
                    /* Cancel any running load */
                    if (s_load.thread_started) {
                        s_load.cancel = true;
                        pthread_join(s_load.thread, NULL);
                        s_load.thread_started = false;
                        pthread_mutex_lock(&s_load.lock);
                        s_load.status = LOAD_IDLE;
                        pthread_mutex_unlock(&s_load.lock);
                    }
                    /* Switch to offline — home menu stays; user picks Artists or Albums */
                    mutable_cfg->offline_mode = true;
                    plex_config_save(mutable_cfg);
                    artists_ls       = LOAD_IDLE;
                    all_albums_ls    = LOAD_IDLE;
                    recent_albums_ls = LOAD_IDLE;
                    if (s_artists == NULL) {
                        s_artists     = malloc(PLEX_MAX_OFFLINE_ITEMS * sizeof(PlexArtist));
                        s_artists_cap = s_artists ? PLEX_MAX_OFFLINE_ITEMS : 0;
                    }
                    if (s_all_albums == NULL) {
                        s_all_albums     = malloc(PLEX_MAX_OFFLINE_ITEMS * sizeof(PlexAlbum));
                        s_all_albums_cap = s_all_albums ? PLEX_MAX_OFFLINE_ITEMS : 0;
                    }
                } else {
                    /* Switch back to online */
                    mutable_cfg->offline_mode = false;
                    plex_config_save(mutable_cfg);
                    artists_ls       = LOAD_IDLE;
                    all_albums_ls    = LOAD_IDLE;
                    recent_albums_ls = LOAD_IDLE;
                }
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* Redraw each frame while fav sync progress is updating */
            if (plex_downloads_album_status(PLEX_FAVORITES_SYNC_ALBUM_ID) == DL_STATUS_DOWNLOADING)
                dirty = 1;

            /* Render */
            if (dirty) {
                HomeLabelCtx hctx;
                hctx.bg_active         = bg_active;
                hctx.now_playing_label = now_playing_label;
                hctx.artists_idx       = artists_idx;
                hctx.albums_idx        = albums_idx;
                hctx.settings_idx      = settings_idx;
                hctx.recent_idx        = recent_idx;
                hctx.nowplay_idx       = nowplay_idx;
                hctx.fav_idx           = fav_idx;

                const char *home_header = cfg->offline_mode ? "Music (Offline)" : "Music (Online)";
                render_browse_screen(screen, home_header,
                                     lib_selected, &lib_scroll,
                                     home_item_count,
                                     NULL, NULL, NULL, NULL,
                                     home_get_label, &hctx,
                                     "SELECT", "QUIT", 0);
                /* Extra hint for offline mode toggle / favorites sync */
                if (!cfg->offline_mode) {
                    if (fav_idx >= 0 && lib_selected == fav_idx)
                        GFX_blitButtonGroup((char*[]){"Y", "OFFLINE SYNC", NULL}, 0, screen, 0);
                    else
                        GFX_blitButtonGroup((char*[]){"SELECT", "OFFLINE", NULL}, 0, screen, 0);
                } else {
                    GFX_blitButtonGroup((char*[]){"SELECT", "ONLINE", NULL}, 0, screen, 0);
                }
                if (quit_confirm_active) {
                    render_quit_confirm_dialog(screen);
                }
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_LIBRARY_PICKER
         * ============================================================== */
        } else if (state == BROWSE_LIBRARY_PICKER) {

            /* Kick on first entry */
            if (libs_ls == LOAD_IDLE) {
                PLEX_LOG("[Browse] Loading libraries from: %s\n", cfg->server_url);
                memset(libs, 0, sizeof(libs));
                lib_count = 0;
                browse_load_kick(BROWSE_LIBRARY_PICKER, cfg,
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
                    if (net_auto_retries < 3) {
                        net_auto_retries++;
                        dirty = 1;
                        GFX_sync();
                        continue;  /* ls == LOAD_IDLE -> re-kick next frame */
                    }
                    net_auto_retries = 0;
                    net_error_from = BROWSE_LIBRARY_PICKER;
                    net_error_back = BROWSE_LIBRARIES;
                    state = BROWSE_NET_ERROR;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }

                /* Auto-select if only one music library */
                if (lib_music_count == 1) {
                    int real_idx = lib_music_idx[0];
                    mutable_cfg->library_id = libs[real_idx].section_id;
                    strncpy(mutable_cfg->library_name, libs[real_idx].title,
                            sizeof(mutable_cfg->library_name) - 1);
                    mutable_cfg->library_name[sizeof(mutable_cfg->library_name) - 1] = '\0';
                    plex_config_save(mutable_cfg);
                    selected_library_id = cfg->library_id;
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
                net_auto_retries = 0;
                libs_ls = LOAD_READY;
            }

            if (libs_ls == LOAD_ERROR) {
                browse_load_join();
                libs_ls = LOAD_IDLE;
                if (net_auto_retries < 3) {
                    net_auto_retries++;
                    dirty = 1;
                    GFX_sync();
                    continue;  /* ls == LOAD_IDLE -> re-kick next frame */
                }
                net_auto_retries = 0;
                net_error_from = BROWSE_LIBRARY_PICKER;
                net_error_back = BROWSE_LIBRARIES;
                state = BROWSE_NET_ERROR;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* libs_ls == LOAD_READY — normal input + render */

            /* Clamp selection */
            if (lib_selected >= lib_music_count && lib_music_count > 0)
                lib_selected = lib_music_count - 1;

            /* Build filtered libs array for label callback */
            PlexLibrary music_libs[16];
            for (int i = 0; i < lib_music_count; i++)
                music_libs[i] = libs[lib_music_idx[i]];

            /* Input */
            if (PAD_justRepeated(BTN_UP)) {
                lib_selected = (lib_selected > 0) ? lib_selected - 1 : lib_music_count - 1;
                dirty = 1;
            } else if (PAD_justRepeated(BTN_DOWN)) {
                lib_selected = (lib_selected < lib_music_count - 1) ? lib_selected + 1 : 0;
                dirty = 1;
            } else if (PAD_justPressed(BTN_A) && lib_music_count > 0) {
                int real_idx = lib_music_idx[lib_selected];
                mutable_cfg->library_id = libs[real_idx].section_id;
                strncpy(mutable_cfg->library_name, libs[real_idx].title,
                        sizeof(mutable_cfg->library_name) - 1);
                mutable_cfg->library_name[sizeof(mutable_cfg->library_name) - 1] = '\0';
                plex_config_save(mutable_cfg);
                selected_library_id = cfg->library_id;
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
            } else if (PAD_justPressed(BTN_B)) {
                lib_selected = 0;
                state = BROWSE_LIBRARIES;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* Render */
            if (dirty) {
                render_browse_screen(screen, "Select Library",
                                     lib_selected, &lib_scroll,
                                     lib_music_count,
                                     NULL, NULL, NULL, NULL,
                                     libpick_get_label, &music_libs,
                                     "SELECT", "BACK", 0);
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
                if (cfg->offline_mode) {
                    artists_loaded = plex_downloads_get_artists(s_artists, s_artists_cap);
                    artists_total  = artists_loaded;
                    artists_ls     = LOAD_READY;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                /* Online: alloc initial buffer for first page */
                if (s_artists == NULL) {
                    s_artists     = malloc(150 * sizeof(PlexArtist));
                    s_artists_cap = s_artists ? 150 : 0;
                }
                if (s_artists == NULL) {
                    net_error_from = BROWSE_ARTISTS;
                    net_error_back = BROWSE_LIBRARIES;
                    state = BROWSE_NET_ERROR;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                PLEX_LOG("[Browse] Loading artists for library_id=%d\n", selected_library_id);
                s_load.list_cap = s_artists_cap;
                browse_load_kick(BROWSE_ARTISTS, cfg,
                                 selected_library_id, 0, 0,
                                 NULL, NULL, s_artists, NULL, NULL, NULL, NULL);
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

                /* Realloc to full totalSize now that we know it */
                if (artists_total > s_artists_cap) {
                    int new_cap = artists_total < PLEX_MAX_ITEMS ? artists_total : PLEX_MAX_ITEMS;
                    PlexArtist *p = realloc(s_artists, (size_t)new_cap * sizeof(PlexArtist));
                    if (p) { s_artists = p; s_artists_cap = new_cap; }
                    if (artists_total > s_artists_cap) artists_total = s_artists_cap;
                }

                dirty = 1;
                net_auto_retries = 0;
                artists_ls = LOAD_READY;
                GFX_sync();
                continue;
            }

            if (artists_ls == LOAD_ERROR) {
                browse_load_join();
                artists_ls = LOAD_IDLE;
                if (net_auto_retries < 3) {
                    net_auto_retries++;
                    dirty = 1;
                    GFX_sync();
                    continue;  /* ls == LOAD_IDLE -> re-kick next frame */
                }
                net_auto_retries = 0;
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
                    if (artists_loaded > s_artists_cap) artists_loaded = s_artists_cap;
                    if (new_count == 0) artists_total = artists_loaded; /* server lied */
                    artists_page_loading = false;
                    dirty = 1;
                    if (pending_r2_jump) {
                        if (artist_selected < artists_loaded) {
                            char orig = toupper((unsigned char)s_artists[artist_selected].title_sort[0]);
                            int i = artist_selected + 1;
                            while (i < artists_loaded &&
                                   toupper((unsigned char)s_artists[i].title_sort[0]) == orig)
                                i++;
                            if (i < artists_loaded) {
                                /* Found the next letter group in the newly loaded data */
                                artist_selected = i;
                                pending_r2_jump = false;
                            } else if (artists_loaded < artists_total && artists_loaded < s_artists_cap) {
                                /* Still not found; advance cursor to trigger the next lookahead page load */
                                artist_selected = artists_loaded - 1;
                                /* pending_r2_jump stays true — will retry when next page completes */
                            } else {
                                /* All pages loaded; letter doesn't exist */
                                pending_r2_jump = false;
                            }
                        } else {
                            pending_r2_jump = false;
                        }
                    }
                    /* Proactive preload: kick next page immediately if more remain */
                    if (artists_loaded < artists_total
                        && artists_loaded < s_artists_cap) {
                        s_load.artist_offset = artists_loaded;
                        s_load.list_cap      = s_artists_cap;
                        browse_load_kick(BROWSE_ARTISTS_PAGE, cfg,
                                         selected_library_id, 0, 0,
                                         NULL, NULL, s_artists, NULL, NULL, NULL, NULL);
                        artists_page_loading = true;
                    }
                } else if (ws == LOAD_ERROR) {
                    browse_load_join();
                    artists_page_loading = false;
                    pending_r2_jump = false;
                    /* silent fail — user can retry by scrolling */
                }
            }

            /* Count visible items (artists + optional "(Loading...)" sentinel) */
            int visible_items = artists_loaded + (artists_page_loading ? 1 : 0);

            /* Trigger background page fetch when cursor nears end */
            if (!artists_page_loading
                && artists_loaded < artists_total
                && artists_loaded < s_artists_cap
                && artist_selected >= artists_loaded - ARTIST_PAGE_LOOKAHEAD) {
                s_load.artist_offset = artists_loaded;
                s_load.list_cap = s_artists_cap;
                browse_load_kick(BROWSE_ARTISTS_PAGE, cfg,
                                 selected_library_id, 0, 0,
                                 NULL, NULL, s_artists, NULL, NULL, NULL, NULL);
                artists_page_loading = true;
            }

            /* Hold-acceleration tracking */
            if (!PAD_isPressed(BTN_UP))    s_hold_up_since   = 0;
            if (!PAD_isPressed(BTN_DOWN))  s_hold_down_since = 0;
            if (PAD_justPressed(BTN_UP))   s_hold_up_since   = SDL_GetTicks();
            if (PAD_justPressed(BTN_DOWN)) s_hold_down_since = SDL_GetTicks();

            /* Input */
            if (quit_confirm_active) {
                if (PAD_justPressed(BTN_A)) return MODULE_QUIT;
                if (PAD_justPressed(BTN_B)) { quit_confirm_active = false; dirty = 1; }
            } else if (PAD_justRepeated(BTN_UP)) {
                int step = scroll_step(s_hold_up_since);
                int prev = artist_selected;
                artist_selected = (artist_selected >= step) ? artist_selected - step : 0;
                if (artist_selected != prev) dirty = 1;
            } else if (PAD_justRepeated(BTN_DOWN)) {
                int step = scroll_step(s_hold_down_since);
                int prev = artist_selected;
                int cap = visible_items - 1;
                artist_selected = (artist_selected + step <= cap) ? artist_selected + step : cap;
                if (artist_selected != prev) dirty = 1;
            } else if (PAD_justRepeated(BTN_R2)) {
                int i = artist_selected + 1;
                while (i < artists_loaded &&
                       toupper((unsigned char)s_artists[i].title_sort[0]) ==
                       toupper((unsigned char)s_artists[artist_selected].title_sort[0]))
                    i++;
                if (i < artists_loaded) {
                    pending_r2_jump = false;
                    artist_selected = i;
                    dirty = 1;
                } else if (artists_loaded < artists_total && artists_loaded < s_artists_cap) {
                    /* Next letter is in an unloaded page */
                    pending_r2_jump = true;
                    if (!artists_page_loading) {
                        /* Kick the page load directly — don't wait for lookahead */
                        s_load.artist_offset = artists_loaded;
                        s_load.list_cap = s_artists_cap;
                        browse_load_kick(BROWSE_ARTISTS_PAGE, cfg,
                                         selected_library_id, 0, 0,
                                         NULL, NULL, s_artists, NULL, NULL, NULL, NULL);
                        artists_page_loading = true;
                    }
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_L2)) {
                char cur = toupper((unsigned char)s_artists[artist_selected].title_sort[0]);
                int start = artist_selected;
                while (start > 0 &&
                       toupper((unsigned char)s_artists[start - 1].title_sort[0]) == cur)
                    start--;
                if (start == artist_selected && start > 0) start--;
                cur = toupper((unsigned char)s_artists[start].title_sort[0]);
                while (start > 0 &&
                       toupper((unsigned char)s_artists[start - 1].title_sort[0]) == cur)
                    start--;
                if (start != artist_selected) { artist_selected = start; dirty = 1; }
            } else if (PAD_justPressed(BTN_A)) {
                if (artist_selected < artists_loaded) {
                    /* Cancel any in-flight page load before navigating away */
                    if (artists_page_loading) {
                        s_load.cancel = true;
                        artists_page_loading = false;
                    }
                    selected_artist_rating_key = s_artists[artist_selected].rating_key;
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
                {
                    /* Cancel any in-flight page load */
                    if (artists_page_loading) {
                        s_load.cancel = true;
                        artists_page_loading = false;
                    }
                    pending_r2_jump = false;
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    lib_selected = 0;
                    state = BROWSE_LIBRARIES;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
            } else if (PAD_justPressed(BTN_SELECT) && cfg->offline_mode) {
                mutable_cfg->offline_mode = false;
                plex_config_save(mutable_cfg);
                /* Reset libs so they reload from network */
                lib_count       = 0;
                lib_music_count = 0;
                lib_selected    = 0;
                lib_scroll      = 0;
                libs_ls         = LOAD_IDLE;
                all_albums_ls      = LOAD_IDLE;
                all_albums_loaded  = 0;
                all_albums_total   = 0;
                all_album_selected = 0;
                all_album_scroll   = 0;
                all_albums_page_loading = false;
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
                actx.artists      = s_artists;
                actx.loaded       = artists_loaded;
                actx.total        = artists_total;
                actx.page_loading = artists_page_loading;

                const char *art_line1 = (artist_selected < artists_loaded)
                    ? s_artists[artist_selected].title : NULL;

                const char *artists_header = cfg->offline_mode ? "Artists (Offline)" : "Artists";
                render_browse_screen(screen, artists_header,
                                     artist_selected, &artist_scroll,
                                     visible_items,
                                     art_line1, NULL, NULL, plex_art_get(),
                                     artist_get_label, &actx,
                                     "SELECT", cfg->offline_mode ? "QUIT" : "BACK", 0);
                if (cfg->offline_mode)
                    GFX_blitButtonGroup((char*[]){"SELECT", "ONLINE", NULL}, 0, screen, 0);
                if (quit_confirm_active)
                    render_quit_confirm_dialog(screen);
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
                                      selected_artist_rating_key, albums, PLEX_MAX_ARTIST_ALBUMS);
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
                net_auto_retries = 0;
                albums_ls = LOAD_READY;
                GFX_sync();
                continue;
            }

            if (albums_ls == LOAD_ERROR) {
                browse_load_join();
                albums_ls = LOAD_IDLE;
                if (net_auto_retries < 3) {
                    net_auto_retries++;
                    dirty = 1;
                    GFX_sync();
                    continue;  /* ls == LOAD_IDLE -> re-kick next frame */
                }
                net_auto_retries = 0;
                net_error_from = BROWSE_ALBUMS;
                net_error_back = BROWSE_ARTISTS;
                state = BROWSE_NET_ERROR;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* albums_ls == LOAD_READY — normal input + render */

            /* Hold-acceleration tracking */
            if (!PAD_isPressed(BTN_UP))    s_hold_up_since   = 0;
            if (!PAD_isPressed(BTN_DOWN))  s_hold_down_since = 0;
            if (PAD_justPressed(BTN_UP))   s_hold_up_since   = SDL_GetTicks();
            if (PAD_justPressed(BTN_DOWN)) s_hold_down_since = SDL_GetTicks();

            /* Input */
            if (delete_confirm_active) {
                if (PAD_justPressed(BTN_A)) {
                    plex_downloads_delete_album(delete_confirm_album_id);
                    album_count = plex_downloads_get_albums_for_artist(
                        s_artists[artist_selected].rating_key, albums, PLEX_MAX_ARTIST_ALBUMS);
                    if (album_count > 0 && album_selected >= album_count)
                        album_selected = album_count - 1;
                    delete_confirm_active = false;
                    album_scroll = 0;
                    if (album_count > 0) {
                        if (albums[album_selected].thumb[0]) {
                            snprintf(last_art_thumb, sizeof(last_art_thumb),
                                     "%s", albums[album_selected].thumb);
                            plex_art_fetch(cfg, albums[album_selected].thumb);
                        }
                    } else {
                        plex_art_clear();
                        last_art_thumb[0] = '\0';
                    }
                } else if (PAD_justPressed(BTN_B)) {
                    delete_confirm_active = false;
                }
                dirty = 1;
            } else if (PAD_justRepeated(BTN_UP)) {
                int step = scroll_step(s_hold_up_since);
                int prev = album_selected;
                album_selected = (album_selected >= step) ? album_selected - step : 0;
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
                int step = scroll_step(s_hold_down_since);
                int prev = album_selected;
                int cap = album_count - 1;
                album_selected = (album_selected + step <= cap) ? album_selected + step : cap;
                if (album_selected != prev) {
                    if (albums[album_selected].thumb[0] &&
                        strcmp(albums[album_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", albums[album_selected].thumb);
                        plex_art_fetch(cfg, albums[album_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_R2) && album_count > 0) {
                int i = album_selected + 1;
                while (i < album_count &&
                       strcmp(albums[i].year, albums[album_selected].year) == 0)
                    i++;
                if (i < album_count) { album_selected = i; dirty = 1; }
            } else if (PAD_justRepeated(BTN_L2) && album_count > 0) {
                const char *cur = albums[album_selected].year;
                int start = album_selected;
                while (start > 0 &&
                       strcmp(albums[start - 1].year, cur) == 0)
                    start--;
                if (start == album_selected && start > 0) start--;
                cur = albums[start].year;
                while (start > 0 &&
                       strcmp(albums[start - 1].year, cur) == 0)
                    start--;
                if (start != album_selected) { album_selected = start; dirty = 1; }
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
                tracks_back_state = BROWSE_ALBUMS;
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
                    s_artists[artist_selected].rating_key,
                    s_artists[artist_selected].title,
                    albums[album_selected].thumb,
                    albums[album_selected].year);
                dirty = 1;
            } else if (PAD_justPressed(BTN_Y) && cfg->offline_mode && album_count > 0) {
                delete_confirm_album_id = albums[album_selected].rating_key;
                delete_confirm_active   = true;
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

                const char *art_line1 = (album_count > 0 && albums[album_selected].artist[0])
                    ? albums[album_selected].artist : NULL;
                const char *art_line2 = (album_count > 0)
                    ? albums[album_selected].title : NULL;
                const char *art_line3 = (album_count > 0 && albums[album_selected].year[0])
                    ? albums[album_selected].year : NULL;

                render_browse_screen(screen, "Albums",
                                     album_selected, &album_scroll,
                                     album_count,
                                     art_line1, art_line2, art_line3, plex_art_get(),
                                     album_get_label, &alctx,
                                     "SELECT", "BACK", 1);
                if (!cfg->offline_mode)
                    GFX_blitButtonGroup((char*[]){"Y", "DOWNLOAD", NULL}, 0, screen, 0);
                else if (album_count > 0)
                    GFX_blitButtonGroup((char*[]){"Y", "DELETE", NULL}, 0, screen, 0);
                if (delete_confirm_active)
                    render_delete_confirm_dialog(screen);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_ALL_ALBUMS
         * ============================================================== */
        } else if (state == BROWSE_ALL_ALBUMS) {

            /* Kick on first entry */
            if (all_albums_ls == LOAD_IDLE) {
                if (cfg->offline_mode) {
                    all_albums_loaded = plex_downloads_get_all_albums(s_all_albums, s_all_albums_cap);
                    all_albums_total  = all_albums_loaded;
                    all_albums_ls     = LOAD_READY;
                    if (all_albums_loaded > 0 && s_all_albums[0].thumb[0]) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", s_all_albums[0].thumb);
                        plex_art_fetch(cfg, s_all_albums[0].thumb);
                    }
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                /* Online: alloc initial buffer for first page */
                if (s_all_albums == NULL) {
                    s_all_albums     = malloc(150 * sizeof(PlexAlbum));
                    s_all_albums_cap = s_all_albums ? 150 : 0;
                }
                if (s_all_albums == NULL) {
                    net_error_from = BROWSE_ALL_ALBUMS;
                    net_error_back = BROWSE_LIBRARIES;
                    state = BROWSE_NET_ERROR;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                s_load.list_cap = s_all_albums_cap;
                browse_load_kick(BROWSE_ALL_ALBUMS, cfg,
                                 selected_library_id, 0, 0,
                                 NULL, NULL, NULL, s_all_albums, NULL, NULL, NULL);
                all_albums_ls = LOAD_RUNNING;
            }

            /* Poll running load */
            if (all_albums_ls == LOAD_RUNNING) {
                LoadState ws = browse_load_poll();
                if (ws == LOAD_DONE || ws == LOAD_ERROR)
                    all_albums_ls = ws;
                else {
                    render_loading(screen, true);
                    if (PAD_justPressed(BTN_B)) {
                        s_load.cancel = true;
                        all_albums_ls = LOAD_IDLE;
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
            if (all_albums_ls == LOAD_DONE) {
                browse_load_join();
                all_albums_loaded = s_load.all_albums_page.count;
                all_albums_total  = s_load.all_albums_page.total;
                PLEX_LOG("[Browse] Got %d / %d all_albums\n", all_albums_loaded, all_albums_total);

                /* Realloc to full totalSize now that we know it */
                if (all_albums_total > s_all_albums_cap) {
                    int new_cap = all_albums_total < PLEX_MAX_ITEMS ? all_albums_total : PLEX_MAX_ITEMS;
                    PlexAlbum *p = realloc(s_all_albums, (size_t)new_cap * sizeof(PlexAlbum));
                    if (p) { s_all_albums = p; s_all_albums_cap = new_cap; }
                    if (all_albums_total > s_all_albums_cap) all_albums_total = s_all_albums_cap;
                }

                if (all_albums_loaded > 0 && s_all_albums[0].thumb[0]) {
                    snprintf(last_art_thumb, sizeof(last_art_thumb),
                             "%s", s_all_albums[0].thumb);
                    plex_art_fetch(cfg, s_all_albums[0].thumb);
                }
                dirty = 1;
                net_auto_retries = 0;
                all_albums_ls = LOAD_READY;
                GFX_sync();
                continue;
            }

            if (all_albums_ls == LOAD_ERROR) {
                browse_load_join();
                all_albums_ls = LOAD_IDLE;
                if (net_auto_retries < 3) {
                    net_auto_retries++;
                    dirty = 1;
                    GFX_sync();
                    continue;  /* ls == LOAD_IDLE -> re-kick next frame */
                }
                net_auto_retries = 0;
                net_error_from = BROWSE_ALL_ALBUMS;
                net_error_back = BROWSE_LIBRARIES;
                state = BROWSE_NET_ERROR;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* all_albums_ls == LOAD_READY — normal input + render */

            /* Collect pagination results */
            if (all_albums_page_loading) {
                LoadState ws = browse_load_poll();
                if (ws == LOAD_DONE) {
                    browse_load_join();
                    int new_count = s_load.all_albums_page.count;
                    all_albums_loaded += new_count;
                    if (all_albums_loaded > s_all_albums_cap) all_albums_loaded = s_all_albums_cap;
                    all_albums_total = s_load.all_albums_page.total;
                    if (new_count == 0) all_albums_total = all_albums_loaded; /* server lied */
                    all_albums_page_loading = false;
                    dirty = 1;
                    if (pending_r2_jump_all) {
                        if (all_album_selected < all_albums_loaded) {
                            const char *orig_year = s_all_albums[all_album_selected].year;
                            int i = all_album_selected + 1;
                            while (i < all_albums_loaded &&
                                   strcmp(s_all_albums[i].year, orig_year) == 0)
                                i++;
                            if (i < all_albums_loaded) {
                                /* Found the next year group in the newly loaded data */
                                all_album_selected = i;
                                if (s_all_albums[all_album_selected].thumb[0] &&
                                    strcmp(s_all_albums[all_album_selected].thumb, last_art_thumb) != 0) {
                                    snprintf(last_art_thumb, sizeof(last_art_thumb),
                                             "%s", s_all_albums[all_album_selected].thumb);
                                    plex_art_fetch(cfg, s_all_albums[all_album_selected].thumb);
                                }
                                pending_r2_jump_all = false;
                            } else if (all_albums_loaded < all_albums_total && all_albums_loaded < s_all_albums_cap) {
                                /* Still not found; advance cursor to trigger the next lookahead page load */
                                all_album_selected = all_albums_loaded - 1;
                                /* pending_r2_jump_all stays true — will retry when next page completes */
                            } else {
                                /* All pages loaded; year doesn't exist */
                                pending_r2_jump_all = false;
                            }
                        } else {
                            pending_r2_jump_all = false;
                        }
                    }
                    /* Proactive preload: kick next page immediately if more remain */
                    if (all_albums_loaded < all_albums_total
                        && all_albums_loaded < s_all_albums_cap) {
                        s_load.all_albums_offset = all_albums_loaded;
                        s_load.list_cap          = s_all_albums_cap;
                        browse_load_kick(BROWSE_ALL_ALBUMS_PAGE, cfg,
                                         selected_library_id, 0, 0,
                                         NULL, NULL, NULL, s_all_albums, NULL, NULL, NULL);
                        all_albums_page_loading = true;
                    }
                } else if (ws == LOAD_ERROR) {
                    browse_load_join();
                    all_albums_page_loading = false;
                    pending_r2_jump_all = false;
                    /* silent fail — user can retry by scrolling */
                }
            }

            /* Count visible items (albums + optional "(Loading...)" sentinel) */
            int aa_visible = all_albums_loaded + (all_albums_page_loading ? 1 : 0);

            /* Trigger background page fetch when cursor nears end */
            if (!all_albums_page_loading
                && all_albums_loaded < all_albums_total
                && all_albums_loaded < s_all_albums_cap
                && all_album_selected >= all_albums_loaded - ARTIST_PAGE_LOOKAHEAD) {
                s_load.all_albums_offset = all_albums_loaded;
                s_load.list_cap = s_all_albums_cap;
                browse_load_kick(BROWSE_ALL_ALBUMS_PAGE, cfg,
                                 selected_library_id, 0, 0,
                                 NULL, NULL, NULL, s_all_albums, NULL, NULL, NULL);
                all_albums_page_loading = true;
            }

            /* Hold-acceleration tracking */
            if (!PAD_isPressed(BTN_UP))    s_hold_up_since   = 0;
            if (!PAD_isPressed(BTN_DOWN))  s_hold_down_since = 0;
            if (PAD_justPressed(BTN_UP))   s_hold_up_since   = SDL_GetTicks();
            if (PAD_justPressed(BTN_DOWN)) s_hold_down_since = SDL_GetTicks();

            /* Input */
            if (delete_confirm_active) {
                if (PAD_justPressed(BTN_A)) {
                    plex_downloads_delete_album(delete_confirm_album_id);
                    all_albums_loaded = plex_downloads_get_all_albums(s_all_albums, s_all_albums_cap);
                    all_albums_total  = all_albums_loaded;
                    if (all_albums_loaded > 0 && all_album_selected >= all_albums_loaded)
                        all_album_selected = all_albums_loaded - 1;
                    delete_confirm_active = false;
                    all_album_scroll = 0;
                    if (all_albums_loaded > 0) {
                        if (s_all_albums[all_album_selected].thumb[0]) {
                            snprintf(last_art_thumb, sizeof(last_art_thumb),
                                     "%s", s_all_albums[all_album_selected].thumb);
                            plex_art_fetch(cfg, s_all_albums[all_album_selected].thumb);
                        }
                    } else {
                        plex_art_clear();
                        last_art_thumb[0] = '\0';
                    }
                } else if (PAD_justPressed(BTN_B)) {
                    delete_confirm_active = false;
                }
                dirty = 1;
            } else if (PAD_justRepeated(BTN_UP)) {
                int step = scroll_step(s_hold_up_since);
                int prev = all_album_selected;
                all_album_selected = (all_album_selected >= step) ? all_album_selected - step : 0;
                if (all_album_selected != prev) {
                    if (all_album_selected < all_albums_loaded &&
                        s_all_albums[all_album_selected].thumb[0] &&
                        strcmp(s_all_albums[all_album_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", s_all_albums[all_album_selected].thumb);
                        plex_art_fetch(cfg, s_all_albums[all_album_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_DOWN)) {
                int step = scroll_step(s_hold_down_since);
                int prev = all_album_selected;
                int cap = aa_visible - 1;
                all_album_selected = (all_album_selected + step <= cap) ? all_album_selected + step : cap;
                if (all_album_selected != prev) {
                    if (all_album_selected < all_albums_loaded &&
                        s_all_albums[all_album_selected].thumb[0] &&
                        strcmp(s_all_albums[all_album_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", s_all_albums[all_album_selected].thumb);
                        plex_art_fetch(cfg, s_all_albums[all_album_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_R2) && all_albums_loaded > 0
                       && all_album_selected < all_albums_loaded) {
                int i = all_album_selected + 1;
                while (i < all_albums_loaded &&
                       strcmp(s_all_albums[i].year, s_all_albums[all_album_selected].year) == 0)
                    i++;
                if (i < all_albums_loaded) {
                    pending_r2_jump_all = false;
                    all_album_selected = i;
                    if (s_all_albums[all_album_selected].thumb[0] &&
                        strcmp(s_all_albums[all_album_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", s_all_albums[all_album_selected].thumb);
                        plex_art_fetch(cfg, s_all_albums[all_album_selected].thumb);
                    }
                    dirty = 1;
                } else if (all_albums_loaded < all_albums_total && all_albums_loaded < s_all_albums_cap) {
                    /* Next year group is in an unloaded page */
                    pending_r2_jump_all = true;
                    if (!all_albums_page_loading) {
                        /* Kick the page load directly — don't wait for lookahead */
                        s_load.all_albums_offset = all_albums_loaded;
                        s_load.list_cap = s_all_albums_cap;
                        browse_load_kick(BROWSE_ALL_ALBUMS_PAGE, cfg,
                                         selected_library_id, 0, 0,
                                         NULL, NULL, NULL, s_all_albums, NULL, NULL, NULL);
                        all_albums_page_loading = true;
                    }
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_L2) && all_albums_loaded > 0
                       && all_album_selected < all_albums_loaded) {
                const char *cur = s_all_albums[all_album_selected].year;
                int start = all_album_selected;
                while (start > 0 &&
                       strcmp(s_all_albums[start - 1].year, cur) == 0)
                    start--;
                if (start == all_album_selected && start > 0) start--;
                cur = s_all_albums[start].year;
                while (start > 0 &&
                       strcmp(s_all_albums[start - 1].year, cur) == 0)
                    start--;
                if (start != all_album_selected) {
                    all_album_selected = start;
                    if (s_all_albums[all_album_selected].thumb[0] &&
                        strcmp(s_all_albums[all_album_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", s_all_albums[all_album_selected].thumb);
                        plex_art_fetch(cfg, s_all_albums[all_album_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_A) && all_album_selected < all_albums_loaded) {
                /* Cancel any in-flight page load before navigating away */
                if (all_albums_page_loading) {
                    s_load.cancel = true;
                    all_albums_page_loading = false;
                }
                selected_album_rating_key = s_all_albums[all_album_selected].rating_key;
                snprintf(selected_album_thumb, sizeof(selected_album_thumb),
                         "%s", s_all_albums[all_album_selected].thumb);
                tracks_ls      = LOAD_IDLE;
                track_selected = 0;
                track_scroll   = 0;
                tracks_back_state = BROWSE_ALL_ALBUMS;
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_TRACKS;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_B)) {
                /* Cancel any in-flight page load */
                if (all_albums_page_loading) {
                    s_load.cancel = true;
                    all_albums_page_loading = false;
                }
                pending_r2_jump_all = false;
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_LIBRARIES;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_Y) && !cfg->offline_mode && all_albums_loaded > 0
                       && all_album_selected < all_albums_loaded) {
                plex_downloads_queue_album(mutable_cfg,
                    s_all_albums[all_album_selected].rating_key,
                    s_all_albums[all_album_selected].title,
                    s_all_albums[all_album_selected].artist_id,
                    s_all_albums[all_album_selected].artist,
                    s_all_albums[all_album_selected].thumb,
                    s_all_albums[all_album_selected].year);
                dirty = 1;
            } else if (PAD_justPressed(BTN_Y) && cfg->offline_mode
                       && all_albums_loaded > 0 && all_album_selected < all_albums_loaded) {
                delete_confirm_album_id = s_all_albums[all_album_selected].rating_key;
                delete_confirm_active   = true;
                dirty = 1;
            }

            /* Poll art async */
            if (plex_art_is_fetching()) dirty = 1;

            /* Render */
            if (dirty) {
                AlbumLabelCtx alctx;
                alctx.albums      = s_all_albums;
                alctx.count       = all_albums_loaded;
                alctx.show_status = !cfg->offline_mode;

                const char *art_line1 = (all_album_selected < all_albums_loaded && s_all_albums[all_album_selected].artist[0])
                    ? s_all_albums[all_album_selected].artist : NULL;
                const char *art_line2 = (all_album_selected < all_albums_loaded)
                    ? s_all_albums[all_album_selected].title : NULL;
                const char *art_line3 = (all_album_selected < all_albums_loaded && s_all_albums[all_album_selected].year[0])
                    ? s_all_albums[all_album_selected].year : NULL;

                const char *aa_header = cfg->offline_mode ? "Albums (Offline)" : "Albums";
                render_browse_screen(screen, aa_header,
                                     all_album_selected, &all_album_scroll,
                                     aa_visible,
                                     art_line1, art_line2, art_line3, plex_art_get(),
                                     album_get_label, &alctx,
                                     "SELECT", "BACK", 1);
                if (!cfg->offline_mode)
                    GFX_blitButtonGroup((char*[]){"Y", "DOWNLOAD", NULL}, 0, screen, 0);
                else if (all_albums_loaded > 0)
                    GFX_blitButtonGroup((char*[]){"Y", "DELETE", NULL}, 0, screen, 0);
                if (delete_confirm_active)
                    render_delete_confirm_dialog(screen);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_RECENT_ALBUMS
         * ============================================================== */
        } else if (state == BROWSE_RECENT_ALBUMS) {

            /* Kick on first entry (online only — state is never entered in offline mode) */
            if (recent_albums_ls == LOAD_IDLE) {
                s_load.list_cap = PLEX_MAX_ARTIST_ALBUMS;
                browse_load_kick(BROWSE_RECENT_ALBUMS, cfg,
                                 selected_library_id, 0, 0,
                                 NULL, NULL, NULL, recent_albums, &recent_albums_count, NULL, NULL);
                recent_albums_ls = LOAD_RUNNING;
            }

            /* Poll running load */
            if (recent_albums_ls == LOAD_RUNNING) {
                LoadState ws = browse_load_poll();
                if (ws == LOAD_DONE || ws == LOAD_ERROR)
                    recent_albums_ls = ws;
                else {
                    render_loading(screen, true);
                    if (PAD_justPressed(BTN_B)) {
                        s_load.cancel = true;
                        recent_albums_ls = LOAD_IDLE;
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
            if (recent_albums_ls == LOAD_DONE) {
                browse_load_join();
                PLEX_LOG("[Browse] Got %d recent albums\n", recent_albums_count);
                if (recent_albums_count > 0 && recent_albums[0].thumb[0]) {
                    snprintf(last_art_thumb, sizeof(last_art_thumb),
                             "%s", recent_albums[0].thumb);
                    plex_art_fetch(cfg, recent_albums[0].thumb);
                }
                dirty = 1;
                net_auto_retries = 0;
                recent_albums_ls = LOAD_READY;
                GFX_sync();
                continue;
            }

            if (recent_albums_ls == LOAD_ERROR) {
                browse_load_join();
                recent_albums_ls = LOAD_IDLE;
                if (net_auto_retries < 3) {
                    net_auto_retries++;
                    dirty = 1;
                    GFX_sync();
                    continue;  /* ls == LOAD_IDLE -> re-kick next frame */
                }
                net_auto_retries = 0;
                net_error_from = BROWSE_RECENT_ALBUMS;
                net_error_back = BROWSE_LIBRARIES;
                state = BROWSE_NET_ERROR;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* recent_albums_ls == LOAD_READY — normal input + render */

            /* Hold-acceleration tracking */
            if (!PAD_isPressed(BTN_UP))    s_hold_up_since   = 0;
            if (!PAD_isPressed(BTN_DOWN))  s_hold_down_since = 0;
            if (PAD_justPressed(BTN_UP))   s_hold_up_since   = SDL_GetTicks();
            if (PAD_justPressed(BTN_DOWN)) s_hold_down_since = SDL_GetTicks();

            /* Input */
            if (PAD_justRepeated(BTN_UP)) {
                int step = scroll_step(s_hold_up_since);
                int prev = recent_album_selected;
                recent_album_selected = (recent_album_selected >= step) ? recent_album_selected - step : 0;
                if (recent_album_selected != prev) {
                    if (recent_albums[recent_album_selected].thumb[0] &&
                        strcmp(recent_albums[recent_album_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", recent_albums[recent_album_selected].thumb);
                        plex_art_fetch(cfg, recent_albums[recent_album_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_DOWN)) {
                int step = scroll_step(s_hold_down_since);
                int prev = recent_album_selected;
                int cap = recent_albums_count - 1;
                recent_album_selected = (recent_album_selected + step <= cap) ? recent_album_selected + step : cap;
                if (recent_album_selected != prev) {
                    if (recent_albums[recent_album_selected].thumb[0] &&
                        strcmp(recent_albums[recent_album_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", recent_albums[recent_album_selected].thumb);
                        plex_art_fetch(cfg, recent_albums[recent_album_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_A) && recent_albums_count > 0) {
                selected_album_rating_key = recent_albums[recent_album_selected].rating_key;
                snprintf(selected_album_thumb, sizeof(selected_album_thumb),
                         "%s", recent_albums[recent_album_selected].thumb);
                tracks_ls      = LOAD_IDLE;
                track_selected = 0;
                track_scroll   = 0;
                tracks_back_state = BROWSE_RECENT_ALBUMS;
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_TRACKS;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_B)) {
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_LIBRARIES;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_Y) && recent_albums_count > 0) {
                plex_downloads_queue_album(mutable_cfg,
                    recent_albums[recent_album_selected].rating_key,
                    recent_albums[recent_album_selected].title,
                    recent_albums[recent_album_selected].artist_id,
                    recent_albums[recent_album_selected].artist,
                    recent_albums[recent_album_selected].thumb,
                    recent_albums[recent_album_selected].year);
                dirty = 1;
            }

            /* Poll art async */
            if (plex_art_is_fetching()) dirty = 1;

            /* Render */
            if (dirty) {
                AlbumLabelCtx ralctx;
                ralctx.albums      = recent_albums;
                ralctx.count       = recent_albums_count;
                ralctx.show_status = !cfg->offline_mode;

                const char *art_line1 = (recent_albums_count > 0 && recent_albums[recent_album_selected].artist[0])
                    ? recent_albums[recent_album_selected].artist : NULL;
                const char *art_line2 = (recent_albums_count > 0)
                    ? recent_albums[recent_album_selected].title : NULL;
                const char *art_line3 = (recent_albums_count > 0 && recent_albums[recent_album_selected].year[0])
                    ? recent_albums[recent_album_selected].year : NULL;

                render_browse_screen(screen, "Recently Added",
                                     recent_album_selected, &recent_album_scroll,
                                     recent_albums_count,
                                     art_line1, art_line2, art_line3, plex_art_get(),
                                     album_get_label, &ralctx,
                                     "SELECT", "BACK", 1);
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
                                      selected_album_rating_key, tracks, PLEX_MAX_TRACKS);
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
                        if (tracks_back_state == BROWSE_ALL_ALBUMS) {
                            if (all_album_selected < all_albums_loaded &&
                                s_all_albums[all_album_selected].thumb[0]) {
                                snprintf(last_art_thumb, sizeof(last_art_thumb),
                                         "%s", s_all_albums[all_album_selected].thumb);
                                plex_art_fetch(cfg, s_all_albums[all_album_selected].thumb);
                            }
                        } else if (tracks_back_state == BROWSE_RECENT_ALBUMS) {
                            if (recent_albums_count > 0 &&
                                recent_albums[recent_album_selected].thumb[0]) {
                                snprintf(last_art_thumb, sizeof(last_art_thumb),
                                         "%s", recent_albums[recent_album_selected].thumb);
                                plex_art_fetch(cfg, recent_albums[recent_album_selected].thumb);
                            }
                        } else {
                            if (album_selected < album_count && albums[album_selected].thumb[0]) {
                                snprintf(last_art_thumb, sizeof(last_art_thumb),
                                         "%s", albums[album_selected].thumb);
                                plex_art_fetch(cfg, albums[album_selected].thumb);
                            }
                        }
                        state = tracks_back_state;
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
                net_auto_retries = 0;
                tracks_ls = LOAD_READY;
                GFX_sync();
                continue;
            }

            if (tracks_ls == LOAD_ERROR) {
                browse_load_join();
                tracks_ls = LOAD_IDLE;
                if (net_auto_retries < 3) {
                    net_auto_retries++;
                    dirty = 1;
                    GFX_sync();
                    continue;  /* ls == LOAD_IDLE -> re-kick next frame */
                }
                net_auto_retries = 0;
                net_error_from = BROWSE_TRACKS;
                net_error_back = tracks_back_state;
                state = BROWSE_NET_ERROR;
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* tracks_ls == LOAD_READY — normal input + render */

            /* Hold-acceleration tracking */
            if (!PAD_isPressed(BTN_UP))    s_hold_up_since   = 0;
            if (!PAD_isPressed(BTN_DOWN))  s_hold_down_since = 0;
            if (PAD_justPressed(BTN_UP))   s_hold_up_since   = SDL_GetTicks();
            if (PAD_justPressed(BTN_DOWN)) s_hold_down_since = SDL_GetTicks();

            /* Input */
            if (PAD_justRepeated(BTN_UP)) {
                int step = scroll_step(s_hold_up_since);
                int prev = track_selected;
                track_selected = (track_selected >= step) ? track_selected - step : 0;
                if (track_selected != prev) dirty = 1;
            } else if (PAD_justRepeated(BTN_DOWN)) {
                int step = scroll_step(s_hold_down_since);
                int prev = track_selected;
                int cap = track_count - 1;
                track_selected = (track_selected + step <= cap) ? track_selected + step : cap;
                if (track_selected != prev) dirty = 1;
            } else if (PAD_justPressed(BTN_A) && track_count > 0) {
                Background_setActive(BG_NONE);
                plex_queue_set(cfg, tracks, track_count, track_selected);
                return MODULE_PLAYER;
            } else if (PAD_justPressed(BTN_B)) {
                last_art_thumb[0] = '\0';
                plex_art_clear();
                /* Restore art for the currently selected album */
                if (tracks_back_state == BROWSE_ALL_ALBUMS) {
                    if (all_album_selected < all_albums_loaded &&
                        s_all_albums[all_album_selected].thumb[0]) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", s_all_albums[all_album_selected].thumb);
                        plex_art_fetch(cfg, s_all_albums[all_album_selected].thumb);
                    }
                } else if (tracks_back_state == BROWSE_RECENT_ALBUMS) {
                    if (recent_albums_count > 0 &&
                        recent_albums[recent_album_selected].thumb[0]) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", recent_albums[recent_album_selected].thumb);
                        plex_art_fetch(cfg, recent_albums[recent_album_selected].thumb);
                    }
                } else {
                    if (album_selected < album_count && albums[album_selected].thumb[0]) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", albums[album_selected].thumb);
                        plex_art_fetch(cfg, albums[album_selected].thumb);
                    }
                }
                state = tracks_back_state;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_Y) && track_count > 0) {
                plex_favorites_toggle(&tracks[track_selected]);
                dirty = 1;
            }

            /* Poll art async */
            if (plex_art_is_fetching()) dirty = 1;

            /* Render */
            if (dirty) {
                TrackLabelCtx tctx;
                tctx.tracks         = tracks;
                tctx.count          = track_count;
                tctx.show_favorites = !cfg->offline_mode;

                /* Album title + year as art metadata */
                char art_line2[PLEX_MAX_STR + 8] = "";
                /* Find album name from track info if available */
                const char *art_line1 = (track_count > 0) ? tracks[0].album : NULL;
                if (tracks_back_state == BROWSE_ALL_ALBUMS) {
                    if (all_album_selected < all_albums_loaded) {
                        art_line1 = s_all_albums[all_album_selected].title;
                        if (s_all_albums[all_album_selected].year[0])
                            snprintf(art_line2, sizeof(art_line2), "%s",
                                     s_all_albums[all_album_selected].year);
                    }
                } else if (tracks_back_state == BROWSE_RECENT_ALBUMS) {
                    if (recent_albums_count > 0) {
                        art_line1 = recent_albums[recent_album_selected].title;
                        if (recent_albums[recent_album_selected].year[0])
                            snprintf(art_line2, sizeof(art_line2), "%s",
                                     recent_albums[recent_album_selected].year);
                    }
                } else if (track_count > 0 && album_count > 0) {
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
                                     NULL,
                                     plex_art_get(),
                                     track_get_label, &tctx,
                                     "PLAY", "BACK", 0);
                if (!cfg->offline_mode)
                    GFX_blitButtonGroup((char*[]){"Y", "FAVORITE", NULL}, 0, screen, 0);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_FAVORITES
         * ============================================================== */
        } else if (state == BROWSE_FAVORITES) {

            /* Hold-acceleration */
            if (!PAD_isPressed(BTN_UP))   s_hold_up_since   = 0;
            if (!PAD_isPressed(BTN_DOWN)) s_hold_down_since = 0;
            if (PAD_justPressed(BTN_UP))  s_hold_up_since   = SDL_GetTicks();
            if (PAD_justPressed(BTN_DOWN))s_hold_down_since = SDL_GetTicks();

            /* Input */
            if (PAD_justRepeated(BTN_UP)) {
                int step = scroll_step(s_hold_up_since);
                int prev = fav_selected;
                fav_selected = (fav_selected >= step) ? fav_selected - step : 0;
                if (fav_selected != prev) dirty = 1;
            } else if (PAD_justRepeated(BTN_DOWN)) {
                int step = scroll_step(s_hold_down_since);
                int prev = fav_selected;
                int cap  = s_fav_count - 1;
                fav_selected = (fav_selected + step <= cap) ? fav_selected + step : (cap >= 0 ? cap : 0);
                if (fav_selected != prev) dirty = 1;
            } else if (PAD_justPressed(BTN_A) && s_fav_count > 0) {
                Background_setActive(BG_NONE);
                plex_queue_set(cfg, s_fav_tracks, s_fav_count, fav_selected);
                return MODULE_PLAYER;
            } else if (PAD_justPressed(BTN_B)) {
                state = BROWSE_LIBRARIES;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_Y) && s_fav_count > 0) {
                plex_favorites_toggle(&s_fav_tracks[fav_selected]);
                if (!cfg->offline_mode) {
                    s_fav_count = plex_favorites_get(s_fav_tracks, PLEX_MAX_TRACKS);
                } else {
                    int all = plex_downloads_get_tracks_for_album(
                                  PLEX_FAVORITES_SYNC_ALBUM_ID, s_fav_tracks, PLEX_MAX_TRACKS);
                    int n = 0;
                    for (int i = 0; i < all; i++) {
                        if (plex_favorites_contains(s_fav_tracks[i].rating_key))
                            s_fav_tracks[n++] = s_fav_tracks[i];
                    }
                    s_fav_count = n;
                }
                if (fav_selected >= s_fav_count)
                    fav_selected = s_fav_count > 0 ? s_fav_count - 1 : 0;
                fav_scroll = 0;
                dirty = 1;
            }

            /* Render */
            if (dirty) {
                TrackLabelCtx tctx;
                tctx.tracks         = s_fav_tracks;
                tctx.count          = s_fav_count;
                tctx.show_favorites = false;  /* no hearts — every item is a favorite */

                render_browse_screen(screen, "Favorite Tracks",
                                     fav_selected, &fav_scroll,
                                     s_fav_count,
                                     NULL, NULL, NULL, NULL,
                                     track_get_label, &tctx,
                                     "PLAY", "BACK", 0);
                GFX_blitButtonGroup((char*[]){"Y", "REMOVE", NULL}, 0, screen, 0);
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
                    case BROWSE_LIBRARIES:      libs_ls    = LOAD_IDLE; break;
                    case BROWSE_LIBRARY_PICKER: libs_ls    = LOAD_IDLE; break;
                    case BROWSE_ARTISTS:        artists_ls = LOAD_IDLE; break;
                    case BROWSE_ALBUMS:         albums_ls  = LOAD_IDLE; break;
                    case BROWSE_TRACKS:         tracks_ls  = LOAD_IDLE; break;
                    case BROWSE_ALL_ALBUMS:
                    case BROWSE_ALL_ALBUMS_PAGE:
                        all_albums_ls = LOAD_IDLE; break;
                    case BROWSE_RECENT_ALBUMS:  recent_albums_ls = LOAD_IDLE; break;
                    default: break;
                }
                state = net_error_from;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_B)) {
                last_art_thumb[0] = '\0';
                plex_art_clear();
                switch (net_error_back) {
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
