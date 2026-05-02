/*
 * album_art.h - Stub for PlexMusic.pak
 *
 * player.c calls album_art_clear() on stop and album_art_get() as a fallback
 * in Player_getAlbumArt(). These stubs satisfy those call sites; Plex album
 * art will be handled separately in task 008.
 */
#ifndef __ALBUM_ART_H__
#define __ALBUM_ART_H__

#include <stdbool.h>

struct SDL_Surface;

void             album_art_init(void);
void             album_art_cleanup(void);
void             album_art_fetch(const char* artist, const char* title);
struct SDL_Surface* album_art_get(void);
bool             album_art_is_fetching(void);
void             album_art_clear(void);

#endif /* __ALBUM_ART_H__ */
