#define _GNU_SOURCE
#include "plex_favorites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "include/parson/parson.h"
#include "plex_log.h"

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

#define FAVORITES_FILENAME  "favorites.json"
#define FALLBACK_BASE_DIR   "/mnt/SDCARD/.userdata/shared/plexmusic"

/* ------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------ */

static PlexTrack g_favs[PLEX_MAX_FAVORITES];
static int       g_fav_count = 0;

/* ------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------ */

static void favorites_path(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base) {
        snprintf(out, out_size, "%s/plexmusic/%s", base, FAVORITES_FILENAME);
    } else {
        snprintf(out, out_size, "%s/%s", FALLBACK_BASE_DIR, FAVORITES_FILENAME);
    }
}

/* Ensure the directory portion of a path exists (one level only). */
static void ensure_dir(const char *file_path)
{
    char dir[640];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    /* Create parent directory first (best-effort) */
    char parent[640];
    strncpy(parent, dir, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *pslash = strrchr(parent, '/');
    if (pslash) {
        *pslash = '\0';
        mkdir(parent, 0755);
    }

    mkdir(dir, 0755);
}

static void favorites_save(void)
{
    char path[640];
    favorites_path(path, sizeof(path));
    ensure_dir(path);

    JSON_Value  *root     = json_value_init_object();
    JSON_Object *root_obj = json_value_get_object(root);
    JSON_Value  *tracks_v = json_value_init_array();
    JSON_Array  *tracks_a = json_value_get_array(tracks_v);

    for (int i = 0; i < g_fav_count; i++) {
        const PlexTrack *t = &g_favs[i];

        JSON_Value  *trk_v   = json_value_init_object();
        JSON_Object *trk_obj = json_value_get_object(trk_v);

        json_object_set_number(trk_obj, "track_id",     (double)t->rating_key);
        json_object_set_number(trk_obj, "track_number", (double)t->track_number);
        json_object_set_number(trk_obj, "duration_ms",  (double)t->duration_ms);
        json_object_set_string(trk_obj, "title",        t->title);
        json_object_set_string(trk_obj, "artist",       t->artist);
        json_object_set_string(trk_obj, "album",        t->album);
        json_object_set_string(trk_obj, "media_key",    t->media_key);
        json_object_set_string(trk_obj, "thumb",        t->thumb);

        json_array_append_value(tracks_a, trk_v);
    }

    json_object_set_value(root_obj, "tracks", tracks_v);

    char *json_str = json_serialize_to_string_pretty(root);
    json_value_free(root);
    if (!json_str) {
        PLEX_LOG_ERROR("[Favorites] Failed to serialise favorites\n");
        return;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        PLEX_LOG_ERROR("[Favorites] Cannot open favorites for write: %s\n", path);
        json_free_serialized_string(json_str);
        return;
    }

    size_t  len     = strlen(json_str);
    ssize_t written = write(fd, json_str, len);
    int     serr    = fsync(fd);
    close(fd);
    json_free_serialized_string(json_str);

    if (written != (ssize_t)len || serr != 0)
        PLEX_LOG_ERROR("[Favorites] Write/fsync error writing favorites\n");
}

static void favorites_load(void)
{
    char path[640];
    favorites_path(path, sizeof(path));

    JSON_Value *root = json_parse_file(path);
    if (!root) {
        /* Missing file is not an error — start empty */
        PLEX_LOG("[Favorites] No favorites file found, starting empty\n");
        return;
    }

    JSON_Object *root_obj = json_value_get_object(root);
    if (!root_obj) {
        json_value_free(root);
        return;
    }

    JSON_Array *tracks_a = json_object_get_array(root_obj, "tracks");
    if (!tracks_a) {
        json_value_free(root);
        return;
    }

    int count = (int)json_array_get_count(tracks_a);
    if (count > PLEX_MAX_FAVORITES)
        count = PLEX_MAX_FAVORITES;

    g_fav_count = 0;
    for (int i = 0; i < count; i++) {
        JSON_Object *trk_obj = json_array_get_object(tracks_a, i);
        if (!trk_obj) continue;

        PlexTrack *t = &g_favs[g_fav_count];
        memset(t, 0, sizeof(*t));

        t->rating_key   = (int)json_object_get_number(trk_obj, "track_id");
        t->track_number = (int)json_object_get_number(trk_obj, "track_number");
        t->duration_ms  = (int)json_object_get_number(trk_obj, "duration_ms");

        const char *title     = json_object_get_string(trk_obj, "title");
        const char *artist    = json_object_get_string(trk_obj, "artist");
        const char *album     = json_object_get_string(trk_obj, "album");
        const char *media_key = json_object_get_string(trk_obj, "media_key");
        const char *thumb     = json_object_get_string(trk_obj, "thumb");

        if (title)     strncpy(t->title,     title,     sizeof(t->title) - 1);
        if (artist)    strncpy(t->artist,    artist,    sizeof(t->artist) - 1);
        if (album)     strncpy(t->album,     album,     sizeof(t->album) - 1);
        if (media_key) strncpy(t->media_key, media_key, sizeof(t->media_key) - 1);
        if (thumb)     strncpy(t->thumb,     thumb,     sizeof(t->thumb) - 1);
        /* local_path is runtime-only — leave as empty string (memset above covers it) */

        g_fav_count++;
    }

    json_value_free(root);
    PLEX_LOG("[Favorites] Loaded %d favorites\n", g_fav_count);
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

void plex_favorites_init(void)
{
    g_fav_count = 0;
    favorites_load();
}

void plex_favorites_quit(void)
{
    /* No-op: static array, nothing to free */
}

bool plex_favorites_toggle(const PlexTrack *t)
{
    if (!t) return false;

    /* Linear scan: if found, remove by shifting left and save */
    for (int i = 0; i < g_fav_count; i++) {
        if (g_favs[i].rating_key == t->rating_key) {
            if (i < g_fav_count - 1)
                memmove(&g_favs[i], &g_favs[i + 1],
                        (g_fav_count - i - 1) * sizeof(PlexTrack));
            g_fav_count--;
            favorites_save();
            return false;
        }
    }

    /* Not found: append if room */
    if (g_fav_count >= PLEX_MAX_FAVORITES) {
        PLEX_LOG_ERROR("[Favorites] Favorites list full (%d), cannot add\n",
                       PLEX_MAX_FAVORITES);
        return false;
    }

    g_favs[g_fav_count] = *t;
    /* Clear runtime-only field */
    g_favs[g_fav_count].local_path[0] = '\0';
    g_fav_count++;
    favorites_save();
    return true;
}

bool plex_favorites_contains(int rating_key)
{
    for (int i = 0; i < g_fav_count; i++) {
        if (g_favs[i].rating_key == rating_key)
            return true;
    }
    return false;
}

int plex_favorites_get(PlexTrack *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = g_fav_count < max ? g_fav_count : max;
    memcpy(out, g_favs, n * sizeof(PlexTrack));
    return n;
}

int plex_favorites_count(void)
{
    return g_fav_count;
}
