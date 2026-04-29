#ifndef PLEX_API_H
#define PLEX_API_H

#include "plex_models.h"

/*
 * Get list of music libraries (type="artist").
 * Sets *count. Returns 0 on success.
 */
int plex_api_get_libraries(const PlexConfig *cfg, PlexLibrary libs[], int *count);

/*
 * Get paginated artists for a library section.
 * offset: 0-based start. max_count: maximum items to write into artists[];
 * pass PLEX_MAX_ITEMS for no limit on first page.
 * page: filled with total/offset/count.
 * Returns 0 on success.
 */
int plex_api_get_artists(const PlexConfig *cfg, int section_id,
                         int offset, int max_count,
                         PlexArtist artists[], PlexPage *page);

/*
 * Get albums for an artist (by artist rating_key).
 * Returns 0 on success.
 */
int plex_api_get_albums(const PlexConfig *cfg, int artist_rating_key,
                        PlexAlbum albums[], int *count);

/*
 * Get tracks for an album (by album rating_key).
 * Returns 0 on success.
 */
int plex_api_get_tracks(const PlexConfig *cfg, int album_rating_key,
                        PlexTrack tracks[], int *count);

/*
 * Build the full authenticated stream URL for a track.
 * out_url_size controls the buffer size; callers must supply a buffer large
 * enough for the full URL (Opus transcode URLs can exceed 650 chars).
 */
void plex_api_get_stream_url(const PlexConfig *cfg, const PlexTrack *track,
                             char *out_url, int out_url_size);

/*
 * Build a Plex audio transcode URL. Appends audioCodec=opus&audioBitrate={kbps} params.
 * bitrate_kbps must be > 0. out_url_size controls the buffer size; callers must
 * supply a buffer large enough for the full URL (typically 2048 bytes).
 */
void plex_api_get_transcode_url(const PlexConfig *cfg, const PlexTrack *track,
                                int bitrate_kbps, char *out_url, int out_url_size);

/*
 * Report playback state to Plex (scrobbling).
 * state: "playing", "paused", "stopped"
 * Fire-and-forget; errors are silently ignored.
 */
void plex_api_timeline(const PlexConfig *cfg, int rating_key,
                       const char *state, int time_ms, int duration_ms);

/*
 * Mark a track as fully played (scrobble).
 * Fire-and-forget.
 */
void plex_api_scrobble(const PlexConfig *cfg, int rating_key);

/*
 * Get paginated list of all albums in a library section, sorted by year descending.
 * offset: 0-based start. max_count: maximum items to write into albums[].
 * page: filled with total/offset/count.
 * Returns 0 on success, -1 on error.
 */
int plex_api_get_all_albums(const PlexConfig *cfg, int section_id,
                             int offset, int max_count,
                             PlexAlbum albums[], PlexPage *page);

#endif /* PLEX_API_H */
