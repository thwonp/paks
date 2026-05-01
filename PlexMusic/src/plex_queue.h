#ifndef PLEX_QUEUE_H
#define PLEX_QUEUE_H

#include <stdbool.h>
#include "plex_models.h"

/* Maximum tracks in a queue (one album at a time) */
#define PLEX_QUEUE_MAX_TRACKS 200

typedef enum {
    REPEAT_OFF = 0,
    REPEAT_ALL,
    REPEAT_ONE
} RepeatMode;

typedef struct {
    PlexTrack tracks[PLEX_QUEUE_MAX_TRACKS];
    int count;
    int current_index;       /* 0-based index into tracks[] */
    char stream_url[2048]; /* authenticated stream URL for current_index */
    bool active;             /* true if queue has been populated */
    bool       shuffle;
    RepeatMode repeat_mode;
    int        shuffle_order[PLEX_QUEUE_MAX_TRACKS];
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

/* Returns pointer to the currently active track (respects shuffle order) */
const PlexTrack *plex_queue_current_track(void);

/* Toggle shuffle on/off. When enabling, Fisher-Yates shuffles shuffle_order[]
 * while keeping current track at current_index. When disabling, restores
 * identity order and updates current_index to point to the same track. */
void plex_queue_toggle_shuffle(void);

/* Cycle repeat mode: Off -> All -> One -> Off */
void plex_queue_cycle_repeat(void);

#endif /* PLEX_QUEUE_H */
