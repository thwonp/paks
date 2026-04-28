#define _GNU_SOURCE
#include "plex_api.h"
#include "plex_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/parson/parson.h"
#include "api.h"
#include "plex_log.h"

/* Page size for paginated artist fetch */
#define ARTIST_PAGE_SIZE 50
/* Response buffer: 2 MB to handle large artist listings with rich metadata */
#define RESP_BUF_SIZE (2 * 1024 * 1024)

/* ------------------------------------------------------------------
 * Internal helper: fetch a JSON endpoint on the Plex server.
 * Builds full URL, appends token as query parameter.
 * Returns heap-allocated JSON_Value* on success, NULL on error.
 * Caller must call json_value_free().
 * ------------------------------------------------------------------ */
static JSON_Value *server_get(const PlexConfig *cfg, const char *path_and_query)
{
    /* Determine separator: does path_and_query already have '?' */
    const char *sep = strchr(path_and_query, '?') ? "&" : "?";

    char url[PLEX_MAX_URL + 256];
    int url_len = snprintf(url, sizeof(url), "%s%s%sX-Plex-Token=%s",
                           cfg->server_url, path_and_query, sep, cfg->token);
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        PLEX_LOG_ERROR("[PlexAPI] URL too long for path: %s\n", path_and_query);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc(RESP_BUF_SIZE);
    if (!buf) return NULL;

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = cfg->token,
        .timeout_sec = 15
    };

    int n = plex_net_fetch(url, buf, RESP_BUF_SIZE, &opts);
    if (n <= 0) {
        /* Log path only — the full URL contains the auth token */
        PLEX_LOG_ERROR("[PlexAPI] fetch failed for path: %s\n", path_and_query);
        free(buf);
        return NULL;
    }
    buf[n] = '\0';

    JSON_Value *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        /* Log path only — the full URL contains the auth token */
        PLEX_LOG_ERROR("[PlexAPI] JSON parse failed for path: %s\n", path_and_query);
    }
    return root;
}

/* ------------------------------------------------------------------
 * plex_api_get_libraries
 * GET {server_url}/library/sections
 * Response: MediaContainer.Directory[] — filter type == "artist"
 * ------------------------------------------------------------------ */
int plex_api_get_libraries(const PlexConfig *cfg, PlexLibrary libs[], int *count)
{
    if (!cfg || !libs || !count) return -1;
    *count = 0;

    JSON_Value *root = server_get(cfg, "/library/sections");
    if (!root) return -1;

    JSON_Object *mc  = json_object_dotget_object(
                           json_value_get_object(root), "MediaContainer");
    JSON_Array  *dirs = mc ? json_object_get_array(mc, "Directory") : NULL;

    if (!dirs) {
        json_value_free(root);
        return -1;
    }

    size_t total = json_array_get_count(dirs);
    for (size_t i = 0; i < total && *count < PLEX_MAX_ITEMS; i++) {
        JSON_Object *d = json_array_get_object(dirs, i);
        if (!d) continue;

        const char *type = json_object_get_string(d, "type");
        if (!type || strcmp(type, "artist") != 0) continue;

        PlexLibrary *lib = &libs[*count];
        memset(lib, 0, sizeof(*lib));

        const char *key_str = json_object_get_string(d, "key");
        const char *title   = json_object_get_string(d, "title");

        lib->section_id = key_str ? atoi(key_str) : 0;
        if (title) strncpy(lib->title, title, sizeof(lib->title) - 1);
        strncpy(lib->type, "artist", sizeof(lib->type) - 1);

        (*count)++;
    }

    json_value_free(root);
    return 0;
}

/* ------------------------------------------------------------------
 * plex_api_get_artists
 * GET {server_url}/library/sections/{section_id}/all
 *     ?X-Plex-Container-Start={offset}&X-Plex-Container-Size=50
 * Response: MediaContainer.Metadata[]
 * ------------------------------------------------------------------ */
int plex_api_get_artists(const PlexConfig *cfg, int section_id,
                         int offset, int max_count,
                         PlexArtist artists[], PlexPage *page)
{
    if (!cfg || !artists || !page) return -1;
    memset(page, 0, sizeof(*page));

    if (max_count <= 0) return 0;

    int page_size = max_count < ARTIST_PAGE_SIZE ? max_count : ARTIST_PAGE_SIZE;

    char path[512];
    snprintf(path, sizeof(path),
             "/library/sections/%d/all"
             "?X-Plex-Container-Start=%d&X-Plex-Container-Size=%d",
             section_id, offset, page_size);

    JSON_Value *root = server_get(cfg, path);
    if (!root) return -1;

    JSON_Object *mc = json_object_dotget_object(
                          json_value_get_object(root), "MediaContainer");
    if (!mc) { json_value_free(root); return -1; }

    page->total  = (int)json_object_get_number(mc, "totalSize");
    page->offset = offset;

    JSON_Array *meta = json_object_get_array(mc, "Metadata");
    if (!meta) {
        json_value_free(root);
        return 0;   /* empty result is not an error */
    }

    size_t n = json_array_get_count(meta);
    int    loaded = 0;

    for (size_t i = 0; i < n && loaded < max_count; i++) {
        JSON_Object *item = json_array_get_object(meta, i);
        if (!item) continue;

        PlexArtist *a = &artists[loaded];
        memset(a, 0, sizeof(*a));

        const char *rk = json_object_get_string(item, "ratingKey");
        a->rating_key      = rk ? atoi(rk) : 0;
        const char *title  = json_object_get_string(item, "title");
        const char *thumb  = json_object_get_string(item, "thumb");

        if (title) strncpy(a->title, title, sizeof(a->title) - 1);
        if (thumb) strncpy(a->thumb, thumb, sizeof(a->thumb) - 1);

        loaded++;
    }

    page->count = loaded;
    json_value_free(root);
    return 0;
}

/* ------------------------------------------------------------------
 * plex_api_get_all_albums
 * GET {server_url}/library/sections/{section_id}/all
 *     ?type=9&sort=year:desc
 *     &X-Plex-Container-Start={offset}&X-Plex-Container-Size=50
 * Response: MediaContainer.Metadata[]
 * ------------------------------------------------------------------ */
int plex_api_get_all_albums(const PlexConfig *cfg, int section_id,
                             int offset, int max_count,
                             PlexAlbum albums[], PlexPage *page)
{
    if (!cfg || !albums || !page) return -1;
    memset(page, 0, sizeof(*page));

    if (max_count <= 0) return 0;

    int page_size = max_count < ARTIST_PAGE_SIZE ? max_count : ARTIST_PAGE_SIZE;

    char path[512];
    snprintf(path, sizeof(path),
             "/library/sections/%d/all"
             "?type=9"
             "&sort=year:desc"
             "&X-Plex-Container-Start=%d"
             "&X-Plex-Container-Size=%d",
             section_id, offset, page_size);

    JSON_Value *root = server_get(cfg, path);
    if (!root) return -1;

    JSON_Object *mc = json_object_dotget_object(
                         json_value_get_object(root), "MediaContainer");
    if (!mc) { json_value_free(root); return -1; }

    page->total  = (int)json_object_get_number(mc, "totalSize");
    page->offset = offset;

    JSON_Array *meta = json_object_get_array(mc, "Metadata");
    if (!meta) {
        json_value_free(root);
        return 0;   /* empty result is not an error */
    }

    size_t n = json_array_get_count(meta);
    int    loaded = 0;

    for (size_t i = 0; i < n && loaded < max_count; i++) {
        JSON_Object *item = json_array_get_object(meta, i);
        if (!item) continue;

        PlexAlbum *al = &albums[loaded];
        memset(al, 0, sizeof(*al));

        const char *rk     = json_object_get_string(item, "ratingKey");
        al->rating_key     = rk ? atoi(rk) : 0;
        const char *prk    = json_object_get_string(item, "parentRatingKey");
        al->artist_id      = prk ? atoi(prk) : 0;
        const char *title  = json_object_get_string(item, "title");
        const char *artist = json_object_get_string(item, "parentTitle");
        const char *thumb  = json_object_get_string(item, "thumb");
        double year_d      = json_object_get_number(item, "year");

        if (title)  strncpy(al->title,  title,  sizeof(al->title) - 1);
        if (artist) strncpy(al->artist, artist, sizeof(al->artist) - 1);
        if (thumb)  strncpy(al->thumb,  thumb,  sizeof(al->thumb) - 1);
        if (year_d > 0)
            snprintf(al->year, sizeof(al->year), "%d", (int)year_d);

        loaded++;
    }

    page->count = loaded;
    json_value_free(root);
    return 0;
}

/* ------------------------------------------------------------------
 * plex_api_get_albums
 * GET {server_url}/library/metadata/{artist_rating_key}/children
 * Response: MediaContainer.Metadata[]
 * ------------------------------------------------------------------ */
int plex_api_get_albums(const PlexConfig *cfg, int artist_rating_key,
                        PlexAlbum albums[], int *count)
{
    if (!cfg || !albums || !count) return -1;
    *count = 0;

    char path[256];
    snprintf(path, sizeof(path),
             "/library/metadata/%d/children", artist_rating_key);

    JSON_Value *root = server_get(cfg, path);
    if (!root) return -1;

    JSON_Object *mc = json_object_dotget_object(
                          json_value_get_object(root), "MediaContainer");
    JSON_Array  *meta = mc ? json_object_get_array(mc, "Metadata") : NULL;

    if (!meta) { json_value_free(root); return 0; }

    size_t total = json_array_get_count(meta);
    for (size_t i = 0; i < total && *count < PLEX_MAX_ITEMS; i++) {
        JSON_Object *item = json_array_get_object(meta, i);
        if (!item) continue;

        PlexAlbum *al = &albums[*count];
        memset(al, 0, sizeof(*al));

        const char *rk         = json_object_get_string(item, "ratingKey");
        al->rating_key         = rk ? atoi(rk) : 0;
        const char *prk        = json_object_get_string(item, "parentRatingKey");
        al->artist_id          = prk ? atoi(prk) : 0;
        const char *title      = json_object_get_string(item, "title");
        const char *artist     = json_object_get_string(item, "parentTitle");
        const char *thumb      = json_object_get_string(item, "thumb");
        double year_d          = json_object_get_number(item, "year");

        if (title)  strncpy(al->title,  title,  sizeof(al->title) - 1);
        if (artist) strncpy(al->artist, artist, sizeof(al->artist) - 1);
        if (thumb)  strncpy(al->thumb,  thumb,  sizeof(al->thumb) - 1);
        if (year_d > 0)
            snprintf(al->year, sizeof(al->year), "%d", (int)year_d);

        (*count)++;
    }

    json_value_free(root);
    return 0;
}

/* ------------------------------------------------------------------
 * plex_api_get_tracks
 * GET {server_url}/library/metadata/{album_rating_key}/children
 * Response: MediaContainer.Metadata[]
 *   ratingKey, title, grandparentTitle, parentTitle, duration, index,
 *   thumb, Media[0].Part[0].key
 * ------------------------------------------------------------------ */
int plex_api_get_tracks(const PlexConfig *cfg, int album_rating_key,
                        PlexTrack tracks[], int *count)
{
    if (!cfg || !tracks || !count) return -1;
    *count = 0;

    char path[256];
    snprintf(path, sizeof(path),
             "/library/metadata/%d/children", album_rating_key);

    JSON_Value *root = server_get(cfg, path);
    if (!root) return -1;

    JSON_Object *mc = json_object_dotget_object(
                          json_value_get_object(root), "MediaContainer");
    JSON_Array  *meta = mc ? json_object_get_array(mc, "Metadata") : NULL;

    if (!meta) { json_value_free(root); return 0; }

    size_t total = json_array_get_count(meta);
    for (size_t i = 0; i < total && *count < PLEX_MAX_ITEMS; i++) {
        JSON_Object *item = json_array_get_object(meta, i);
        if (!item) continue;

        PlexTrack *t = &tracks[*count];
        memset(t, 0, sizeof(*t));

        const char *rk         = json_object_get_string(item, "ratingKey");
        t->rating_key          = rk ? atoi(rk) : 0;
        t->duration_ms         = (int)json_object_get_number(item, "duration");
        t->track_number        = (int)json_object_get_number(item, "index");

        const char *title      = json_object_get_string(item, "title");
        const char *artist     = json_object_get_string(item, "grandparentTitle");
        const char *album      = json_object_get_string(item, "parentTitle");
        const char *thumb      = json_object_get_string(item, "thumb");

        if (title)  strncpy(t->title,  title,  sizeof(t->title) - 1);
        if (artist) strncpy(t->artist, artist, sizeof(t->artist) - 1);
        if (album)  strncpy(t->album,  album,  sizeof(t->album) - 1);
        if (thumb)  strncpy(t->thumb,  thumb,  sizeof(t->thumb) - 1);

        /* Extract media_key from Media[0].Part[0].key */
        JSON_Array *media_arr = json_object_get_array(item, "Media");
        if (media_arr && json_array_get_count(media_arr) > 0) {
            JSON_Object *media = json_array_get_object(media_arr, 0);
            if (media) {
                JSON_Array *part_arr = json_object_get_array(media, "Part");
                if (part_arr && json_array_get_count(part_arr) > 0) {
                    JSON_Object *part = json_array_get_object(part_arr, 0);
                    if (part) {
                        const char *key = json_object_get_string(part, "key");
                        if (key)
                            strncpy(t->media_key, key, sizeof(t->media_key) - 1);
                    }
                }
            }
        }

        (*count)++;
    }

    json_value_free(root);
    return 0;
}

/* ------------------------------------------------------------------
 * plex_api_get_stream_url
 * Produces: {server_url}{media_key}?X-Plex-Token={token}
 * ------------------------------------------------------------------ */
void plex_api_get_stream_url(const PlexConfig *cfg, const PlexTrack *track,
                             char *out_url, int out_url_size)
{
    if (!cfg || !track || !out_url || out_url_size <= 0) return;
    snprintf(out_url, out_url_size, "%s%s?X-Plex-Token=%s",
             cfg->server_url, track->media_key, cfg->token);
}

/* ------------------------------------------------------------------
 * plex_api_get_transcode_url
 * Uses /audio/:/transcode/universal/start — Plex transcodes to MP3 VBR.
 * bitrate_kbps is accepted but ignored (server does not honour it).
 * ------------------------------------------------------------------ */
void plex_api_get_transcode_url(const PlexConfig *cfg, const PlexTrack *track,
                                int bitrate_kbps, char *out_url, int out_url_size)
{
    (void)bitrate_kbps;  /* Plex server ignores audioBitrate; param kept for ABI compat */
    if (!cfg || !track || !out_url || out_url_size <= 0) return;
    snprintf(out_url, out_url_size,
             "%s/audio/:/transcode/universal/start"
             "?path=%%2Flibrary%%2Fmetadata%%2F%d"
             "&mediaIndex=0&partIndex=0&protocol=http"
             "&copyts=1&directStreamAudio=0"
             "&session=pm_%d"
             "&X-Plex-Token=%s",
             cfg->server_url, track->rating_key, track->rating_key, cfg->token);
}

/* ------------------------------------------------------------------
 * plex_api_timeline
 * GET {server_url}/:/timeline?ratingKey=...&state=...&time=...
 *     &duration=...&identifier=com.plexapp.plugins.library
 * Fire-and-forget; small timeout (5s).
 * ------------------------------------------------------------------ */
void plex_api_timeline(const PlexConfig *cfg, int rating_key,
                       const char *state, int time_ms, int duration_ms)
{
    if (!cfg || !state) return;

    char path[512];
    snprintf(path, sizeof(path),
             "/:/timeline"
             "?ratingKey=%d"
             "&state=%s"
             "&time=%d"
             "&duration=%d"
             "&identifier=com.plexapp.plugins.library",
             rating_key, state, time_ms, duration_ms);

    /* Build URL manually (token already appended by server_get's convention) */
    char url[PLEX_MAX_URL + 256];
    snprintf(url, sizeof(url), "%s%s&X-Plex-Token=%s",
             cfg->server_url, path, cfg->token);

    /* Allocate a small discard buffer */
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return;

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = cfg->token,
        .timeout_sec = 5
    };

    plex_net_fetch(url, buf, 4096, &opts);  /* ignore errors */
    free(buf);
}

/* ------------------------------------------------------------------
 * plex_api_scrobble
 * GET {server_url}/:/scrobble?key={rating_key}
 *     &identifier=com.plexapp.plugins.library
 * Fire-and-forget.
 * ------------------------------------------------------------------ */
void plex_api_scrobble(const PlexConfig *cfg, int rating_key)
{
    if (!cfg) return;

    char url[PLEX_MAX_URL + 256];
    snprintf(url, sizeof(url),
             "%s/:/scrobble"
             "?key=%d"
             "&identifier=com.plexapp.plugins.library"
             "&X-Plex-Token=%s",
             cfg->server_url, rating_key, cfg->token);

    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return;

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = cfg->token,
        .timeout_sec = 5
    };

    plex_net_fetch(url, buf, 4096, &opts);  /* ignore errors */
    free(buf);
}
