#ifndef PLEX_QUEUE_H
#define PLEX_QUEUE_H

#include <stdbool.h>
#include "plex_models.h"

/* Maximum tracks in a queue (one album at a time) */
#define PLEX_QUEUE_MAX_TRACKS 200

typedef struct {
    PlexTrack tracks[PLEX_QUEUE_MAX_TRACKS];
    int count;
    int current_index;       /* 0-based index into tracks[] */
    char stream_url[2048]; /* authenticated stream URL for current_index */
    bool active;             /* true if queue has been populated */
} PlexQueue;

/* Access the global queue */
PlexQueue *plex_queue_get(void);

/* Populate from an album's track list. Builds stream_url for tracks[index]. */
void plex_queue_set(const PlexConfig *cfg,
                    const PlexTrack *tracks, int count, int start_index);

/* Advance to next track. Returns false if already at last track. */
bool plex_queue_next(const PlexConfig *cfg);

/* Move to previous track. Returns false if already at first track. */
bool plex_queue_prev(const PlexConfig *cfg);

/* True if there is a next/prev track available */
bool plex_queue_has_next(void);
bool plex_queue_has_prev(void);

void plex_queue_clear(void);

#endif /* PLEX_QUEUE_H */
