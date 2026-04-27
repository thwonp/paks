#include <stdio.h>
#include <string.h>

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
} BrowseState;

/* Width fraction for the list panel (70%) */
#define LIST_PANEL_FRAC 70

/* Right panel art padding */
#define ART_PANEL_PAD SCALE1(8)

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

static void render_loading(SDL_Surface *screen)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
    int hw = screen->w;
    int hh = screen->h;
    SDL_Surface *msg = TTF_RenderUTF8_Blended(Fonts_getMedium(), "Loading...", COLOR_WHITE);
    if (msg) {
        SDL_BlitSurface(msg, NULL, screen,
                        &(SDL_Rect){(hw - msg->w) / 2, (hh - msg->h) / 2});
        SDL_FreeSurface(msg);
    }
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
 * total_items includes a possible "Load more" sentinel.
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
                                  const char *btn_b_label)
{
    int hw = screen->w;
    int list_w = (hw * LIST_PANEL_FRAC) / 100;

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

    /* Art panel */
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
} ArtistLabelCtx;

static void artist_get_label(int i, char *buf, int size, void *ud)
{
    ArtistLabelCtx *ctx = (ArtistLabelCtx *)ud;
    /* "Load more" sentinel is one past the loaded artists */
    if (i == ctx->loaded) {
        snprintf(buf, size, "[ Load more... ]");
    } else {
        snprintf(buf, size, "%s", ctx->artists[i].title);
    }
}

typedef struct {
    PlexAlbum *albums;
    int count;
} AlbumLabelCtx;

static void album_get_label(int i, char *buf, int size, void *ud)
{
    AlbumLabelCtx *ctx = (AlbumLabelCtx *)ud;
    if (ctx->albums[i].year[0])
        snprintf(buf, size, "%s (%s)", ctx->albums[i].title, ctx->albums[i].year);
    else
        snprintf(buf, size, "%s", ctx->albums[i].title);
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
    static bool          libs_tried        = false; /* one-shot load guard */

    /* Artists */
    static PlexArtist    artists[PLEX_MAX_ITEMS];
    static int           artists_loaded    = 0;
    static int           artists_total     = 0;
    static int           artist_selected   = 0;
    static int           artist_scroll     = 0;
    static bool          artists_tried     = false;

    /* Albums */
    static PlexAlbum     albums[PLEX_MAX_ITEMS];
    static int           album_count       = 0;
    static int           album_selected    = 0;
    static int           album_scroll      = 0;

    /* Tracks */
    static PlexTrack     tracks[PLEX_MAX_ITEMS];
    static int           track_count       = 0;
    static int           track_selected    = 0;
    static int           track_scroll      = 0;

    /* Selected keys carried between levels */
    static int           selected_library_id       = 0;
    static int           selected_artist_rating_key = 0;
    static int           selected_album_rating_key  = 0;
    static char          selected_album_thumb[PLEX_MAX_URL] = "";

    /* Art fetch state: last thumb that was requested */
    static char          last_art_thumb[PLEX_MAX_URL] = "";

    /* ------------------------------------------------------------------ */
    /* Per-frame locals                                                    */
    /* ------------------------------------------------------------------ */
    int dirty = 1;
    int show_setting = 0;

    const PlexConfig *cfg = plex_config_get_mutable();
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
        libs_tried      = false;
        artists_loaded  = 0;
        artists_total   = 0;
        artists_tried   = false;
        artist_selected = 0;
        artist_scroll   = 0;
        album_count     = 0;
        album_selected  = 0;
        album_scroll    = 0;
        track_count     = 0;
        track_selected  = 0;
        track_scroll    = 0;
        last_art_thumb[0] = '\0';
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

            /* Load on first entry — one-shot via libs_tried flag */
            if (!libs_tried) {
                libs_tried = true;
                PLEX_LOG("[Browse] Loading libraries from: %s\n", cfg->server_url);
                render_loading(screen);
                memset(libs, 0, sizeof(libs));
                int raw_count = 0;
                plex_api_get_libraries(cfg, libs, &raw_count);
                PLEX_LOG("[Browse] Got %d libraries (raw)\n", raw_count);
                lib_music_count = 0;
                for (int i = 0; i < raw_count; i++) {
                    if (strcmp(libs[i].type, "artist") == 0) {
                        lib_music_idx[lib_music_count++] = i;
                    }
                }
                lib_count = raw_count;

                PLEX_LOG("[Browse] Music library count: %d\n", lib_music_count);

                /* Auto-select if only one music library */
                if (lib_music_count == 1) {
                    selected_library_id = libs[lib_music_idx[0]].section_id;
                    artists_loaded = 0;
                    artists_total  = 0;
                    artists_tried  = false;
                    artist_selected = 0;
                    artist_scroll   = 0;
                    state = BROWSE_ARTISTS;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                dirty = 1;
            }

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
            if (PAD_justRepeated(BTN_UP)) {
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
                    artists_loaded = 0;
                    artists_total  = 0;
                    artists_tried  = false;
                    artist_selected = 0;
                    artist_scroll   = 0;
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    state = BROWSE_ARTISTS;
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_B)) {
                return MODULE_QUIT;
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
                                     "SELECT", "QUIT");
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_ARTISTS
         * ============================================================== */
        } else if (state == BROWSE_ARTISTS) {

            /* Load on first entry */
            if (!artists_tried) {
                artists_tried = true;
                PLEX_LOG("[Browse] Loading artists for library_id=%d\n", selected_library_id);
                render_loading(screen);
                PlexPage page;
                memset(&page, 0, sizeof(page));
                int rc = plex_api_get_artists(cfg, selected_library_id, 0, PLEX_MAX_ITEMS,
                                              artists, &page);
                if (rc != 0) {
                    lib_count       = 0;
                    lib_music_count = 0;
                    lib_selected    = 0;
                    lib_scroll      = 0;
                    libs_tried      = false;
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    state = BROWSE_LIBRARIES;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                artists_loaded = page.count;
                artists_total  = page.total;
                PLEX_LOG("[Browse] Got %d / %d artists\n", artists_loaded, artists_total);
                dirty = 1;

                /* Trigger art fetch for initial selection */
                if (artists_loaded > 0 && artists[0].thumb[0]) {
                    snprintf(last_art_thumb, sizeof(last_art_thumb), "%s", artists[0].thumb);
                    plex_art_fetch(cfg, artists[0].thumb);
                }
            }

            /* Count visible items (artists + optional "Load more") */
            int visible_items = artists_loaded;
            if (artists_loaded < artists_total)
                visible_items++; /* "Load more" sentinel */

            /* Input */
            if (PAD_justRepeated(BTN_UP)) {
                int prev = artist_selected;
                artist_selected = (artist_selected > 0) ? artist_selected - 1
                                                         : visible_items - 1;
                if (artist_selected != prev) {
                    if (artist_selected < artists_loaded &&
                        artists[artist_selected].thumb[0] &&
                        strcmp(artists[artist_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", artists[artist_selected].thumb);
                        plex_art_fetch(cfg, artists[artist_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_DOWN)) {
                int prev = artist_selected;
                artist_selected = (artist_selected < visible_items - 1)
                                      ? artist_selected + 1 : 0;
                if (artist_selected != prev) {
                    if (artist_selected < artists_loaded &&
                        artists[artist_selected].thumb[0] &&
                        strcmp(artists[artist_selected].thumb, last_art_thumb) != 0) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb),
                                 "%s", artists[artist_selected].thumb);
                        plex_art_fetch(cfg, artists[artist_selected].thumb);
                    }
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_A)) {
                if (artists_loaded < artists_total &&
                    artist_selected == artists_loaded) {
                    /* "Load more" selected */
                    render_loading(screen);
                    PlexPage page;
                    memset(&page, 0, sizeof(page));
                    int cap = PLEX_MAX_ITEMS - artists_loaded;
                    if (cap > 0) {
                        plex_api_get_artists(cfg, selected_library_id,
                                             artists_loaded, cap,
                                             &artists[artists_loaded], &page);
                        artists_loaded += page.count;
                    }
                    dirty = 1;
                } else if (artist_selected < artists_loaded) {
                    selected_artist_rating_key = artists[artist_selected].rating_key;
                    album_count    = 0;
                    album_selected = 0;
                    album_scroll   = 0;
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    state = BROWSE_ALBUMS;
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_B)) {
                /* Back to libraries; reset lib state so it reloads */
                lib_count       = 0;
                lib_music_count = 0;
                lib_selected    = 0;
                lib_scroll      = 0;
                libs_tried      = false;
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_LIBRARIES;
                dirty = 1;
            }

            /* Poll art async */
            if (plex_art_is_fetching()) dirty = 1;

            /* Render */
            if (dirty) {
                ArtistLabelCtx actx;
                actx.artists = artists;
                actx.loaded  = artists_loaded;
                actx.total   = artists_total;

                const char *art_line1 = (artist_selected < artists_loaded)
                    ? artists[artist_selected].title : NULL;

                render_browse_screen(screen, "Artists",
                                     artist_selected, &artist_scroll,
                                     visible_items,
                                     art_line1, NULL, plex_art_get(),
                                     artist_get_label, &actx,
                                     "SELECT", "BACK");
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_ALBUMS
         * ============================================================== */
        } else if (state == BROWSE_ALBUMS) {

            /* Load on first entry */
            if (album_count == 0) {
                render_loading(screen);
                int rc = plex_api_get_albums(cfg, selected_artist_rating_key,
                                             albums, &album_count);
                if (rc != 0) {
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    if (artist_selected < artists_loaded && artists[artist_selected].thumb[0]) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb), "%s", artists[artist_selected].thumb);
                        plex_art_fetch(cfg, artists[artist_selected].thumb);
                    }
                    state = BROWSE_ARTISTS;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                dirty = 1;

                /* Trigger art for first album */
                if (album_count > 0 && albums[0].thumb[0]) {
                    snprintf(last_art_thumb, sizeof(last_art_thumb), "%s", albums[0].thumb);
                    plex_art_fetch(cfg, albums[0].thumb);
                }
            }

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
                snprintf(selected_album_thumb, sizeof(selected_album_thumb),
                         "%s", albums[album_selected].thumb);
                track_count    = 0;
                track_selected = 0;
                track_scroll   = 0;
                last_art_thumb[0] = '\0';
                plex_art_clear();
                state = BROWSE_TRACKS;
                dirty = 1;
            } else if (PAD_justPressed(BTN_B)) {
                last_art_thumb[0] = '\0';
                plex_art_clear();
                /* Restore art for the currently selected artist */
                if (artist_selected < artists_loaded && artists[artist_selected].thumb[0]) {
                    snprintf(last_art_thumb, sizeof(last_art_thumb),
                             "%s", artists[artist_selected].thumb);
                    plex_art_fetch(cfg, artists[artist_selected].thumb);
                }
                state = BROWSE_ARTISTS;
                dirty = 1;
            }

            /* Poll art async */
            if (plex_art_is_fetching()) dirty = 1;

            /* Render */
            if (dirty) {
                AlbumLabelCtx alctx;
                alctx.albums = albums;
                alctx.count  = album_count;

                const char *art_line1 = (album_count > 0)
                    ? albums[album_selected].title : NULL;
                const char *art_line2 = (album_count > 0 && albums[album_selected].year[0])
                    ? albums[album_selected].year : NULL;

                render_browse_screen(screen, "Albums",
                                     album_selected, &album_scroll,
                                     album_count,
                                     art_line1, art_line2, plex_art_get(),
                                     album_get_label, &alctx,
                                     "SELECT", "BACK");
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ==============================================================
         * BROWSE_TRACKS
         * ============================================================== */
        } else if (state == BROWSE_TRACKS) {

            /* Load tracks on first entry */
            if (track_count == 0) {
                render_loading(screen);
                int rc = plex_api_get_tracks(cfg, selected_album_rating_key,
                                             tracks, &track_count);
                if (rc != 0) {
                    last_art_thumb[0] = '\0';
                    plex_art_clear();
                    if (album_selected < album_count && albums[album_selected].thumb[0]) {
                        snprintf(last_art_thumb, sizeof(last_art_thumb), "%s", albums[album_selected].thumb);
                        plex_art_fetch(cfg, albums[album_selected].thumb);
                    }
                    state = BROWSE_ALBUMS;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                dirty = 1;

                /* Fetch album art once — same for all tracks */
                if (selected_album_thumb[0] &&
                    strcmp(selected_album_thumb, last_art_thumb) != 0) {
                    snprintf(last_art_thumb, sizeof(last_art_thumb),
                             "%s", selected_album_thumb);
                    plex_art_fetch(cfg, selected_album_thumb);
                }
            }

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
                                     "PLAY", "BACK");
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }
        }
    }
}
