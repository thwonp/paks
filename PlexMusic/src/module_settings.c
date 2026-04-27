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

/* Menu item indices (items 0 and 1 are header lines — non-selectable) */
#define SETTINGS_ITEM_SWITCH_SERVER 0
#define SETTINGS_ITEM_SIGN_OUT      1
#define SETTINGS_ITEM_COUNT         2

/* Milliseconds to show "Server updated." confirmation */
#define SERVER_UPDATED_PAUSE_MS 1500

/* =========================================================================
 * Render helpers
 * ========================================================================= */

/*
 * Render the main settings menu:
 *   - Server info header (non-selectable)
 *   - "Switch Server" item
 *   - "Sign Out" item
 */
static void render_settings_menu(SDL_Surface *screen, int show_setting,
                                  int menu_selected)
{
    const PlexConfig *cfg = plex_config_get_mutable();
    int hw = screen->w;

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    render_screen_header(screen, "Settings", show_setting);

    ListLayout layout = calc_list_layout(screen);

    /* --- Server info header block --- */
    /* "Server: <name>" in medium white */
    char server_label[PLEX_MAX_STR + 12];
    if (cfg->server_name[0]) {
        snprintf(server_label, sizeof(server_label), "Server: %s", cfg->server_name);
    } else {
        snprintf(server_label, sizeof(server_label), "Server: (unknown)");
    }

    SDL_Surface *name_surf = TTF_RenderUTF8_Blended(Fonts_getMedium(),
                                                     server_label, COLOR_WHITE);
    if (name_surf) {
        int tx = (hw - name_surf->w) / 2;
        if (tx < SCALE1(BUTTON_PADDING)) tx = SCALE1(BUTTON_PADDING);
        SDL_BlitSurface(name_surf, NULL, screen,
                        &(SDL_Rect){tx, layout.list_y});
        SDL_FreeSurface(name_surf);
    }

    /* Server URL in small grey on the next line */
    if (cfg->server_url[0]) {
        SDL_Surface *url_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(),
                                                        cfg->server_url,
                                                        COLOR_LIGHT_TEXT);
        if (url_surf) {
            int tx = (hw - url_surf->w) / 2;
            if (tx < SCALE1(BUTTON_PADDING)) tx = SCALE1(BUTTON_PADDING);
            SDL_BlitSurface(url_surf, NULL, screen,
                            &(SDL_Rect){tx, layout.list_y + layout.item_h});
            SDL_FreeSurface(url_surf);
        }
    }

    /* --- Selectable menu items (offset below the 2-line header) --- */
    /* Each item uses render_menu_item_pill; index offset by 2 rows to clear header */
    const char *labels[SETTINGS_ITEM_COUNT] = {
        "Switch Server",
        "Sign Out"
    };

    /* Adjust list_y for menu items so they appear below the header block */
    ListLayout menu_layout = layout;
    menu_layout.list_y = layout.list_y + layout.item_h * 2 + SCALE1(8);

    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        bool sel = (i == menu_selected);
        char truncated[256];
        MenuItemPos pos = render_menu_item_pill(screen, &menu_layout,
                                                labels[i], truncated,
                                                i, sel, 0);
        render_list_item_text(screen, NULL, truncated, Fonts_getMedium(),
                              pos.text_x, pos.text_y, menu_layout.max_width, sel);
    }

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
static void render_signout_dialog(SDL_Surface *screen, int show_setting)
{
    render_settings_menu(screen, show_setting, SETTINGS_ITEM_SIGN_OUT);

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
    int dirty = 1;
    int show_setting = 0;

    PlexServer servers[PLEX_MAX_SERVERS];
    int server_count = 0;
    int server_selected = 0;
    int server_scroll = 0;
    bool servers_loaded = false;

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

        /* ------------------------------------------------------------------
         * SETTINGS_STATE_MENU
         * ------------------------------------------------------------------ */
        if (state == SETTINGS_STATE_MENU) {
            if (PAD_justRepeated(BTN_UP)) {
                if (menu_selected > 0) {
                    menu_selected--;
                    dirty = 1;
                }
            } else if (PAD_justRepeated(BTN_DOWN)) {
                if (menu_selected < SETTINGS_ITEM_COUNT - 1) {
                    menu_selected++;
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_A)) {
                if (menu_selected == SETTINGS_ITEM_SWITCH_SERVER) {
                    /* Reset server list so it gets re-fetched */
                    server_count = 0;
                    server_selected = 0;
                    server_scroll = 0;
                    servers_loaded = false;
                    state = SETTINGS_STATE_SERVERS;
                    dirty = 1;
                } else if (menu_selected == SETTINGS_ITEM_SIGN_OUT) {
                    state = SETTINGS_STATE_SIGNOUT_CONFIRM;
                    dirty = 1;
                }
            } else if (PAD_justPressed(BTN_B)) {
                return MODULE_BROWSE;
            }

            if (dirty) {
                render_settings_menu(screen, show_setting, menu_selected);
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
            } else if (PAD_justPressed(BTN_B)) {
                state = SETTINGS_STATE_MENU;
                dirty = 1;
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
            } else if (PAD_justPressed(BTN_B)) {
                state = SETTINGS_STATE_MENU;
                dirty = 1;
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
            }

            if (dirty) {
                render_signout_dialog(screen, show_setting);
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
