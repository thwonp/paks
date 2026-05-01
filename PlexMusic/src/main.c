#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <msettings.h>

#include "api.h"
#include "utils.h"
#include "config.h"

#include "plex_log.h"
#include "plex_config.h"
#include "plex_net.h"
#include "plex_art.h"
#include "player.h"
#include "ui_fonts.h"
#include "module_auth.h"
#include "module_browse.h"
#include "module_player.h"
#include "module_settings.h"
#include "plex_queue.h"
#include "plex_downloads.h"
#include "plex_favorites.h"

/* Global quit flag, set by signal handler */
static volatile bool g_quit = false;

/* Global application config — accessible to future modules via plex_config_get() */
static PlexConfig g_config;

/* Provide read-only access to the global config for other modules */
const PlexConfig *plex_config_get(void)
{
    return &g_config;
}

/* Provide mutable access to the global config — used by module_auth after save */
PlexConfig *plex_config_get_mutable(void)
{
    return &g_config;
}

/* Probe server_url; if unreachable and relay_url is set, switch for this session. */
static void apply_relay_fallback(PlexConfig *cfg)
{
    PLEX_LOG("[Main] apply_relay_fallback: server_url=%s relay_url=%s\n",
             cfg->server_url, cfg->relay_url);
    if (!plex_config_is_valid(cfg) || cfg->relay_url[0] == '\0') return;

    char identity_url[PLEX_MAX_URL + 16];
    snprintf(identity_url, sizeof(identity_url), "%s/identity", cfg->server_url);

    uint8_t probe_buf[4096];
    PlexNetOptions probe_opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = cfg->token,
        .timeout_sec = 3
    };
    int r = plex_net_fetch(identity_url, probe_buf, (int)sizeof(probe_buf), &probe_opts);
    if (r > 0) {
        PLEX_LOG("[Main] server_url reachable: %s\n", cfg->server_url);
    } else {
        PLEX_LOG("[Main] server_url unreachable, switching to relay_url: %s\n", cfg->relay_url);
        strncpy(cfg->server_url, cfg->relay_url, sizeof(cfg->server_url) - 1);
        cfg->server_url[sizeof(cfg->server_url) - 1] = '\0';
    }
}

static void sig_handler(int sig) {
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        g_quit = true;
        break;
    default:
        break;
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    plex_log_init();
    PLEX_LOG("[DIAG] main() entered\n");

    /* --- Platform / SDL init (mirror musicplayer.c sequence) --- */
    SDL_Surface *screen = GFX_init(MODE_MAIN);
    PLEX_LOG("[DIAG] GFX_init done\n");
    PWR_pinToCores(CPU_CORE_PERFORMANCE);
    PLEX_LOG("[DIAG] PWR_pinToCores done\n");

    InitSettings();
    PLEX_LOG("[DIAG] InitSettings done\n");
    PAD_init();
    PLEX_LOG("[DIAG] PAD_init done\n");
    PWR_init();
    PLEX_LOG("[DIAG] PWR_init done\n");
    PWR_disableAutosleep();
    WIFI_init();
    PLEX_LOG("[DIAG] WIFI_init done\n");
    Fonts_load();
    PLEX_LOG("[DIAG] Fonts_load done\n");
    plex_art_init();
    PLEX_LOG("[DIAG] plex_art_init done\n");
    if (Player_init() != 0) {
        PLEX_LOG_ERROR("[Main] Player_init failed\n");
        plex_log_flush();
        return EXIT_FAILURE;
    }
    PLEX_LOG("[DIAG] Player_init done\n");

    plex_downloads_init();
    PLEX_LOG("[DIAG] plex_downloads_init done\n");

    plex_favorites_init();
    PLEX_LOG("[DIAG] plex_favorites_init done\n");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /*
     * Load config from $SHARED_USERDATA_PATH/plexmusic/config.json.
     * Fallback path is /mnt/SDCARD/.userdata/shared/plexmusic/config.json.
     */
    memset(&g_config, 0, sizeof(g_config));
    if (plex_config_load(&g_config) == 0) {
        PLEX_LOG("Config loaded: server=%s\n", g_config.server_url);
    } else {
        PLEX_LOG("No config found — starting at auth screen\n");
    }
    PLEX_LOG("[DIAG] plex_config_load done\n");

    if (!g_config.offline_mode) {
        SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
        GFX_flip(screen);
        apply_relay_fallback(&g_config);
    }
    PLEX_LOG("[DIAG] startup URL probe done\n");

    /*
     * Determine starting module.
     * If the config has a valid token and server URL, go straight to browse.
     * Otherwise begin at auth.
     */
    bool has_token = plex_config_is_valid(&g_config);
    AppModule current = has_token ? MODULE_BROWSE : MODULE_AUTH;
    PLEX_LOG("[DIAG] plex_config_is_valid done, module=%d\n", current);

    /* --- Main module loop --- */
    while (!g_quit) {
        AppModule next;

        switch (current) {
        case MODULE_AUTH:
            next = module_auth_run(screen);
            if (next == MODULE_BROWSE && !g_config.offline_mode) {
                SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
                GFX_flip(screen);
                apply_relay_fallback(&g_config);
            }
            break;
        case MODULE_BROWSE:
            next = module_browse_run(screen);
            break;
        case MODULE_PLAYER:
            next = module_player_run(screen);
            break;
        case MODULE_SETTINGS:
            next = module_settings_run(screen);
            break;
        case MODULE_QUIT:
        default:
            g_quit = true;
            next = MODULE_QUIT;
            break;
        }

        if (next == MODULE_QUIT) {
            g_quit = true;
        } else {
            current = next;
        }
    }

    /* --- Clean shutdown --- */
    PWR_enableAutosleep();
    plex_downloads_quit();
    plex_favorites_quit();
    plex_art_cleanup();
    Player_quit();
    Fonts_unload();
    PWR_quit();
    PAD_quit();
    GFX_quit();
    plex_log_flush();
    return EXIT_SUCCESS;
}
