#ifndef PLEX_AUTH_H
#define PLEX_AUTH_H

#include "plex_models.h"

typedef struct {
    int  pin_id;
    char pin_code[8];            /* 4-char code shown to user */
    char token[PLEX_MAX_STR];   /* filled in when auth completes */
} PlexPin;

/*
 * Create a new PIN. Returns 0 on success, -1 on error.
 * Calls POST https://plex.tv/api/v2/pins
 * Fills pin->pin_id and pin->pin_code.
 */
int plex_auth_create_pin(PlexPin *pin);

/*
 * Poll for PIN auth completion. Returns:
 *   1  — authorized (pin->token filled in)
 *   0  — not yet authorized
 *  -1  — error or expired
 * Calls GET https://plex.tv/api/v2/pins/{pin->pin_id}
 */
int plex_auth_check_pin(PlexPin *pin);

/*
 * Fetch list of available Plex servers for the authenticated token.
 * Calls GET https://plex.tv/api/v2/resources?includeHttps=1
 * Fills servers[] array, sets *count. Returns 0 on success.
 */
#define PLEX_MAX_SERVERS 16
int plex_auth_get_servers(const char *token, PlexServer servers[], int *count);

/*
 * Refresh cfg->server_url and cfg->relay_url from Plex TV on startup.
 * Calls GET https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1
 * and finds the resource matching cfg->server_id.
 * Returns 0 on success, -1 if skipped (no token/server_id) or fetch failed.
 * On failure, cfg is unchanged (cached values remain intact).
 */
int plex_auth_refresh_server_urls(PlexConfig *cfg);

#endif /* PLEX_AUTH_H */
