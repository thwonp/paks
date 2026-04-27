#ifndef MODULE_AUTH_H
#define MODULE_AUTH_H

#include <SDL2/SDL.h>

/* Forward-declare AppModule so module_auth.h can be included without main.c. */
typedef enum {
    MODULE_AUTH,
    MODULE_BROWSE,
    MODULE_PLAYER,
    MODULE_SETTINGS,
    MODULE_QUIT
} AppModule;

/*
 * Run the auth module. Returns the next AppModule.
 *   MODULE_BROWSE — authenticated + server selected, ready to browse
 *   MODULE_QUIT   — user chose to quit
 */
AppModule module_auth_run(SDL_Surface *screen);

#endif /* MODULE_AUTH_H */
