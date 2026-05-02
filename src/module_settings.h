#ifndef MODULE_SETTINGS_H
#define MODULE_SETTINGS_H

#include <SDL2/SDL.h>
#include "module_auth.h"

/*
 * Run the settings module.
 *   MODULE_AUTH   — user signed out
 *   MODULE_BROWSE — user pressed B or completed a server switch
 *   MODULE_QUIT   — Start long-press quit
 */
AppModule module_settings_run(SDL_Surface *screen);

#endif /* MODULE_SETTINGS_H */
