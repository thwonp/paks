#ifndef PLEX_MODELS_H
#define PLEX_MODELS_H

#include <stdbool.h>

#define PLEX_MAX_STR   256
#define PLEX_MAX_URL   512
#define PLEX_MAX_ITEMS 200   /* max items per page result */

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
    char thumb[PLEX_MAX_URL];         /* relative path for artwork */
} PlexArtist;

typedef struct {
    int  rating_key;
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
