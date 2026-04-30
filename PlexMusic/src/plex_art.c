#define _GNU_SOURCE
#include "plex_art.h"
#include "plex_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "api.h"
#include "plex_log.h"

/* ------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------ */

typedef struct {
    /* Current ready surface (main-thread-owned) */
    SDL_Surface *art_surface;

    /* Pending surface written by worker, consumed by main thread */
    SDL_Surface *pending_surface;

    /* Worker thread */
    pthread_t    fetch_thread;
    bool         thread_active;

    /* Written by worker, checked by main */
    volatile bool result_ready;

    /* Set by main thread to signal worker to abort */
    volatile bool should_stop;

    /* Inputs copied for worker thread */
    char req_server_url[PLEX_MAX_URL];
    char req_token[PLEX_MAX_STR];
    char req_thumb_path[PLEX_MAX_URL];

    bool fetching;

    volatile int         generation;     /* incremented on each new request  */
    volatile int         req_generation; /* generation copied at thread start */
} PlexArtContext;

static PlexArtContext art_ctx;

/* ------------------------------------------------------------------
 * djb2 hash (same family as album_art.c reference)
 * ------------------------------------------------------------------ */

static unsigned int djb2_hash(const char *str)
{
    unsigned int hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

/* ------------------------------------------------------------------
 * Cache helpers
 * ------------------------------------------------------------------ */

static void ensure_cache_dir(void)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (!base || base[0] == '\0') base = "/mnt/SDCARD/.userdata/shared";

    char parent[768];
    snprintf(parent, sizeof(parent), "%s/plexmusic", base);
    mkdir(parent, 0755);

    char cache_dir[768];
    snprintf(cache_dir, sizeof(cache_dir), "%s/plexmusic/art", base);
    mkdir(cache_dir, 0755);
}

static void get_cache_path(const char *thumb_path, char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (!base || base[0] == '\0') base = "/mnt/SDCARD/.userdata/shared";

    char cache_dir[768];
    snprintf(cache_dir, sizeof(cache_dir), "%s/plexmusic/art", base);

    unsigned int hash = djb2_hash(thumb_path);
    snprintf(out, out_size, "%s/%08x.jpg", cache_dir, hash);
}

/* Load image bytes from a file into an SDL_Surface via memory buffer. */
static SDL_Surface *load_surface_from_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 2 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(f); return NULL; }

    if ((long)fread(data, 1, (size_t)size, f) != size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    SDL_RWops *rw = SDL_RWFromConstMem(data, (int)size);
    SDL_Surface *surf = NULL;
    if (rw)
        surf = IMG_Load_RW(rw, 1);  /* freesrc=1: SDL frees rw */

    free(data);
    return surf;
}

static void save_to_cache(const char *path, const uint8_t *data, int size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, (size_t)size, f);
    fclose(f);
}

/* ------------------------------------------------------------------
 * URL encoding
 * ------------------------------------------------------------------ */

static void url_encode(const char *src, char *dst, int dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 4; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        }
    }
    dst[j] = '\0';
}

/* ------------------------------------------------------------------
 * Worker thread
 * ------------------------------------------------------------------ */

static void *fetch_thread_func(void *arg)
{
    /* Generation is passed as the arg so we capture the value set by the
       thread creator, not a potentially-overwritten req_generation field. */
    int my_generation = (int)(intptr_t)arg;

    /* Take a local snapshot of the request parameters */
    char server_url[PLEX_MAX_URL];
    char token[PLEX_MAX_STR];
    char thumb_path[PLEX_MAX_URL];

    strncpy(server_url, art_ctx.req_server_url, sizeof(server_url) - 1);
    server_url[sizeof(server_url) - 1] = '\0';
    strncpy(token, art_ctx.req_token, sizeof(token) - 1);
    token[sizeof(token) - 1] = '\0';
    strncpy(thumb_path, art_ctx.req_thumb_path, sizeof(thumb_path) - 1);
    thumb_path[sizeof(thumb_path) - 1] = '\0';

    ensure_cache_dir();

    /* Check disk cache */
    char cache_path[768];
    get_cache_path(thumb_path, cache_path, sizeof(cache_path));

    if (!art_ctx.should_stop) {
        SDL_Surface *cached = load_surface_from_file(cache_path);
        if (cached) {
            bool still_current = (my_generation == art_ctx.generation) && !art_ctx.should_stop;
            if (still_current) {
                if (art_ctx.pending_surface)
                    SDL_FreeSurface(art_ctx.pending_surface);
                art_ctx.pending_surface = cached;
                cached = NULL;
            }
            if (my_generation == art_ctx.generation)
                art_ctx.result_ready = true;
            if (cached)
                SDL_FreeSurface(cached);  /* discard stale result */
            return NULL;
        }
    }

    if (art_ctx.should_stop) {
        if (my_generation == art_ctx.generation)
            art_ctx.result_ready = true;
        return NULL;
    }

    /* Build fetch URL:
     * {server_url}/photo/:/transcode?url={encoded_thumb}&width=300&height=300
     * Token is sent as X-Plex-Token header via opts.token (set below).
     */
    char encoded_thumb[PLEX_MAX_URL * 3];  /* worst-case 3x expansion */
    url_encode(thumb_path, encoded_thumb, sizeof(encoded_thumb));

    /* Buffer must fit: server_url + path + encoded_thumb + fixed text */
    char fetch_url[PLEX_MAX_URL + PLEX_MAX_URL * 3 + 64];
    snprintf(fetch_url, sizeof(fetch_url),
             "%s/photo/:/transcode?url=%s&width=120&height=120",
             server_url, encoded_thumb);

    if (art_ctx.should_stop) {
        if (my_generation == art_ctx.generation)
            art_ctx.result_ready = true;
        return NULL;
    }

    /* Allocate image buffer (256 KB is ample for a 120x120 JPEG) */
    uint8_t *img_buf = (uint8_t *)malloc(256 * 1024);
    if (!img_buf) {
        if (my_generation == art_ctx.generation)
            art_ctx.result_ready = true;
        return NULL;
    }

    PlexNetOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.method      = PLEX_HTTP_GET;
    opts.token       = token;
    opts.timeout_sec = 15;
    opts.no_persist  = true;

    int bytes = plex_net_fetch(fetch_url, img_buf, 256 * 1024, &opts);

    if (art_ctx.should_stop || bytes <= 0) {
        if (bytes <= 0)
            PLEX_LOG_ERROR("[PlexArt] fetch failed for thumb: %s\n", thumb_path);
        free(img_buf);
        if (my_generation == art_ctx.generation)
            art_ctx.result_ready = true;
        return NULL;
    }

    /* Save to disk cache before loading into SDL */
    save_to_cache(cache_path, img_buf, bytes);

    /* Load into SDL_Surface via memory */
    SDL_Surface *surf = NULL;
    SDL_RWops *rw = SDL_RWFromConstMem(img_buf, bytes);
    if (rw) {
        surf = IMG_Load_RW(rw, 1);  /* freesrc=1 */
        if (!surf)
            PLEX_LOG_ERROR("[PlexArt] IMG_Load_RW failed: %s\n", IMG_GetError());
    }

    free(img_buf);

    /* Only publish result if we're still the current generation */
    bool still_current = (my_generation == art_ctx.generation) && !art_ctx.should_stop;
    if (still_current) {
        if (art_ctx.pending_surface)
            SDL_FreeSurface(art_ctx.pending_surface);
        art_ctx.pending_surface = surf;
        surf = NULL;
    }
    if (my_generation == art_ctx.generation)
        art_ctx.result_ready = true;

    if (surf)
        SDL_FreeSurface(surf);  /* discard stale result */
    return NULL;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

void plex_art_init(void)
{
    memset(&art_ctx, 0, sizeof(PlexArtContext));
}

void plex_art_cleanup(void)
{
    art_ctx.should_stop = true;

    if (art_ctx.thread_active) {
        pthread_join(art_ctx.fetch_thread, NULL);
        art_ctx.thread_active = false;
    }

    if (art_ctx.pending_surface) {
        SDL_FreeSurface(art_ctx.pending_surface);
        art_ctx.pending_surface = NULL;
    }
    if (art_ctx.art_surface) {
        SDL_FreeSurface(art_ctx.art_surface);
        art_ctx.art_surface = NULL;
    }

    art_ctx.fetching      = false;
    art_ctx.result_ready  = false;
    art_ctx.should_stop   = false;
}

void plex_art_clear(void)
{
    art_ctx.should_stop = true;
    art_ctx.generation++;

    if (art_ctx.thread_active) {
        pthread_join(art_ctx.fetch_thread, NULL);
        art_ctx.thread_active = false;
    }

    if (art_ctx.pending_surface) {
        SDL_FreeSurface(art_ctx.pending_surface);
        art_ctx.pending_surface = NULL;
    }
    if (art_ctx.art_surface) {
        SDL_FreeSurface(art_ctx.art_surface);
        art_ctx.art_surface = NULL;
    }

    art_ctx.fetching      = false;
    art_ctx.result_ready  = false;
    art_ctx.should_stop   = false;
}

void plex_art_fetch(const PlexConfig *cfg, const char *thumb_path)
{
    if (!thumb_path || thumb_path[0] == '\0') return;
    if (!cfg) return;

    /* Signal running worker to stop; bump generation so its result is discarded */
    art_ctx.should_stop = true;
    art_ctx.generation++;
    if (art_ctx.thread_active) {
        if (art_ctx.result_ready) {
            /* Thread already done — fast join, no blocking */
            pthread_join(art_ctx.fetch_thread, NULL);
            if (art_ctx.pending_surface) {
                SDL_FreeSurface(art_ctx.pending_surface);
                art_ctx.pending_surface = NULL;
            }
        } else {
            /* Thread still running — detach it; it will discard its result via
               generation check and free any surfaces before exiting */
            pthread_detach(art_ctx.fetch_thread);
        }
        art_ctx.thread_active = false;
    }
    art_ctx.should_stop  = false;
    art_ctx.result_ready = false;
    art_ctx.fetching     = true;

    strncpy(art_ctx.req_server_url, cfg->server_url,
            sizeof(art_ctx.req_server_url) - 1);
    art_ctx.req_server_url[sizeof(art_ctx.req_server_url) - 1] = '\0';

    strncpy(art_ctx.req_token, cfg->token,
            sizeof(art_ctx.req_token) - 1);
    art_ctx.req_token[sizeof(art_ctx.req_token) - 1] = '\0';

    strncpy(art_ctx.req_thumb_path, thumb_path,
            sizeof(art_ctx.req_thumb_path) - 1);
    art_ctx.req_thumb_path[sizeof(art_ctx.req_thumb_path) - 1] = '\0';

    art_ctx.req_generation = art_ctx.generation;

    if (pthread_create(&art_ctx.fetch_thread, NULL, fetch_thread_func,
                       (void *)(intptr_t)art_ctx.generation) == 0) {
        art_ctx.thread_active = true;
    } else {
        art_ctx.fetching = false;
    }
}

SDL_Surface *plex_art_get(void)
{
    if (art_ctx.result_ready && art_ctx.thread_active) {
        pthread_join(art_ctx.fetch_thread, NULL);
        art_ctx.thread_active = false;
        art_ctx.fetching      = false;
        art_ctx.result_ready  = false;
        if (art_ctx.pending_surface) {
            if (art_ctx.art_surface)
                SDL_FreeSurface(art_ctx.art_surface);
            art_ctx.art_surface     = art_ctx.pending_surface;
            art_ctx.pending_surface = NULL;
        }
    }
    return art_ctx.art_surface;
}

bool plex_art_is_fetching(void)
{
    return art_ctx.fetching;
}
