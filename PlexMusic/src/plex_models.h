#ifndef PLEX_MODELS_H
#define PLEX_MODELS_H

#include <stdbool.h>

#define PLEX_MAX_STR         256
#define PLEX_MAX_URL         512
#define PLEX_MAX_ITEMS       50000  /* sanity ceiling for dynamic lists */
#define PLEX_MAX_ARTIST_ALBUMS 500  /* static per-artist album array */
#define PLEX_MAX_TRACKS        500  /* static per-album track array */
#define PLEX_MAX_LIBRARIES      16  /* static library list */
#define PLEX_MAX_OFFLINE_ITEMS 5000 /* offline artist/album array cap */

typedef struct {
    char token[PLEX_MAX_STR];
    char server_url[PLEX_MAX_URL];   /* e.g. http://192.168.1.10:32400 */
    char relay_url[PLEX_MAX_URL];    /* best relay-tier URL; fallback when LAN unreachable */
    char server_name[PLEX_MAX_STR];
    char server_id[PLEX_MAX_STR];    /* machineIdentifier */
    bool offline_mode;               /* true = offline browse; persisted to config.json */
    int  screen_timeout;             /* seconds before screen sleeps; 0 = disabled */
    int  library_id;                 /* section_id of the selected music library; 0 = none */
    char library_name[PLEX_MAX_STR]; /* display name of the selected music library */
    int  stream_bitrate_kbps;        /* 0 = Original; 96/128/192/256/320 = transcode to Opus */
    int  download_bitrate_kbps;      /* same, for offline downloads */
    bool pocket_lock_enabled;        /* true = MENU+SELECT required to wake; false = any button */
    int  preload_count;              /* 0 = disabled; 1–10 = tracks to preload ahead */
} PlexConfig;

typedef struct {
    char name[PLEX_MAX_STR];
    char url[PLEX_MAX_URL];          /* best connection URL */
    char relay_url[PLEX_MAX_URL];    /* best relay-tier URL */
    char id[PLEX_MAX_STR];           /* clientIdentifier */
    bool owned;
} PlexServer;

typedef struct {
    int  section_id;                  /* numeric library key */
    char title[PLEX_MAX_STR];
    char type[32];                    /* "artist" for music libraries */
} PlexLibrary;

typedef struct {
    int  rating_key;
    char title[PLEX_MAX_STR];
    char title_sort[PLEX_MAX_STR];   /* titleSort from Plex; used for L2/R2 group jumps */
    char thumb[PLEX_MAX_URL];         /* relative path for artwork */
} PlexArtist;

typedef struct {
    int  rating_key;
    int  artist_id;                     /* parentRatingKey — artist's rating key */
    char title[PLEX_MAX_STR];
    char artist[PLEX_MAX_STR];
    char year[8];
    char thumb[PLEX_MAX_URL];
} PlexAlbum;

typedef struct {
    int  rating_key;
    int  duration_ms;
    int  track_number;
    char title[PLEX_MAX_STR];
    char artist[PLEX_MAX_STR];
    char album[PLEX_MAX_STR];
    char media_key[PLEX_MAX_URL];    /* streaming path, e.g. /library/parts/123/... */
    char thumb[PLEX_MAX_URL];
    char local_path[768];            /* non-empty = offline: skip download, play this file */
} PlexTrack;

/* Generic paginated result */
typedef struct {
    int total;
    int offset;
    int count;
} PlexPage;

#endif /* PLEX_MODELS_H */
