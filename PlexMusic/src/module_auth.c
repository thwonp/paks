#include <stdio.h>
#include <string.h>

#include "api.h"
#include "plex_log.h"
#include "defines.h"
#include "module_auth.h"
#include "module_common.h"
#include "plex_auth.h"
#include "plex_config.h"
#include "plex_models.h"
#include "ui_fonts.h"
#include "ui_utils.h"

/* =========================================================================
 * Internal state machine
 * ========================================================================= */

typedef enum {
    AUTH_STATE_PIN,      /* Show PIN code, poll for auth */
    AUTH_STATE_SERVERS,  /* List servers to pick from */
    AUTH_STATE_ERROR     /* Timed out or error — show message, retry option */
} AuthState;

/* PIN expiry window in milliseconds */
#define AUTH_PIN_TIMEOUT_MS   120000
/* Poll interval in milliseconds */
#define AUTH_POLL_INTERVAL_MS 2000

/* =========================================================================
 * Render helpers
 * ========================================================================= */

static void render_pin_screen(SDL_Surface *screen, const char *pin_code,
                               uint32_t entry_time, uint32_t last_poll_time)
{
    int hw = screen->w;
    int hh = screen->h;

    /* Dark background */
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    /* "Sign in to Plex" title — centered, upper third */
    SDL_Surface *title = TTF_RenderUTF8_Blended(Fonts_getLarge(), "Sign in to Plex", COLOR_WHITE);
    if (title) {
        SDL_BlitSurface(title, NULL, screen,
                        &(SDL_Rect){(hw - title->w) / 2, hh / 3 - SCALE1(60)});
        SDL_FreeSurface(title);
    }

    /* PIN code — very large, centered */
    SDL_Surface *pin_surf = TTF_RenderUTF8_Blended(Fonts_getXLarge(), pin_code,
                                                    (SDL_Color){0xE5, 0xA0, 0x0D, 0xFF});
    if (pin_surf) {
        SDL_BlitSurface(pin_surf, NULL, screen,
                        &(SDL_Rect){(hw - pin_surf->w) / 2, hh / 3 - SCALE1(10)});
        SDL_FreeSurface(pin_surf);
    }

    /* Instruction text */
    SDL_Surface *instr = TTF_RenderUTF8_Blended(Fonts_getMedium(),
                                                  "Visit plex.tv/link on your phone",
                                                  COLOR_LIGHT_TEXT);
    if (instr) {
        SDL_BlitSurface(instr, NULL, screen,
                        &(SDL_Rect){(hw - instr->w) / 2, hh / 3 + SCALE1(50)});
        SDL_FreeSurface(instr);
    }

    /* Countdown */
    uint32_t elapsed = SDL_GetTicks() - entry_time;
    int remaining = (int)((AUTH_PIN_TIMEOUT_MS - (int)elapsed) / 1000);
    if (remaining < 0) remaining = 0;

    char countdown[32];
    snprintf(countdown, sizeof(countdown), "Expires in %ds", remaining);
    SDL_Surface *timer = TTF_RenderUTF8_Blended(Fonts_getSmall(), countdown,
                                                  (SDL_Color){0x99, 0x99, 0x99, 0xFF});
    if (timer) {
        SDL_BlitSurface(timer, NULL, screen,
                        &(SDL_Rect){(hw - timer->w) / 2, hh / 3 + SCALE1(75)});
        SDL_FreeSurface(timer);
    }

    /* Poll activity indicator — show "..." while we are within the poll window */
    uint32_t since_poll = SDL_GetTicks() - last_poll_time;
    if (since_poll < 600) {
        SDL_Surface *tick = TTF_RenderUTF8_Blended(Fonts_getSmall(), "...",
                                                    COLOR_LIGHT_TEXT);
        if (tick) {
            SDL_BlitSurface(tick, NULL, screen,
                            &(SDL_Rect){(hw - tick->w) / 2, hh / 3 + SCALE1(95)});
            SDL_FreeSurface(tick);
        }
    }

    /* Button hints */
    GFX_blitButtonGroup((char*[]){"B", "QUIT", NULL}, 1, screen, 1);
}

static void render_servers_screen(SDL_Surface *screen, const PlexServer servers[],
                                   int server_count, int selected, int *scroll)
{
    /* Dark background */
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    render_screen_header(screen, "Select Plex Server", 0);

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

static void render_loading_screen(SDL_Surface *screen)
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

static void render_error_screen(SDL_Surface *screen, const char *message)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    int hw = screen->w;
    int hh = screen->h;

    SDL_Surface *err = TTF_RenderUTF8_Blended(Fonts_getMedium(), message,
                                               (SDL_Color){0xE5, 0x45, 0x45, 0xFF});
    if (err) {
        SDL_BlitSurface(err, NULL, screen,
                        &(SDL_Rect){(hw - err->w) / 2, hh / 2 - SCALE1(20)});
        SDL_FreeSurface(err);
    }

    SDL_Surface *hint = TTF_RenderUTF8_Blended(Fonts_getSmall(),
                                                "Press A to retry, B to quit",
                                                COLOR_LIGHT_TEXT);
    if (hint) {
        SDL_BlitSurface(hint, NULL, screen,
                        &(SDL_Rect){(hw - hint->w) / 2, hh / 2 + SCALE1(10)});
        SDL_FreeSurface(hint);
    }

    GFX_blitButtonGroup((char*[]){"A", "RETRY", "B", "QUIT", NULL}, 1, screen, 1);
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

AppModule module_auth_run(SDL_Surface *screen)
{
    AuthState state = AUTH_STATE_PIN;
    int dirty = 1;

    PlexPin pin;
    memset(&pin, 0, sizeof(pin));
    uint32_t entry_time = 0;
    uint32_t last_poll_time = 0;

    PlexServer servers[PLEX_MAX_SERVERS];
    int server_count = 0;
    int server_selected = 0;
    int server_scroll = 0;

    const char *error_msg = "Auth timed out";

    /* --- Enter PIN state --- */
    if (plex_auth_create_pin(&pin) != 0) {
        state = AUTH_STATE_ERROR;
        error_msg = "Failed to create PIN";
    } else {
        entry_time = SDL_GetTicks();
        last_poll_time = entry_time;
    }

    int show_setting = 0;

    while (1) {
        GFX_startFrame();
        PAD_poll();

        /* Handle global input (Start long-press → quit dialog, volume, etc.) */
        GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0);
        if (global.should_quit) {
            return MODULE_QUIT;
        }
        if (global.input_consumed) {
            GFX_sync();
            continue;
        }

        /* ---------------------------------------------------------------
         * PIN screen
         * --------------------------------------------------------------- */
        if (state == AUTH_STATE_PIN) {
            /* Check for timeout */
            if ((int)(SDL_GetTicks() - entry_time) >= AUTH_PIN_TIMEOUT_MS) {
                state = AUTH_STATE_ERROR;
                error_msg = "Auth timed out";
                dirty = 1;
                GFX_sync();
                continue;
            }

            /* Poll every 2 seconds */
            if ((int)(SDL_GetTicks() - last_poll_time) >= AUTH_POLL_INTERVAL_MS) {
                last_poll_time = SDL_GetTicks();
                int result = plex_auth_check_pin(&pin);
                if (result == 1) {
                    /* Authenticated — load servers */
                    state = AUTH_STATE_SERVERS;
                    server_selected = 0;
                    server_scroll = 0;
                    dirty = 1;
                    GFX_sync();
                    continue;
                }
                /* result == 0: still pending; result == -1: transient network error.
                 * Both cases: keep polling. The 120s timer above handles actual expiry. */
                dirty = 1; /* Refresh countdown / activity dot */
            }

            /* Input: B quits */
            if (PAD_justPressed(BTN_B)) {
                return MODULE_QUIT;
            }

            /* Render */
            /* Always re-render the PIN screen so the countdown stays current */
            render_pin_screen(screen, pin.pin_code, entry_time, last_poll_time);
            GFX_flip(screen);
            dirty = 0;
        }
        /* ---------------------------------------------------------------
         * Server selection screen
         * --------------------------------------------------------------- */
        else if (state == AUTH_STATE_SERVERS) {
            /* On first entry: fetch servers (synchronous) */
            if (server_count == 0 && pin.token[0] != '\0') {
                /* Show loading screen before the blocking call */
                render_loading_screen(screen);

                PLEX_LOG("[Auth] Fetching servers...\n");
                if (plex_auth_get_servers(pin.token, servers, &server_count) != 0) {
                    server_count = 0;
                }
                PLEX_LOG("[Auth] server_count=%d\n", server_count);

                if (server_count == 0) {
                    state = AUTH_STATE_ERROR;
                    error_msg = "No servers found";
                    dirty = 1;
                    GFX_sync();
                    continue;
                }

                if (server_count == 1) {
                    /* Auto-select the only server */
                    PLEX_LOG("[Auth] Auto-selecting server: %s\n", servers[0].url);
                    PlexConfig *cfg = plex_config_get_mutable();
                    strncpy(cfg->token, pin.token, PLEX_MAX_STR - 1);
                    cfg->token[PLEX_MAX_STR - 1] = '\0';
                    strncpy(cfg->server_url, servers[0].url, PLEX_MAX_URL - 1);
                    cfg->server_url[PLEX_MAX_URL - 1] = '\0';
                    strncpy(cfg->relay_url, servers[0].relay_url, PLEX_MAX_URL - 1);
                    cfg->relay_url[PLEX_MAX_URL - 1] = '\0';
                    strncpy(cfg->server_name, servers[0].name, PLEX_MAX_STR - 1);
                    cfg->server_name[PLEX_MAX_STR - 1] = '\0';
                    strncpy(cfg->server_id, servers[0].id, PLEX_MAX_STR - 1);
                    cfg->server_id[PLEX_MAX_STR - 1] = '\0';
                    PLEX_LOG("[Auth] Saving config...\n");
                    plex_config_save(cfg);
                    PLEX_LOG("[Auth] Returning MODULE_BROWSE\n");
                    return MODULE_BROWSE;
                }

                dirty = 1;
            }

            /* Input */
            if (PAD_justRepeated(BTN_UP)) {
                server_selected = (server_selected > 0) ? server_selected - 1 : server_count - 1;
                dirty = 1;
            } else if (PAD_justRepeated(BTN_DOWN)) {
                server_selected = (server_selected < server_count - 1) ? server_selected + 1 : 0;
                dirty = 1;
            } else if (PAD_justPressed(BTN_A)) {
                PlexConfig *cfg = plex_config_get_mutable();
                strncpy(cfg->token, pin.token, PLEX_MAX_STR - 1);
                cfg->token[PLEX_MAX_STR - 1] = '\0';
                strncpy(cfg->server_url, servers[server_selected].url, PLEX_MAX_URL - 1);
                cfg->server_url[PLEX_MAX_URL - 1] = '\0';
                strncpy(cfg->relay_url, servers[server_selected].relay_url, PLEX_MAX_URL - 1);
                cfg->relay_url[PLEX_MAX_URL - 1] = '\0';
                strncpy(cfg->server_name, servers[server_selected].name, PLEX_MAX_STR - 1);
                cfg->server_name[PLEX_MAX_STR - 1] = '\0';
                strncpy(cfg->server_id, servers[server_selected].id, PLEX_MAX_STR - 1);
                cfg->server_id[PLEX_MAX_STR - 1] = '\0';
                plex_config_save(cfg);
                return MODULE_BROWSE;
            } else if (PAD_justPressed(BTN_B)) {
                /* Go back to PIN — re-create PIN */
                memset(&pin, 0, sizeof(pin));
                server_count = 0;
                server_selected = 0;
                server_scroll = 0;
                if (plex_auth_create_pin(&pin) != 0) {
                    state = AUTH_STATE_ERROR;
                    error_msg = "Failed to create PIN";
                } else {
                    state = AUTH_STATE_PIN;
                    entry_time = SDL_GetTicks();
                    last_poll_time = entry_time;
                }
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
        }
        /* ---------------------------------------------------------------
         * Error screen
         * --------------------------------------------------------------- */
        else if (state == AUTH_STATE_ERROR) {
            /* Input */
            if (PAD_justPressed(BTN_A)) {
                /* Retry — restart PIN flow */
                memset(&pin, 0, sizeof(pin));
                server_count = 0;
                server_selected = 0;
                server_scroll = 0;
                if (plex_auth_create_pin(&pin) != 0) {
                    error_msg = "Failed to create PIN";
                    /* Stay in error state */
                } else {
                    state = AUTH_STATE_PIN;
                    entry_time = SDL_GetTicks();
                    last_poll_time = entry_time;
                }
                dirty = 1;
                GFX_sync();
                continue;
            } else if (PAD_justPressed(BTN_B)) {
                return MODULE_QUIT;
            }

            if (dirty) {
                render_error_screen(screen, error_msg);
                GFX_flip(screen);
                dirty = 0;
            } else {
                GFX_sync();
            }
        }
    }

    /* Unreachable */
    return MODULE_QUIT;
}
