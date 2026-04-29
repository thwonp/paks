#ifndef PLEX_DOWNLOADS_H
#define PLEX_DOWNLOADS_H

#include <stdbool.h>
#include "plex_models.h"
#include "plex_config.h"

typedef enum {
    DL_STATUS_NONE        = 0,  /* not in queue and not downloaded */
    DL_STATUS_QUEUED      = 1,  /* in queue, not yet started */
    DL_STATUS_DOWNLOADING = 2,  /* currently downloading */
    DL_STATUS_DONE        = 3,  /* fully downloaded and in manifest */
} DlStatus;

/* Call once at startup. Loads manifest from disk. Starts worker thread. */
void plex_downloads_init(void);

/* Call at shutdown. Signals worker to stop, joins thread. */
void plex_downloads_quit(void);

/*
 * Queue an album for download. No-op if already downloaded (DL_STATUS_DONE)
 * or queue is full. cfg provides server_url + token; album_rating_key and
 * album_title identify the album; artist_id/artist_name come from the browse
 * module; thumb is the album thumb path.
 */
void plex_downloads_queue_album(const PlexConfig *cfg,
                                int album_rating_key,
                                const char *album_title,
                                int artist_id,
                                const char *artist_name,
                                const char *thumb,
                                const char *year);

/* Returns the download status for the given album rating_key. */
DlStatus plex_downloads_album_status(int album_rating_key);

/* Returns true if any album is currently being downloaded. */
bool plex_downloads_is_active(void);

/*
 * If album_id is the currently-downloading album, sets *completed and *total
 * and returns true. Otherwise returns false (caller should not use the output
 * values). Safe to call from any thread.
 */
bool plex_downloads_album_progress(int album_id, int *completed, int *total);

/*
 * Offline browse query functions. All return data from the in-memory manifest.
 * Caller provides output arrays; returns count of items written.
 * Max items written is capped at out_max.
 * These are safe to call from the main thread at any time.
 */

/* Unique artists in the manifest, sorted by name. */
int plex_downloads_get_artists(PlexArtist *out, int out_max);

/* Albums for a given artist_id (from manifest). */
int plex_downloads_get_albums_for_artist(int artist_id,
                                         PlexAlbum *out, int out_max);

/*
 * Tracks for a given album_id (from manifest).
 * Populates PlexTrack fields including local_path.
 */
int plex_downloads_get_tracks_for_album(int album_id,
                                        PlexTrack *out, int out_max);

/*
 * All downloaded albums, sorted by year descending (albums without a year
 * sort to the end). Returns count of items written, capped at out_max.
 */
int plex_downloads_get_all_albums(PlexAlbum *out, int out_max);

#endif /* PLEX_DOWNLOADS_H */
