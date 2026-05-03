#include <stdio.h>
#include <string.h>

#include "api.h"
#include "defines.h"
#include "module_auth.h"
#include "module_browse.h"
#include "module_common.h"
#include "module_settings.h"
#include "plex_art.h"
#include "plex_auth.h"
#include "plex_config.h"
#include "plex_models.h"
#include "ui_fonts.h"
#include "ui_utils.h"

/* =========================================================================
 * Internal state machine
 * ========================================================================= */

typedef enum {
    SETTINGS_STATE_MENU,          /* Main settings menu */
    SETTINGS_STATE_SERVERS,       /* Server selection list */
    SETTINGS_STATE_SERVER_ERROR,  /* No servers found / fetch error */
    SETTINGS_STATE_SIGNOUT_CONFIRM /* Sign-out confirmation dialog */
} SettingsState;

/* Menu item indices */
#define SETTINGS_ITEM_SWITCH_SERVER    0
#define SETTINGS_ITEM_LIBRARY          1
#define SETTINGS_ITEM_SCREEN_TIMEOUT   2
#define SETTINGS_ITEM_STREAM_BITRATE   3
#define SETTINGS_ITEM_DOWNLOAD_BITRATE 4
#define SETTINGS_ITEM_POCKET_LOCK      5
#define SETTINGS_ITEM_PRELOAD_COUNT    6
#define SETTINGS_ITEM_SIGN_OUT         7
#define SETTINGS_ITEM_COUNT            8

/* Milliseconds to show "Server updated." confirmation */
#define SERVER_UPDATED_PAUSE_MS 1500

/* Screen-timeout cycle: ordered list of values (seconds) */
static const int SCREEN_TIMEOUT_VALUES[] = { 0, 15, 30, 60, 120, 300 };
static const int SCREEN_TIMEOUT_COUNT =
    (int)(sizeof(SCREEN_TIMEOUT_VALUES) / sizeof(SCREEN_TIMEOUT_VALUES[0]));

/* Human-readable label for a screen_timeout value */
static const char *screen_timeout_label(int seconds)
{
    switch (seconds) {
        case 0:   return "Off";
        case 15:  return "15 seconds";
        case 30:  return "30 seconds";
        case 60:  return "1 minute";
        case 120: return "2 minutes";
        case 300: return "5 minutes";
        default:  return "Off";
    }
}

/* Return the cycle index for the current timeout value (0 if not found) */
static int screen_timeout_index(int seconds)
{
    for (int i = 0; i < SCREEN_TIMEOUT_COUNT; i++) {
        if (SCREEN_TIMEOUT_VALUES[i] == seconds)
            return i;
    }
    return 0;
}

/* Bitrate cycle: 0 = Original; 96/128/192/256/320 kbps = transcode to Opus */
static const int BITRATE_VALUES[] = { 0, 96, 128, 192, 256, 320 };
static const int BITRATE_COUNT    = 6;

static const char *bitrate_label(int kbps)
{
    if (kbps == 0) return "Original";
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d kbps", kbps);
    return buf;
}

static int bitrate_index(int kbps)
{
    for (int i = 0; i < BITRATE_COUNT; i++)
        if (BITRATE_VALUES[i] == kbps) return i;
    return 0;
}

/* Human-readable label for a preload_count value */
static const char *preload_count_label(int count)
{
    if (count == 0) return "Off";
    if (count == 1) return "1 track";
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d tracks", count);
    return buf;
}

/* =========================================================================
 * Render helpers
 * ========================================================================= */

/*
 * Render the main settings menu.
 */
static void render_settings_menu(SDL_Surface *screen, int show_setting,
                                  int menu_selected, int scroll,
                                  int menu_items_visible)
{
    const PlexConfig *cfg = plex_config_get_mutable();

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    render_screen_header(screen, "Settings", show_setting);

    ListLayout layout = calc_list_layout(screen);

    /* --- Selectable menu items --- */
    char server_label[PLEX_MAX_STR + 12];
    if (cfg->server_name[0])
        snprintf(server_label, sizeof(server_label), "Server: %s", cfg->server_name);
    else
        snprintf(server_label, sizeof(server_label), "Server: (unknown)");

    char library_label[PLEX_MAX_STR + 12];
    if (cfg->library_name[0])
        snprintf(library_label, sizeof(library_label), "Library: %s", cfg->library_name);
    else
        snprintf(library_label, sizeof(library_label), "Library: (not set)");

    char timeout_label[64];
    snprintf(timeout_label, sizeof(timeout_label), "Screen timeout: %s",
             screen_timeout_label(cfg->screen_timeout));

    char stream_bitrate_label[32];
    snprintf(stream_bitrate_label, sizeof(stream_bitrate_label),
             "Streaming Quality: %s", bitrate_label(cfg->stream_bitrate_kbps));

    char download_bitrate_label[32];
    snprintf(download_bitrate_label, sizeof(download_bitrate_label),
             "Download Quality: %s", bitrate_label(cfg->download_bitrate_kbps));

    char pocket_lock_label[48];
    snprintf(pocket_lock_label, sizeof(pocket_lock_label),
             "Pocket Button Lock: %s",
             cfg->pocket_lock_enabled ? "MENU+SELECT" : "Off");

    char preload_count_lbl[32];
    snprintf(preload_count_lbl, sizeof(preload_count_lbl),
             "Track Preload: %s", preload_count_label(cfg->preload_count));

    const char *labels[SETTINGS_ITEM_COUNT] = {
        server_label,
        library_label,
        timeout_label,
        stream_bitrate_label,
        download_bitrate_label,
        pocket_lock_label,
        preload_count_lbl,
        "Sign Out",
    };

    for (int i = scroll; i < SETTINGS_ITEM_COUNT && i < scroll + menu_items_visible; i++) {
        bool sel = (i == menu_selected);
        char truncated[256];
        MenuItemPos pos = render_menu_item_pill(screen, &layout,
                                                labels[i], truncated,
                                                i - scroll,   /* visual row index */
                                                sel, 0);
        render_list_item_text(screen, NULL, truncated, Fonts_getMedium(),
                              pos.text_x, pos.text_y, layout.max_width, sel);
    }

    render_scroll_indicators(screen, scroll, menu_items_visible, SETTINGS_ITEM_COUNT);

    GFX_blitButtonGroup((char*[]){"A", "SELECT", "B", "BACK", NULL}, 1, screen, 1);
}

/*
 * Render the server selection list (same pattern as module_auth).
 */
static void render_servers_screen(SDL_Surface *screen, const PlexServer servers[],
                                   int server_count, int selected, int *scroll)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    render_screen_header(screen, "Select Server", 0);

    ListLayout layout = calc_list_layout(screen);
    adjust_list_scroll(selected, scroll, layout.items_per_page);

    for (int i = *scroll; i < server_count && i < *scroll + layout.items_per_page; i++) {
        int y = layout.list_y + (i - *scroll) * layout.item_h;
        bool sel = (i == selected);
        char truncated[256];
        ListItemPos pos = render_list_item_pill(screen, &layout, servers[i].name,
                                                truncated, y, sel, 0);
        render_list_item_text(screen, NULL, truncated, Fonts_getMedium(),
                              pos.text_x, pos.text_y,
                              layout.max_width - SCALE1(BUTTON_PADDING * 2), sel);
    }

    render_scroll_indicators(screen, *scroll, layout.items_per_page, server_count);

    GFX_blitButtonGroup((char*[]){"A", "SELECT", "B", "BACK", NULL}, 1, screen, 1);
}

/*
 * Render a brief loading indicator (blocking call is about to happen).
 */
static void render_loading(SDL_Surface *screen, const char *msg)
{
    int hw = screen->w;
    int hh = screen->h;

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    SDL_Surface *label = TTF_RenderUTF8_Blended(Fonts_getMedium(), msg, COLOR_WHITE);
    if (label) {
        SDL_BlitSurface(label, NULL, screen,
                        &(SDL_Rect){(hw - label->w) / 2, (hh - label->h) / 2});
        SDL_FreeSurface(label);
    }
    GFX_flip(screen);
}

/*
 * Render the server error screen.
 */
static void render_server_error(SDL_Surface *screen)
{
    int hw = screen->w;
    int hh = screen->h;

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    SDL_Surface *err = TTF_RenderUTF8_Blended(
        Fonts_getMedium(), "No servers found.",
        (SDL_Color){0xE5, 0x45, 0x45, 0xFF});
    if (err) {
        SDL_BlitSurface(err, NULL, screen,
                        &(SDL_Rect){(hw - err->w) / 2, hh / 2 - SCALE1(20)});
        SDL_FreeSurface(err);
    }

    SDL_Surface *hint = TTF_RenderUTF8_Blended(
        Fonts_getSmall(), "Press A to retry, B to cancel", COLOR_LIGHT_TEXT);
    if (hint) {
        SDL_BlitSurface(hint, NULL, screen,
                        &(SDL_Rect){(hw - hint->w) / 2, hh / 2 + SCALE1(10)});
        SDL_FreeSurface(hint);
    }

    GFX_blitButtonGroup((char*[]){"A", "RETRY", "B", "CANCEL", NULL}, 1, screen, 1);
}

/*
 * Render the sign-out confirmation dialog overlay on top of the menu.
 */
static void render_signout_dialog(SDL_Surface *screen, int show_setting, int scroll,
                                   int menu_items_visible)
{
    render_settings_menu(screen, show_setting, SETTINGS_ITEM_SIGN_OUT, scroll,
                         menu_items_visible);

    int box_w = SCALE1(280);
    int box_h = SCALE1(80);

    DialogBox dlg = render_dialog_box(screen, box_w, box_h);

    SDL_Surface *msg = TTF_RenderUTF8_Blended(
        Fonts_getMedium(), "Sign out of Plex?", COLOR_WHITE);
    if (msg) {
        int tx = dlg.box_x + (dlg.box_w - msg->w) / 2;
        int ty = dlg.box_y + SCALE1(12);
        SDL_BlitSurface(msg, NULL, screen, &(SDL_Rect){tx, ty});
        SDL_FreeSurface(msg);
    }

    SDL_Surface *hint = TTF_RenderUTF8_Blended(
        Fonts_getSmall(), "[A] Confirm  [B] Cancel", COLOR_LIGHT_TEXT);
    if (hint) {
        int tx = dlg.box_x + (dlg.box_w - hint->w) / 2;
        int ty = dlg.box_y + box_h - SCALE1(28);
        SDL_BlitSurface(hint, NULL, screen, &(SDL_Rect){tx, ty});
        SDL_FreeSurface(hint);
    }
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

AppModule module_settings_run(SDL_Surface *screen)
{
    SettingsState state = SETTINGS_STATE_MENU;
    int menu_selected = SETTINGS_ITEM_SWITCH_SERVER;
    int menu_scroll = 0;
    int menu_items_visible;
    int dirty = 1;
    int show_setting = 0;

    /* Compute once — screen size is fixed for the session */
    {
        ListLayout il = calc_list_layout(screen);
        int v = il.list_h / il.item_h;
        menu_items_visible = (v < 1) ? 1 : v;
    }

    PlexServer servers[PLEX_MAX_SERVERS];
    int server_count = 0;
    int server_selected = 0;
    int server_scroll = 0;
    bool servers_loaded = false;

    while (1) {
        GFX_startFrame();
        PAD_poll();
        SDL_Delay(16);

        /* Power management heartbeat — must run every frame before any early-return */
        ModuleCommon_PWR_update(&dirty, &show_setting);

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

        /* ------------------------------------------------------------------
         * SETTINGS_STATE_MENU
         * ------------------------------------------------------------------ */
        if (state == SETTINGS_STATE_MENU) {
            if (PAD_justRepeated(BTN_UP)) {
                if (menu_selected > 0) {
                    menu_selected--;
                    adjust_list_scroll(menu_selected, &menu_scroll, menu_items_visible);
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_DOWN)) {
                if (menu_selected < SETTINGS_ITEM_COUNT - 1) {
                    menu_selected++;
                    adjust_list_scroll(menu_selected, &menu_scroll, menu_items_visible);
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_RIGHT)) {
                if (menu_selected == SETTINGS_ITEM_SWITCH_SERVER) {
                    /* Reset server list so it gets re-fetched */
                    server_count = 0;
                    server_selected = 0;
                    server_scroll = 0;
                    servers_loaded = false;
                    state = SETTINGS_STATE_SERVERS;
                    dirty = 1;
                    GFX_sync();
                    continue;
                } else if (menu_selected == SETTINGS_ITEM_SIGN_OUT) {
                    state = SETTINGS_STATE_SIGNOUT_CONFIRM;
                    dirty = 1;
                    GFX_sync();
                    continue;
                } else if (menu_selected == SETTINGS_ITEM_SCREEN_TIMEOUT) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    int idx = screen_timeout_index(cfg->screen_timeout);
                    idx = (idx + 1) % SCREEN_TIMEOUT_COUNT;
                    cfg->screen_timeout = SCREEN_TIMEOUT_VALUES[idx];
                    plex_config_save(cfg);
                    dirty = 1;
                } else if (menu_selected == SETTINGS_ITEM_LIBRARY) {
                    module_browse_request_library_pick();
                    return MODULE_BROWSE;
                } else if (menu_selected == SETTINGS_ITEM_STREAM_BITRATE) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    int idx = bitrate_index(cfg->stream_bitrate_kbps);
                    idx = (idx + 1) % BITRATE_COUNT;
                    cfg->stream_bitrate_kbps = BITRATE_VALUES[idx];
                    plex_config_save(cfg);
                    dirty = 1;
                } else if (menu_selected == SETTINGS_ITEM_DOWNLOAD_BITRATE) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    int idx = bitrate_index(cfg->download_bitrate_kbps);
                    idx = (idx + 1) % BITRATE_COUNT;
                    cfg->download_bitrate_kbps = BITRATE_VALUES[idx];
                    plex_config_save(cfg);
                    dirty = 1;
                } else if (menu_selected == SETTINGS_ITEM_POCKET_LOCK) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    cfg->pocket_lock_enabled = !cfg->pocket_lock_enabled;
                    plex_config_save(cfg);
                    dirty = 1;
                } else if (menu_selected == SETTINGS_ITEM_PRELOAD_COUNT) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    cfg->preload_count = (cfg->preload_count + 1) % 11;
                    plex_config_save(cfg);
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_LEFT)) {
                if (menu_selected == SETTINGS_ITEM_SCREEN_TIMEOUT) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    int idx = screen_timeout_index(cfg->screen_timeout);
                    idx = (idx - 1 + SCREEN_TIMEOUT_COUNT) % SCREEN_TIMEOUT_COUNT;
                    cfg->screen_timeout = SCREEN_TIMEOUT_VALUES[idx];
                    plex_config_save(cfg);
                    dirty = 1;
                } else if (menu_selected == SETTINGS_ITEM_STREAM_BITRATE) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    int idx = bitrate_index(cfg->stream_bitrate_kbps);
                    idx = (idx - 1 + BITRATE_COUNT) % BITRATE_COUNT;
                    cfg->stream_bitrate_kbps = BITRATE_VALUES[idx];
                    plex_config_save(cfg);
                    dirty = 1;
                } else if (menu_selected == SETTINGS_ITEM_DOWNLOAD_BITRATE) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    int idx = bitrate_index(cfg->download_bitrate_kbps);
                    idx = (idx - 1 + BITRATE_COUNT) % BITRATE_COUNT;
                    cfg->download_bitrate_kbps = BITRATE_VALUES[idx];
                    plex_config_save(cfg);
                    dirty = 1;
                } else if (menu_selected == SETTINGS_ITEM_POCKET_LOCK) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    cfg->pocket_lock_enabled = !cfg->pocket_lock_enabled;
                    plex_config_save(cfg);
                    dirty = 1;
                } else if (menu_selected == SETTINGS_ITEM_PRELOAD_COUNT) {
                    PlexConfig *cfg = plex_config_get_mutable();
                    cfg->preload_count = (cfg->preload_count + 10) % 11;
                    plex_config_save(cfg);
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_B)) {
                return MODULE_BROWSE;
            }

            if (dirty) {
                render_settings_menu(screen, show_setting, menu_selected, menu_scroll,
                                     menu_items_visible);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ------------------------------------------------------------------
         * SETTINGS_STATE_SERVERS
         * ------------------------------------------------------------------ */
        } else if (state == SETTINGS_STATE_SERVERS) {

            /* Fetch servers on first entry into this state */
            if (!servers_loaded) {
                render_loading(screen, "Loading servers...");
                PlexConfig *cfg = plex_config_get_mutable();
                server_count = 0;
                int result = plex_auth_get_servers(cfg->token, servers, &server_count);
                servers_loaded = true;

                if (result != 0 || server_count == 0) {
                    server_count = 0;
                    state = SETTINGS_STATE_SERVER_ERROR;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                dirty = 1;
            }

            if (PAD_justRepeated(BTN_UP)) {
                server_selected = (server_selected > 0)
                    ? server_selected - 1 : server_count - 1;
                dirty = 1;
            } else if (PAD_justRepeated(BTN_DOWN)) {
                server_selected = (server_selected < server_count - 1)
                    ? server_selected + 1 : 0;
                dirty = 1;
            } else if (PAD_justPressed(BTN_A)) {
                PlexConfig *cfg = plex_config_get_mutable();
                strncpy(cfg->server_url, servers[server_selected].url, PLEX_MAX_URL - 1);
                cfg->server_url[PLEX_MAX_URL - 1] = '\0';
                strncpy(cfg->server_name, servers[server_selected].name, PLEX_MAX_STR - 1);
                cfg->server_name[PLEX_MAX_STR - 1] = '\0';
                strncpy(cfg->server_id, servers[server_selected].id, PLEX_MAX_STR - 1);
                cfg->server_id[PLEX_MAX_STR - 1] = '\0';
                plex_config_save(cfg);

                /* Brief confirmation message */
                render_loading(screen, "Server updated.");
                SDL_Delay(SERVER_UPDATED_PAUSE_MS);

                /* Return to settings menu */
                state = SETTINGS_STATE_MENU;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_B)) {
                state = SETTINGS_STATE_MENU;
                dirty = 1;
                GFX_sync();
                continue;
            }

            if (dirty) {
                render_servers_screen(screen, servers, server_count,
                                      server_selected, &server_scroll);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ------------------------------------------------------------------
         * SETTINGS_STATE_SERVER_ERROR
         * ------------------------------------------------------------------ */
        } else if (state == SETTINGS_STATE_SERVER_ERROR) {

            if (PAD_justPressed(BTN_A)) {
                /* Retry */
                server_count = 0;
                servers_loaded = false;
                state = SETTINGS_STATE_SERVERS;
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_B)) {
                state = SETTINGS_STATE_MENU;
                dirty = 1;
                GFX_sync();
                continue;
            }

            if (dirty) {
                render_server_error(screen);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }

        /* ------------------------------------------------------------------
         * SETTINGS_STATE_SIGNOUT_CONFIRM
         * ------------------------------------------------------------------ */
        } else if (state == SETTINGS_STATE_SIGNOUT_CONFIRM) {

            if (PAD_justPressed(BTN_A)) {
                /* Confirmed — zero config, save, clear art, reset browse, go to auth */
                PlexConfig *cfg = plex_config_get_mutable();
                memset(cfg, 0, sizeof(*cfg));
                plex_config_save(cfg);
                plex_art_clear();
                module_browse_reset();
                return MODULE_AUTH;
            } else if (PAD_justPressed(BTN_B)) {
                state = SETTINGS_STATE_MENU;
                dirty = 1;
                GFX_sync();
                continue;
            }

            if (dirty) {
                render_signout_dialog(screen, show_setting, menu_scroll, menu_items_visible);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }
        }
    }

    /* Unreachable */
    return MODULE_BROWSE;
}
