#ifndef PLEX_ART_H
#define PLEX_ART_H

#include <stdbool.h>
#include <SDL2/SDL.h>
#include "plex_models.h"

/*
 * Async cover art fetcher with disk cache.
 *
 * At most one fetch is in-flight at any time. Calling plex_art_fetch()
 * while a fetch is active cancels (discards) the previous request.
 * The main thread polls plex_art_get() to retrieve the result.
 */

/* Initialize module state. Call once at startup. */
void plex_art_init(void);

/* Release all resources. Joins any active thread. */
void plex_art_cleanup(void);

/*
 * Start an async fetch for the given thumb_path.
 * Cancels any in-progress fetch.
 * cfg provides server_url and token.
 * No-op if thumb_path is NULL or empty.
 */
void plex_art_fetch(const PlexConfig *cfg, const char *thumb_path);

/*
 * Returns the current SDL_Surface* if available, NULL if still loading or
 * no art has been loaded. Joins the worker thread when result is ready.
 */
SDL_Surface *plex_art_get(void);

/* Returns true if a fetch is currently in progress. */
bool plex_art_is_fetching(void);

/* Cancel any in-flight fetch and free the current surface. */
void plex_art_clear(void);

#endif /* PLEX_ART_H */
