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

#include "plex_config.h"
#include "plex_art.h"
#include "player.h"
#include "ui_fonts.h"
#include "module_auth.h"
#include "module_browse.h"
#include "module_player.h"
#include "module_settings.h"
#include "plex_queue.h"

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

    fprintf(stderr, "[DIAG] main() entered\n");

    /* --- Platform / SDL init (mirror musicplayer.c sequence) --- */
    SDL_Surface *screen = GFX_init(MODE_MAIN);
    fprintf(stderr, "[DIAG] GFX_init done\n");
    PWR_pinToCores(CPU_CORE_PERFORMANCE);
    fprintf(stderr, "[DIAG] PWR_pinToCores done\n");

    InitSettings();
    fprintf(stderr, "[DIAG] InitSettings done\n");
    PAD_init();
    fprintf(stderr, "[DIAG] PAD_init done\n");
    PWR_init();
    fprintf(stderr, "[DIAG] PWR_init done\n");
    WIFI_init();
    fprintf(stderr, "[DIAG] WIFI_init done\n");
    Fonts_load();
    fprintf(stderr, "[DIAG] Fonts_load done\n");
    plex_art_init();
    fprintf(stderr, "[DIAG] plex_art_init done\n");
    if (Player_init() != 0) {
        LOG_error("[Main] Player_init failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "[DIAG] Player_init done\n");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /*
     * Load config from $SHARED_USERDATA_PATH/plexmusic/config.json.
     * Fallback path is /mnt/SDCARD/.userdata/shared/plexmusic/config.json.
     */
    memset(&g_config, 0, sizeof(g_config));
    if (plex_config_load(&g_config) == 0) {
        LOG_info("Config loaded: server=%s\n", g_config.server_url);
    } else {
        LOG_info("No config found — starting at auth screen\n");
    }
    fprintf(stderr, "[DIAG] plex_config_load done\n");

    /*
     * Determine starting module.
     * If the config has a valid token and server URL, go straight to browse.
     * Otherwise begin at auth.
     */
    bool has_token = plex_config_is_valid(&g_config);
    AppModule current = has_token ? MODULE_BROWSE : MODULE_AUTH;
    fprintf(stderr, "[DIAG] plex_config_is_valid done, module=%d\n", current);

    /* --- Main module loop --- */
    while (!g_quit) {
        AppModule next;

        switch (current) {
        case MODULE_AUTH:
            next = module_auth_run(screen);
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
    plex_art_cleanup();
    Player_quit();
    Fonts_unload();
    PWR_quit();
    PAD_quit();
    GFX_quit();

    return EXIT_SUCCESS;
}
