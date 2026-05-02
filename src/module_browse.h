#ifndef MODULE_BROWSE_H
#define MODULE_BROWSE_H

#include <SDL2/SDL.h>
#include "module_auth.h"

/*
 * Run the browse module (libraries → artists → albums → tracks).
 * Returns the next AppModule:
 *   MODULE_PLAYER    — user selected a track; queue has been populated
 *   MODULE_SETTINGS  — user navigated to settings
 *   MODULE_QUIT      — user quit the application
 */
AppModule module_browse_run(SDL_Surface *screen);

/* Reset internal state so next call starts fresh (call before sign-out). */
void module_browse_reset(void);

/* Request the library picker on the next call to module_browse_run(). */
void module_browse_request_library_pick(void);

#endif /* MODULE_BROWSE_H */
