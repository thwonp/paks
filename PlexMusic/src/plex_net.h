#ifndef PLEX_NET_H
#define PLEX_NET_H

#include <stdint.h>
#include <stdbool.h>

/* HTTP methods */
typedef enum { PLEX_HTTP_GET, PLEX_HTTP_POST } PlexHttpMethod;

/* Per-request options */
typedef struct {
    PlexHttpMethod method;
    const char    *body;         /* POST body (NULL for GET) */
    const char    *token;        /* X-Plex-Token value (NULL if not needed) */
    int            timeout_sec;  /* 0 = default (15s) */
} PlexNetOptions;

/*
 * Fetch a URL with Plex headers.
 *
 * Always adds: X-Plex-Client-Identifier, X-Plex-Product, X-Plex-Version,
 *              X-Plex-Platform, Accept: application/json
 * If opts->token is non-NULL, adds X-Plex-Token header.
 *
 * Returns bytes written to buffer, or -1 on error.
 */
int plex_net_fetch(const char *url, uint8_t *buffer, int buffer_size,
                   const PlexNetOptions *opts);

/*
 * Download a URL to a local file path in streaming fashion (no large memory
 * buffer). Uses the same SSL/HTTP infrastructure as plex_net_fetch.
 *
 * progress_pct: updated with 0-100 as download proceeds; may be NULL.
 *               Updated from Content-Length header when available.
 * should_cancel: if set to true by caller, abort download and return -1; may
 *                be NULL.
 * opts: standard PlexNetOptions (token may be NULL if already in URL).
 *
 * Returns total bytes written on success, or -1 on error or cancellation.
 */
int plex_net_download_file(const char *url, const char *filepath,
                           volatile int *progress_pct,
                           volatile bool *should_cancel,
                           const PlexNetOptions *opts);

#endif /* PLEX_NET_H */
