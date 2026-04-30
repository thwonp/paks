#pragma once

#include "plex_models.h"
#include <stdbool.h>

#define PLEX_MAX_FAVORITES 512

void plex_favorites_init(void);
void plex_favorites_quit(void);

/* Add if not present, remove if present. Returns true if now favorited. */
bool plex_favorites_toggle(const PlexTrack *t);

bool plex_favorites_contains(int rating_key);

/* Copy current list (insertion order) into out[0..max-1]. Returns count written. */
int  plex_favorites_get(PlexTrack *out, int max);

int  plex_favorites_count(void);
