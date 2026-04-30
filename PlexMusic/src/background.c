#include "background.h"
#include "module_player.h"
#include "player.h"

static BackgroundPlayerType active_bg = BG_NONE;

void Background_setActive(BackgroundPlayerType type) {
    active_bg = type;
}

BackgroundPlayerType Background_getActive(void) {
    return active_bg;
}

void Background_stopAll(void) {
    switch (active_bg) {
        case BG_MUSIC:
            Player_stop();
            break;
        case BG_NONE:
            break;
    }
    active_bg = BG_NONE;
}

bool Background_isPlaying(void) {
    switch (active_bg) {
        case BG_MUSIC:
            return Player_getState() != PLAYER_STATE_STOPPED;
        case BG_NONE:
            break;
    }
    return false;
}

void Background_tick(void) {
    switch (active_bg) {
        case BG_MUSIC:
            PlayerModule_backgroundTick();
            break;
        case BG_NONE:
            break;
    }
}
