#define _GNU_SOURCE
#include "plex_downloads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <time.h>

#include "include/parson/parson.h"
#include "plex_api.h"
#include "plex_net.h"
#include "plex_log.h"

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

#define MANIFEST_FILENAME    "downloads.json"
#define FALLBACK_BASE_DIR    "/mnt/SDCARD/.userdata/shared/plexmusic"
#define DL_QUEUE_MAX         8
#define MANIFEST_ALBUMS_MAX  64
#define MANIFEST_TRACKS_MAX  64   /* max tracks per album */

/* URL-encoded X-Plex-Client-Profile-Extra value requesting OGG/Opus output */
#define OPUS_PROFILE_EXTRA \
    "add-transcode-target(replace%3Dtrue%26type%3DmusicProfile%26context%3Dstreaming" \
    "%26protocol%3Dhttp%26container%3Dogg%26audioCodec%3Dopus)" \
    "%2Badd-limitation(scope%3DmusicCodec%26scopeName%3Dopus%26type%3DupperBound" \
    "%26name%3Daudio.channels%26value%3D2%26onlyTranscodes%3Dtrue%26replace%3Dtrue)"

/* ------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------ */

/* One track as stored in the manifest (no media_key — not needed after download). */
typedef struct {
    int  track_id;
    int  track_number;
    int  duration_ms;
    char title[PLEX_MAX_STR];
    char artist[PLEX_MAX_STR];
    char album[PLEX_MAX_STR];
    char thumb[PLEX_MAX_URL];
    char local_path[768];
} ManifestTrack;

/* One album as stored in the manifest. */
typedef struct {
    int           album_id;
    int           artist_id;
    char          album_title[PLEX_MAX_STR];
    char          artist_name[PLEX_MAX_STR];
    char          thumb[PLEX_MAX_URL];
    char          year[8];
    ManifestTrack tracks[MANIFEST_TRACKS_MAX];
    int           track_count;
} ManifestAlbum;

/* One entry in the pending download queue. */
typedef struct {
    char server_url[PLEX_MAX_URL];
    char token[PLEX_MAX_STR];
    int  album_rating_key;
    char album_id_str[32];
    char album_title[PLEX_MAX_STR];
    int  artist_id;
    char artist_name[PLEX_MAX_STR];
    char thumb[PLEX_MAX_URL];
    char year[8];
} QueueEntry;

/* ------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------ */

static pthread_mutex_t  g_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cond    = PTHREAD_COND_INITIALIZER;
static pthread_t        g_worker;
static volatile bool    g_stop    = false;
static bool             g_started = false;

/* Manifest: downloaded albums */
static ManifestAlbum g_albums[MANIFEST_ALBUMS_MAX];
static int           g_album_count = 0;

/* Download queue (ring buffer, FIFO) */
static QueueEntry g_queue[DL_QUEUE_MAX];
static int        g_queue_head  = 0;
static int        g_queue_tail  = 0;
static int        g_queue_count = 0;

/* Currently downloading album_id (-1 = none) */
static int g_active_album_id  = -1;
static int g_active_completed = 0;
static int g_active_total     = 0;

/* ------------------------------------------------------------------
 * Path helpers
 * ------------------------------------------------------------------ */

static void manifest_path(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base)
        snprintf(out, out_size, "%s/plexmusic/%s", base, MANIFEST_FILENAME);
    else
        snprintf(out, out_size, "%s/%s", FALLBACK_BASE_DIR, MANIFEST_FILENAME);
}

static void downloads_base_dir(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base)
        snprintf(out, out_size, "%s/plexmusic/downloads", base);
    else
        snprintf(out, out_size, "%s/downloads", FALLBACK_BASE_DIR);
}

static void track_local_path(int album_id, int track_id, const char *ext,
                              char *out, int out_size)
{
    char base[512];
    downloads_base_dir(base, sizeof(base));
    snprintf(out, out_size, "%s/%d/%d.%s", base, album_id, track_id, ext);
}

/* ------------------------------------------------------------------
 * Directory creation (adapted from plex_config.c ensure_config_dir)
 * ------------------------------------------------------------------ */

static void ensure_parent_dirs(const char *file_path)
{
    char dir[640];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    /* Create grandparent directory first (best-effort) */
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

/* ------------------------------------------------------------------
 * Extension extraction (mirrors module_player.c extract_ext)
 * ------------------------------------------------------------------ */

static void extract_ext(const char *media_key, char *out, int out_size)
{
    if (!media_key || media_key[0] == '\0') {
        snprintf(out, out_size, "mp3");
        return;
    }
    const char *q   = strchr(media_key, '?');
    const char *dot = NULL;
    const char *p   = media_key;
    while (*p && (!q || p < q)) {
        if (*p == '.') dot = p;
        p++;
    }
    if (!dot || *(dot + 1) == '\0') {
        snprintf(out, out_size, "mp3");
        return;
    }
    snprintf(out, out_size, "%s", dot + 1);
    char *qm = strchr(out, '?');
    if (qm) *qm = '\0';
}

/* ------------------------------------------------------------------
 * Opus transcode helpers
 * ------------------------------------------------------------------ */

static void generate_session_id(int track_rating_key, char *out, int out_size)
{
    snprintf(out, out_size, "pm-%d-%ld", track_rating_key, (long)time(NULL));
}

/*
 * Download one track via /audio/:/transcode/universal as OGG/Opus.
 * Returns 0 on success, -1 on failure.
 */
static int opus_download_track(const char *server_url,
                                const char *token,
                                int         track_rating_key,
                                int         bitrate_kbps,
                                const char *local_path)
{
    char session_id[64];
    generate_session_id(track_rating_key, session_id, sizeof(session_id));
    PLEX_LOG("[Downloads] Opus session_id=%s track=%d bitrate=%d\n",
             session_id, track_rating_key, bitrate_kbps);

    /* --- Step 1: /decision --- */
    char url[2048];
    snprintf(url, sizeof(url),
             "%s/audio/:/transcode/universal/decision"
             "?path=%%2Flibrary%%2Fmetadata%%2F%d"
             "&musicBitrate=%d"
             "&session=%s"
             "&directPlay=0"
             "&X-Plex-Client-Identifier=plexmusic-nextui"
             "&X-Plex-Session-Identifier=%s"
             "&X-Plex-Chunked=1"
             "&X-Plex-Client-Profile-Extra=%s"
             "&X-Plex-Token=%s",
             server_url, track_rating_key, bitrate_kbps,
             session_id, session_id, OPUS_PROFILE_EXTRA, token);

    static uint8_t decision_buf[16 * 1024];
    PlexNetOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.method      = PLEX_HTTP_GET;
    opts.token       = NULL;
    opts.timeout_sec = 30;

    int dec_ret = plex_net_fetch(url, decision_buf, sizeof(decision_buf), &opts);
    if (dec_ret < 0) {
        PLEX_LOG_ERROR("[Downloads] /decision failed for track %d\n", track_rating_key);
        /* Fall through — attempt /start anyway; /stop always runs */
    } else {
        decision_buf[dec_ret < (int)sizeof(decision_buf) ? dec_ret : (int)sizeof(decision_buf) - 1] = '\0';
        PLEX_LOG("[Downloads] /decision response (%d bytes): %.256s\n", dec_ret, (char *)decision_buf);
    }

    /* --- Step 2: /start (stream to file) --- */
    snprintf(url, sizeof(url),
             "%s/audio/:/transcode/universal/start"
             "?path=%%2Flibrary%%2Fmetadata%%2F%d"
             "&musicBitrate=%d"
             "&session=%s"
             "&directPlay=0"
             "&X-Plex-Client-Identifier=plexmusic-nextui"
             "&X-Plex-Session-Identifier=%s"
             "&X-Plex-Chunked=1"
             "&X-Plex-Client-Profile-Extra=%s"
             "&X-Plex-Token=%s",
             server_url, track_rating_key, bitrate_kbps,
             session_id, session_id, OPUS_PROFILE_EXTRA, token);

    memset(&opts, 0, sizeof(opts));
    opts.method      = PLEX_HTTP_GET;
    opts.token       = NULL;
    opts.timeout_sec = 300;

    int start_ret = plex_net_download_file(url, local_path, NULL, NULL, &opts);
    if (start_ret >= 0)
        PLEX_LOG("[Downloads] /start downloaded %d bytes -> %s\n", start_ret, local_path);
    else
        PLEX_LOG_ERROR("[Downloads] /start failed for track %d (%s)\n",
                       track_rating_key, local_path);

    /* --- Step 3: /stop (always, for cleanup) --- */
    snprintf(url, sizeof(url),
             "%s/audio/:/transcode/universal/stop"
             "?closeResourceSession=1"
             "&path=%%2Flibrary%%2Fmetadata%%2F%d"
             "&musicBitrate=%d"
             "&session=%s"
             "&directPlay=0"
             "&X-Plex-Client-Identifier=plexmusic-nextui"
             "&X-Plex-Session-Identifier=%s"
             "&X-Plex-Chunked=1"
             "&X-Plex-Client-Profile-Extra=%s"
             "&X-Plex-Token=%s",
             server_url, track_rating_key, bitrate_kbps,
             session_id, session_id, OPUS_PROFILE_EXTRA, token);

    static uint8_t stop_buf[1024];
    memset(&opts, 0, sizeof(opts));
    opts.method      = PLEX_HTTP_GET;
    opts.token       = NULL;
    opts.timeout_sec = 15;

    plex_net_fetch(url, stop_buf, sizeof(stop_buf), &opts); /* fire-and-forget */
    PLEX_LOG("[Downloads] /stop called for track %d\n", track_rating_key);

    return (start_ret >= 0) ? 0 : -1;
}

/* ------------------------------------------------------------------
 * Manifest persistence — caller must hold g_mutex
 * ------------------------------------------------------------------ */

static void manifest_save_locked(void)
{
    char path[640];
    manifest_path(path, sizeof(path));
    ensure_parent_dirs(path);

    JSON_Value  *root     = json_value_init_object();
    JSON_Object *root_obj = json_value_get_object(root);
    JSON_Value  *albums_v = json_value_init_array();
    JSON_Array  *albums_a = json_value_get_array(albums_v);

    for (int i = 0; i < g_album_count; i++) {
        const ManifestAlbum *ma = &g_albums[i];

        JSON_Value  *alb_v   = json_value_init_object();
        JSON_Object *alb_obj = json_value_get_object(alb_v);

        json_object_set_number(alb_obj, "album_id",    (double)ma->album_id);
        json_object_set_string(alb_obj, "album_title", ma->album_title);
        json_object_set_number(alb_obj, "artist_id",   (double)ma->artist_id);
        json_object_set_string(alb_obj, "artist_name", ma->artist_name);
        json_object_set_string(alb_obj, "thumb",       ma->thumb);
        json_object_set_string(alb_obj, "year",        ma->year);

        JSON_Value *tracks_v = json_value_init_array();
        JSON_Array *tracks_a = json_value_get_array(tracks_v);

        for (int j = 0; j < ma->track_count; j++) {
            const ManifestTrack *mt = &ma->tracks[j];

            JSON_Value  *trk_v   = json_value_init_object();
            JSON_Object *trk_obj = json_value_get_object(trk_v);

            json_object_set_number(trk_obj, "track_id",     (double)mt->track_id);
            json_object_set_string(trk_obj, "title",        mt->title);
            json_object_set_string(trk_obj, "artist",       mt->artist);
            json_object_set_string(trk_obj, "album",        mt->album);
            json_object_set_number(trk_obj, "track_number", (double)mt->track_number);
            json_object_set_number(trk_obj, "duration_ms",  (double)mt->duration_ms);
            json_object_set_string(trk_obj, "thumb",        mt->thumb);
            json_object_set_string(trk_obj, "local_path",   mt->local_path);

            json_array_append_value(tracks_a, trk_v);
        }

        json_object_set_value(alb_obj, "tracks", tracks_v);
        json_array_append_value(albums_a, alb_v);
    }

    json_object_set_value(root_obj, "albums", albums_v);

    char *json_str = json_serialize_to_string_pretty(root);
    json_value_free(root);
    if (!json_str) {
        PLEX_LOG_ERROR("[Downloads] Failed to serialise manifest\n");
        return;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        PLEX_LOG_ERROR("[Downloads] Cannot open manifest for write: %s\n", path);
        json_free_serialized_string(json_str);
        return;
    }

    size_t  len     = strlen(json_str);
    ssize_t written = write(fd, json_str, len);
    int     serr    = fsync(fd);
    close(fd);
    json_free_serialized_string(json_str);

    if (written != (ssize_t)len || serr != 0)
        PLEX_LOG_ERROR("[Downloads] Write/fsync error writing manifest\n");
}

/* ------------------------------------------------------------------
 * Manifest load (called once at init, no lock needed yet)
 * ------------------------------------------------------------------ */

static bool album_fully_downloaded(const ManifestAlbum *ma)
{
    if (ma->track_count == 0) return false;
    for (int i = 0; i < ma->track_count; i++) {
        if (access(ma->tracks[i].local_path, F_OK) != 0)
            return false;
    }
    return true;
}

static void manifest_load(void)
{
    char path[640];
    manifest_path(path, sizeof(path));

    JSON_Value *root = json_parse_file(path);
    if (!root) {
        PLEX_LOG("[Downloads] No manifest at %s; starting empty\n", path);
        return;
    }

    JSON_Object *root_obj = json_value_get_object(root);
    if (!root_obj) {
        json_value_free(root);
        return;
    }

    JSON_Array *albums_a = json_object_get_array(root_obj, "albums");
    if (!albums_a) {
        json_value_free(root);
        return;
    }

    size_t album_cnt = json_array_get_count(albums_a);
    for (size_t i = 0; i < album_cnt && g_album_count < MANIFEST_ALBUMS_MAX; i++) {
        JSON_Object *alb_obj = json_array_get_object(albums_a, i);
        if (!alb_obj) continue;

        ManifestAlbum ma;
        memset(&ma, 0, sizeof(ma));

        ma.album_id  = (int)json_object_get_number(alb_obj, "album_id");
        ma.artist_id = (int)json_object_get_number(alb_obj, "artist_id");

        const char *s;
        if ((s = json_object_get_string(alb_obj, "album_title")))
            strncpy(ma.album_title, s, sizeof(ma.album_title) - 1);
        if ((s = json_object_get_string(alb_obj, "artist_name")))
            strncpy(ma.artist_name, s, sizeof(ma.artist_name) - 1);
        if ((s = json_object_get_string(alb_obj, "thumb")))
            strncpy(ma.thumb, s, sizeof(ma.thumb) - 1);
        /* year: optional — old manifests without it load as empty string */
        s = json_object_get_string(alb_obj, "year");
        if (s) strncpy(ma.year, s, sizeof(ma.year) - 1);

        JSON_Array *tracks_a = json_object_get_array(alb_obj, "tracks");
        if (tracks_a) {
            size_t trk_cnt = json_array_get_count(tracks_a);
            for (size_t j = 0; j < trk_cnt && ma.track_count < MANIFEST_TRACKS_MAX; j++) {
                JSON_Object *trk_obj = json_array_get_object(tracks_a, j);
                if (!trk_obj) continue;

                ManifestTrack mt;
                memset(&mt, 0, sizeof(mt));

                mt.track_id     = (int)json_object_get_number(trk_obj, "track_id");
                mt.track_number = (int)json_object_get_number(trk_obj, "track_number");
                mt.duration_ms  = (int)json_object_get_number(trk_obj, "duration_ms");

                if ((s = json_object_get_string(trk_obj, "title")))
                    strncpy(mt.title, s, sizeof(mt.title) - 1);
                if ((s = json_object_get_string(trk_obj, "artist")))
                    strncpy(mt.artist, s, sizeof(mt.artist) - 1);
                if ((s = json_object_get_string(trk_obj, "album")))
                    strncpy(mt.album, s, sizeof(mt.album) - 1);
                if ((s = json_object_get_string(trk_obj, "thumb")))
                    strncpy(mt.thumb, s, sizeof(mt.thumb) - 1);
                if ((s = json_object_get_string(trk_obj, "local_path")))
                    strncpy(mt.local_path, s, sizeof(mt.local_path) - 1);

                ma.tracks[ma.track_count++] = mt;
            }
        }

        /* Discard albums with any missing files (partial previous download) */
        if (!album_fully_downloaded(&ma)) {
            PLEX_LOG("[Downloads] Discarding partial album %d (%s)\n",
                     ma.album_id, ma.album_title);
            continue;
        }

        g_albums[g_album_count++] = ma;
    }

    json_value_free(root);
    PLEX_LOG("[Downloads] Manifest loaded: %d complete album(s)\n", g_album_count);
}

/* ------------------------------------------------------------------
 * Background worker thread
 * ------------------------------------------------------------------ */

static void *download_worker(void *arg)
{
    (void)arg;

    while (1) {
        pthread_mutex_lock(&g_mutex);
        while (g_queue_count == 0 && !g_stop)
            pthread_cond_wait(&g_cond, &g_mutex);

        if (g_stop) {
            pthread_mutex_unlock(&g_mutex);
            break;
        }

        /* Dequeue FIFO */
        QueueEntry entry = g_queue[g_queue_head];
        g_queue_head  = (g_queue_head + 1) % DL_QUEUE_MAX;
        g_queue_count--;
        g_active_album_id = entry.album_rating_key;

        pthread_mutex_unlock(&g_mutex);

        /* Build a minimal PlexConfig for API calls */
        PlexConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        strncpy(cfg.server_url, entry.server_url, sizeof(cfg.server_url) - 1);
        strncpy(cfg.token,      entry.token,      sizeof(cfg.token) - 1);

        /* Fetch track listing */
        PlexTrack tracks[PLEX_MAX_ITEMS];
        int       track_count = 0;
        if (plex_api_get_tracks(&cfg, entry.album_rating_key,
                                tracks, &track_count) != 0) {
            PLEX_LOG_ERROR("[Downloads] plex_api_get_tracks failed for album %d\n",
                           entry.album_rating_key);
            pthread_mutex_lock(&g_mutex);
            if (g_active_album_id == entry.album_rating_key)
                g_active_album_id = -1;
            pthread_mutex_unlock(&g_mutex);
            continue;
        }

        PLEX_LOG("[Downloads] Album %d: %d tracks to download\n",
                 entry.album_rating_key, track_count);

        /* Build manifest album in-progress; appended to g_albums track by track */
        ManifestAlbum ma;
        memset(&ma, 0, sizeof(ma));
        ma.album_id  = entry.album_rating_key;
        ma.artist_id = entry.artist_id;
        strncpy(ma.album_title, entry.album_title, sizeof(ma.album_title) - 1);
        strncpy(ma.artist_name, entry.artist_name, sizeof(ma.artist_name) - 1);
        strncpy(ma.thumb,       entry.thumb,       sizeof(ma.thumb) - 1);
        strncpy(ma.year,        entry.year,        sizeof(ma.year) - 1);

        int limit = track_count < MANIFEST_TRACKS_MAX ? track_count : MANIFEST_TRACKS_MAX;

        pthread_mutex_lock(&g_mutex);
        g_active_completed = 0;
        g_active_total     = limit;
        pthread_mutex_unlock(&g_mutex);

        /* Read download bitrate once per album (int read is atomic enough; value only
         * changes when the user saves settings on the main thread). */
        int dl_bitrate = plex_config_get_mutable()->download_bitrate_kbps;

        for (int i = 0; i < limit; i++) {
            /* Check stop flag between tracks */
            pthread_mutex_lock(&g_mutex);
            bool should_stop = g_stop;
            pthread_mutex_unlock(&g_mutex);
            if (should_stop) break;

            const PlexTrack *t = &tracks[i];

            char ext[16];
            if (dl_bitrate > 0) {
                strncpy(ext, "opus", sizeof(ext) - 1);
                ext[sizeof(ext) - 1] = '\0';
            } else {
                extract_ext(t->media_key, ext, sizeof(ext));
            }

            char local_path[768];
            track_local_path(entry.album_rating_key, t->rating_key,
                             ext, local_path, sizeof(local_path));

            PLEX_LOG("[Downloads] [%d/%d] %s -> %s\n",
                     i + 1, limit, t->title, local_path);

            if (dl_bitrate > 0) {
                ensure_parent_dirs(local_path);
                int ret = opus_download_track(entry.server_url, entry.token,
                                              t->rating_key, dl_bitrate, local_path);
                if (ret < 0) {
                    PLEX_LOG_ERROR("[Downloads] Opus download failed for track %d (%s), skipping\n",
                                   t->rating_key, t->title);
                    continue;
                }
            } else {
                /* existing direct-stream path — unchanged */
                char stream_url[PLEX_MAX_URL];
                plex_api_get_stream_url(&cfg, t, stream_url, sizeof(stream_url));
                ensure_parent_dirs(local_path);
                PlexNetOptions opts;
                memset(&opts, 0, sizeof(opts));
                opts.method      = PLEX_HTTP_GET;
                opts.token       = NULL;
                opts.timeout_sec = 120;
                int ret = plex_net_download_file(stream_url, local_path, NULL, NULL, &opts);
                if (ret < 0) {
                    PLEX_LOG_ERROR("[Downloads] Failed to download track %d (%s), skipping\n",
                                   t->rating_key, t->title);
                    continue;
                }
            }

            /* Build manifest track */
            ManifestTrack mt;
            memset(&mt, 0, sizeof(mt));
            mt.track_id     = t->rating_key;
            mt.track_number = t->track_number;
            mt.duration_ms  = t->duration_ms;
            strncpy(mt.title,      t->title,     sizeof(mt.title) - 1);
            strncpy(mt.artist,     t->artist,    sizeof(mt.artist) - 1);
            strncpy(mt.album,      t->album,     sizeof(mt.album) - 1);
            strncpy(mt.thumb,      t->thumb,     sizeof(mt.thumb) - 1);
            strncpy(mt.local_path, local_path,   sizeof(mt.local_path) - 1);

            pthread_mutex_lock(&g_mutex);
            g_active_completed = ma.track_count + 1;  /* +1: this track is about to be committed */
            pthread_mutex_unlock(&g_mutex);
            ma.tracks[ma.track_count++] = mt;
        }

        pthread_mutex_lock(&g_mutex);
        if (g_active_album_id == entry.album_rating_key)
            g_active_album_id = -1;
        g_active_completed = 0;
        g_active_total     = 0;

        if (ma.track_count > 0) {
            bool found = false;
            for (int k = 0; k < g_album_count; k++) {
                if (g_albums[k].album_id == ma.album_id) {
                    g_albums[k] = ma;
                    found = true;
                    break;
                }
            }
            if (!found && g_album_count < MANIFEST_ALBUMS_MAX)
                g_albums[g_album_count++] = ma;
            manifest_save_locked();
        }
        pthread_mutex_unlock(&g_mutex);

        PLEX_LOG("[Downloads] Album %d done (%d/%d tracks)\n",
                 entry.album_rating_key, ma.track_count, limit);
    }

    return NULL;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

void plex_downloads_init(void)
{
    manifest_load();

    g_stop            = false;
    g_queue_head      = 0;
    g_queue_tail      = 0;
    g_queue_count     = 0;
    g_active_album_id = -1;

    if (pthread_create(&g_worker, NULL, download_worker, NULL) != 0) {
        PLEX_LOG_ERROR("[Downloads] Failed to create worker thread\n");
        return;
    }
    g_started = true;
    PLEX_LOG("[Downloads] Initialised\n");
}

void plex_downloads_quit(void)
{
    if (!g_started) return;

    pthread_mutex_lock(&g_mutex);
    g_stop = true;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);

    pthread_join(g_worker, NULL);
    g_started = false;
    PLEX_LOG("[Downloads] Shutdown complete\n");
}

void plex_downloads_queue_album(const PlexConfig *cfg,
                                int album_rating_key,
                                const char *album_title,
                                int artist_id,
                                const char *artist_name,
                                const char *thumb,
                                const char *year)
{
    if (!cfg) return;

    pthread_mutex_lock(&g_mutex);

    /* No-op if fully downloaded */
    for (int i = 0; i < g_album_count; i++) {
        if (g_albums[i].album_id == album_rating_key) {
            pthread_mutex_unlock(&g_mutex);
            return;
        }
    }

    /* No-op if currently downloading */
    if (g_active_album_id == album_rating_key) {
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    /* No-op if already in queue */
    for (int i = 0; i < g_queue_count; i++) {
        int idx = (g_queue_head + i) % DL_QUEUE_MAX;
        if (g_queue[idx].album_rating_key == album_rating_key) {
            pthread_mutex_unlock(&g_mutex);
            return;
        }
    }

    /* No-op if queue full */
    if (g_queue_count >= DL_QUEUE_MAX) {
        PLEX_LOG("[Downloads] Queue full, dropping album %d\n", album_rating_key);
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    QueueEntry *e = &g_queue[g_queue_tail];
    memset(e, 0, sizeof(*e));
    strncpy(e->server_url, cfg->server_url, sizeof(e->server_url) - 1);
    strncpy(e->token,      cfg->token,      sizeof(e->token) - 1);
    e->album_rating_key = album_rating_key;
    snprintf(e->album_id_str, sizeof(e->album_id_str), "%d", album_rating_key);
    if (album_title)  strncpy(e->album_title,  album_title,  sizeof(e->album_title) - 1);
    e->artist_id = artist_id;
    if (artist_name)  strncpy(e->artist_name,  artist_name,  sizeof(e->artist_name) - 1);
    if (thumb)        strncpy(e->thumb,         thumb,        sizeof(e->thumb) - 1);
    if (year)         strncpy(e->year,          year,         sizeof(e->year) - 1);

    g_queue_tail  = (g_queue_tail + 1) % DL_QUEUE_MAX;
    g_queue_count++;

    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);

    PLEX_LOG("[Downloads] Queued album %d (%s)\n", album_rating_key,
             album_title ? album_title : "");
}

DlStatus plex_downloads_album_status(int album_rating_key)
{
    pthread_mutex_lock(&g_mutex);

    /* Check manifest first (DONE) */
    for (int i = 0; i < g_album_count; i++) {
        if (g_albums[i].album_id == album_rating_key) {
            pthread_mutex_unlock(&g_mutex);
            return DL_STATUS_DONE;
        }
    }

    /* Check currently downloading */
    if (g_active_album_id == album_rating_key) {
        pthread_mutex_unlock(&g_mutex);
        return DL_STATUS_DOWNLOADING;
    }

    /* Check queue */
    for (int i = 0; i < g_queue_count; i++) {
        int idx = (g_queue_head + i) % DL_QUEUE_MAX;
        if (g_queue[idx].album_rating_key == album_rating_key) {
            pthread_mutex_unlock(&g_mutex);
            return DL_STATUS_QUEUED;
        }
    }

    pthread_mutex_unlock(&g_mutex);
    return DL_STATUS_NONE;
}

bool plex_downloads_is_active(void)
{
    pthread_mutex_lock(&g_mutex);
    bool active = (g_active_album_id != -1);
    pthread_mutex_unlock(&g_mutex);
    return active;
}

bool plex_downloads_album_progress(int album_id, int *completed, int *total)
{
    pthread_mutex_lock(&g_mutex);
    bool active = (g_active_album_id == album_id);
    if (active) {
        *completed = g_active_completed;
        *total     = g_active_total;
    }
    pthread_mutex_unlock(&g_mutex);
    return active;
}

/* ------------------------------------------------------------------
 * Offline browse query functions
 * ------------------------------------------------------------------ */

int plex_downloads_get_artists(PlexArtist *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    pthread_mutex_lock(&g_mutex);

    /* Collect unique (artist_id, artist_name) pairs */
    int  artist_ids[MANIFEST_ALBUMS_MAX];
    char artist_names[MANIFEST_ALBUMS_MAX][PLEX_MAX_STR];
    int  unique_count = 0;

    for (int i = 0; i < g_album_count; i++) {
        bool found = false;
        for (int j = 0; j < unique_count; j++) {
            if (artist_ids[j] == g_albums[i].artist_id) {
                found = true;
                break;
            }
        }
        if (!found && unique_count < MANIFEST_ALBUMS_MAX) {
            artist_ids[unique_count] = g_albums[i].artist_id;
            strncpy(artist_names[unique_count], g_albums[i].artist_name,
                    PLEX_MAX_STR - 1);
            artist_names[unique_count][PLEX_MAX_STR - 1] = '\0';
            unique_count++;
        }
    }

    /* Insertion sort by name (list is small) */
    for (int i = 1; i < unique_count; i++) {
        int  tmp_id = artist_ids[i];
        char tmp_name[PLEX_MAX_STR];
        strncpy(tmp_name, artist_names[i], sizeof(tmp_name) - 1);
        tmp_name[sizeof(tmp_name) - 1] = '\0';

        int j = i - 1;
        while (j >= 0 && strcasecmp(artist_names[j], tmp_name) > 0) {
            artist_ids[j + 1] = artist_ids[j];
            strncpy(artist_names[j + 1], artist_names[j],
                    sizeof(artist_names[j + 1]) - 1);
            artist_names[j + 1][sizeof(artist_names[j + 1]) - 1] = '\0';
            j--;
        }
        artist_ids[j + 1] = tmp_id;
        strncpy(artist_names[j + 1], tmp_name, sizeof(artist_names[j + 1]) - 1);
        artist_names[j + 1][sizeof(artist_names[j + 1]) - 1] = '\0';
    }

    int n = unique_count < out_max ? unique_count : out_max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].rating_key = artist_ids[i];
        strncpy(out[i].title, artist_names[i], sizeof(out[i].title) - 1);
    }

    pthread_mutex_unlock(&g_mutex);
    return n;
}

int plex_downloads_get_albums_for_artist(int artist_id,
                                         PlexAlbum *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    pthread_mutex_lock(&g_mutex);

    int n = 0;
    for (int i = 0; i < g_album_count && n < out_max; i++) {
        if (g_albums[i].artist_id != artist_id) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].rating_key = g_albums[i].album_id;
        strncpy(out[n].title,  g_albums[i].album_title, sizeof(out[n].title) - 1);
        strncpy(out[n].artist, g_albums[i].artist_name, sizeof(out[n].artist) - 1);
        strncpy(out[n].thumb,  g_albums[i].thumb,       sizeof(out[n].thumb) - 1);
        strncpy(out[n].year,   g_albums[i].year,        sizeof(out[n].year) - 1);
        n++;
    }

    pthread_mutex_unlock(&g_mutex);
    return n;
}

static int cmp_album_year_desc(const void *a, const void *b)
{
    const PlexAlbum *pa = (const PlexAlbum *)a;
    const PlexAlbum *pb = (const PlexAlbum *)b;
    int ya = pa->year[0] ? atoi(pa->year) : 0;
    int yb = pb->year[0] ? atoi(pb->year) : 0;
    /* Descending: higher year first; albums with year 0 sort to end */
    return yb - ya;
}

int plex_downloads_get_all_albums(PlexAlbum *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    pthread_mutex_lock(&g_mutex);

    int n = g_album_count < out_max ? g_album_count : out_max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].rating_key = g_albums[i].album_id;
        strncpy(out[i].title,  g_albums[i].album_title, sizeof(out[i].title) - 1);
        strncpy(out[i].artist, g_albums[i].artist_name, sizeof(out[i].artist) - 1);
        strncpy(out[i].thumb,  g_albums[i].thumb,       sizeof(out[i].thumb) - 1);
        strncpy(out[i].year,   g_albums[i].year,        sizeof(out[i].year) - 1);
    }

    pthread_mutex_unlock(&g_mutex);

    qsort(out, (size_t)n, sizeof(PlexAlbum), cmp_album_year_desc);
    return n;
}

int plex_downloads_get_tracks_for_album(int album_id,
                                        PlexTrack *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    pthread_mutex_lock(&g_mutex);

    int n = 0;
    for (int i = 0; i < g_album_count; i++) {
        if (g_albums[i].album_id != album_id) continue;

        const ManifestAlbum *ma = &g_albums[i];
        int limit = ma->track_count < out_max ? ma->track_count : out_max;

        for (int j = 0; j < limit; j++) {
            const ManifestTrack *mt = &ma->tracks[j];
            memset(&out[n], 0, sizeof(out[n]));
            out[n].rating_key   = mt->track_id;
            out[n].track_number = mt->track_number;
            out[n].duration_ms  = mt->duration_ms;
            strncpy(out[n].title,  mt->title,  sizeof(out[n].title) - 1);
            strncpy(out[n].artist, mt->artist, sizeof(out[n].artist) - 1);
            strncpy(out[n].album,  mt->album,  sizeof(out[n].album) - 1);
            strncpy(out[n].thumb,      mt->thumb,      sizeof(out[n].thumb) - 1);
            strncpy(out[n].local_path, mt->local_path, sizeof(out[n].local_path) - 1);
            n++;
        }
        break;  /* album_id is unique in manifest */
    }

    pthread_mutex_unlock(&g_mutex);
    return n;
}
