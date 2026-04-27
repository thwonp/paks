#ifndef MODULE_PLAYER_H
#define MODULE_PLAYER_H

#include <SDL2/SDL.h>
#include "module_auth.h"  /* AppModule */

/*
 * Run the player module (now-playing screen).
 *
 * Prerequisites: plex_queue_get() must return an active queue with
 * current_index and stream_url already set.
 *
 * Returns the next AppModule:
 *   MODULE_BROWSE — user pressed B (back)
 *   MODULE_QUIT   — user pressed Start+long or quit
 */
AppModule module_player_run(SDL_Surface *screen);

#endif /* MODULE_PLAYER_H */
