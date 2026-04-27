/* album_art.c - Stub implementation for PlexMusic.pak */
#include <stddef.h>
#include "album_art.h"

void album_art_init(void) {}
void album_art_cleanup(void) {}
void album_art_fetch(const char* artist, const char* title) {
    (void)artist;
    (void)title;
}
struct SDL_Surface* album_art_get(void) { return NULL; }
bool album_art_is_fetching(void) { return false; }
void album_art_clear(void) {}
