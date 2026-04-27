#ifndef MODULE_BROWSE_H
#define MODULE_BROWSE_H

#include <SDL2/SDL.h>
#include "module_auth.h"

/*
 * Run the browse module (libraries → artists → albums → tracks).
 * Returns the next AppModule:
 *   MODULE_PLAYER  — user selected a track; queue has been populated
 *   MODULE_QUIT    — user quit the application
 */
AppModule module_browse_run(SDL_Surface *screen);

/* Reset internal state so next call starts fresh (call before sign-out). */
void module_browse_reset(void);

#endif /* MODULE_BROWSE_H */
