#define _GNU_SOURCE
#include "plex_auth.h"
#include "plex_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/parson/parson.h"
#include "api.h"
#include "plex_config.h"
#include "plex_log.h"

#define PLEX_TV_API "https://plex.tv/api/v2"
#define RESP_BUF_SIZE (32 * 1024)

/* ------------------------------------------------------------------
 * plex_auth_create_pin
 * POST https://plex.tv/api/v2/pins
 * body: strong=true
 * Response: { "id": 12345, "code": "ABCD" }
 * ------------------------------------------------------------------ */
int plex_auth_create_pin(PlexPin *pin)
{
    if (!pin) return -1;
    memset(pin, 0, sizeof(*pin));

    uint8_t *buf = (uint8_t *)malloc(RESP_BUF_SIZE);
    if (!buf) return -1;

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_POST,
        .body        = "strong=false",
        .token       = NULL,
        .timeout_sec = 15
    };

    int n = plex_net_fetch(PLEX_TV_API "/pins", buf, RESP_BUF_SIZE, &opts);
    if (n <= 0) {
        PLEX_LOG_ERROR("[PlexAuth] create_pin: fetch failed\n");
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    JSON_Value  *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        PLEX_LOG_ERROR("[PlexAuth] create_pin: JSON parse failed\n");
        return -1;
    }

    JSON_Object *obj = json_value_get_object(root);
    if (!obj) {
        json_value_free(root);
        return -1;
    }

    double id_d       = json_object_get_number(obj, "id");
    const char *code  = json_object_get_string(obj, "code");

    if (id_d == 0.0 || !code || !*code) {
        PLEX_LOG_ERROR("[PlexAuth] create_pin: missing id or code in response\n");
        json_value_free(root);
        return -1;
    }

    pin->pin_id = (int)id_d;
    strncpy(pin->pin_code, code, sizeof(pin->pin_code) - 1);

    json_value_free(root);
    return 0;
}

/* ------------------------------------------------------------------
 * plex_auth_check_pin
 * GET https://plex.tv/api/v2/pins/{pin_id}
 * Response: { "authToken": "TOKEN" }  (null/empty while pending)
 * Returns:  1 = authorized, 0 = pending, -1 = error
 * ------------------------------------------------------------------ */
int plex_auth_check_pin(PlexPin *pin)
{
    if (!pin || pin->pin_id == 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(RESP_BUF_SIZE);
    if (!buf) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/pins/%d", PLEX_TV_API, pin->pin_id);

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = NULL,
        .timeout_sec = 15
    };

    int n = plex_net_fetch(url, buf, RESP_BUF_SIZE, &opts);
    if (n <= 0) {
        PLEX_LOG_ERROR("[PlexAuth] check_pin: fetch failed\n");
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    JSON_Value  *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        PLEX_LOG_ERROR("[PlexAuth] check_pin: JSON parse failed\n");
        return -1;
    }

    JSON_Object *obj = json_value_get_object(root);
    if (!obj) { json_value_free(root); return -1; }

    const char *auth_token = json_object_get_string(obj, "authToken");
    int result = 0;

    if (auth_token && *auth_token) {
        strncpy(pin->token, auth_token, sizeof(pin->token) - 1);
        result = 1;
    }

    json_value_free(root);
    return result;
}

/* ------------------------------------------------------------------
 * conn_rank — lower is better
 * Tier 0: local=true
 * Tier 1: local=false, relay=false
 * Tier 2: relay=true
 * Within a tier, prefer non-plex.direct (direct IP) over plex.direct —
 * plex.direct requires Plex's DNS to resolve and may fail on some networks.
 * Within each (tier, plex_direct) pair, prefer HTTPS over HTTP.
 * Final rank = tier*4 + plex_direct*2 + https_penalty
 * ------------------------------------------------------------------ */
static int conn_rank(JSON_Object *conn)
{
    int local = json_object_get_boolean(conn, "local");
    int relay = json_object_get_boolean(conn, "relay");
    const char *proto = json_object_get_string(conn, "protocol");
    const char *uri   = json_object_get_string(conn, "uri");

    int tier;
    if (local == 1)
        tier = 0;
    else if (relay == 1)
        tier = 2;
    else
        tier = 1;

    int plex_direct   = (uri && strstr(uri, "plex.direct")) ? 1 : 0;
    int https_penalty = (proto && strcmp(proto, "https") == 0) ? 0 : 1;
    return tier * 4 + plex_direct * 2 + https_penalty;
}

/* ------------------------------------------------------------------
 * best_conn_uri — walk connections array, return URI of best-ranked entry.
 * Returns NULL if array is empty or no valid URI found.
 * ------------------------------------------------------------------ */
static const char *best_conn_uri(JSON_Array *conns)
{
    if (!conns) return NULL;
    size_t n = json_array_get_count(conns);
    if (n == 0) return NULL;

    const char *best_uri  = NULL;
    int         best_score = 999;

    for (size_t i = 0; i < n; i++) {
        JSON_Object *c = json_array_get_object(conns, i);
        if (!c) continue;
        const char *uri = json_object_get_string(c, "uri");
        if (!uri || !*uri) continue;

        int score = conn_rank(c);
        if (score < best_score) {
            best_score = score;
            best_uri   = uri;
        }
    }
    return best_uri;
}

/* ------------------------------------------------------------------
 * best_nonlocal_uri — walk connections array, return URI of best-ranked
 * entry where local != 1. Covers both external plex.direct and relay.
 * Returns NULL if all connections are local.
 * ------------------------------------------------------------------ */
static const char *best_nonlocal_uri(JSON_Array *conns)
{
    if (!conns) return NULL;
    size_t n = json_array_get_count(conns);
    if (n == 0) return NULL;

    const char *best_uri   = NULL;
    int         best_score = 999;

    for (size_t i = 0; i < n; i++) {
        JSON_Object *c = json_array_get_object(conns, i);
        if (!c) continue;
        if (json_object_get_boolean(c, "local") == 1) continue;
        const char *uri = json_object_get_string(c, "uri");
        if (!uri || !*uri) continue;

        int score = conn_rank(c);
        if (score < best_score) {
            best_score = score;
            best_uri   = uri;
        }
    }
    return best_uri;
}

/* ------------------------------------------------------------------
 * plex_auth_get_servers
 * GET https://plex.tv/api/v2/resources?includeHttps=1
 * Filters for provides containing "server".
 * Picks best-ranked connection URI (local > non-relay > relay, HTTPS
 * preferred within tier).
 * ------------------------------------------------------------------ */
int plex_auth_get_servers(const char *token, PlexServer servers[], int *count)
{
    if (!token || !servers || !count) return -1;
    *count = 0;

    uint8_t *buf = (uint8_t *)malloc(RESP_BUF_SIZE);
    if (!buf) return -1;

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = token,
        .timeout_sec = 15
    };

    int n = plex_net_fetch(PLEX_TV_API "/resources?includeHttps=1",
                           buf, RESP_BUF_SIZE, &opts);
    if (n <= 0) {
        PLEX_LOG_ERROR("[PlexAuth] get_servers: fetch failed\n");
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    JSON_Value *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        PLEX_LOG_ERROR("[PlexAuth] get_servers: JSON parse failed\n");
        return -1;
    }

    /* Response is a JSON array */
    JSON_Array *arr = json_value_get_array(root);
    if (!arr) {
        json_value_free(root);
        return -1;
    }

    size_t total = json_array_get_count(arr);
    for (size_t i = 0; i < total && *count < PLEX_MAX_SERVERS; i++) {
        JSON_Object *res = json_array_get_object(arr, i);
        if (!res) continue;

        const char *provides = json_object_get_string(res, "provides");
        if (!provides || !strstr(provides, "server")) continue;

        /* Pick best-ranked connection URI */
        JSON_Array  *conns     = json_object_get_array(res, "connections");
        const char  *uri       = best_conn_uri(conns);
        if (!uri) continue;

        PlexServer *sv = &servers[*count];
        memset(sv, 0, sizeof(*sv));

        const char *name      = json_object_get_string(res, "name");
        const char *cid       = json_object_get_string(res, "clientIdentifier");
        int owned             = json_object_get_boolean(res, "owned");  /* -1 = missing */
        const char *nonlocal_uri = best_nonlocal_uri(conns);

        if (name) strncpy(sv->name, name, sizeof(sv->name) - 1);
        if (cid)  strncpy(sv->id,   cid,  sizeof(sv->id) - 1);
        strncpy(sv->url, uri, sizeof(sv->url) - 1);
        if (nonlocal_uri) strncpy(sv->relay_url, nonlocal_uri, sizeof(sv->relay_url) - 1);
        sv->owned = (owned == 1);

        PLEX_LOG("[PlexAuth] server[%d]: name=%s url=%s relay_url=%s conns=%zu\n",
                 *count, sv->name, sv->url, sv->relay_url, json_array_get_count(conns));

        (*count)++;
    }

    json_value_free(root);
    return 0;
}

/* ------------------------------------------------------------------
 * plex_auth_refresh_server_urls
 * GET https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1
 * Finds the resource matching cfg->server_id, then updates
 * cfg->server_url and cfg->relay_url with fresh connection URIs.
 * Returns 0 on success, -1 on failure (cfg unchanged).
 * ------------------------------------------------------------------ */
int plex_auth_refresh_server_urls(PlexConfig *cfg)
{
    if (!cfg || cfg->token[0] == '\0' || cfg->server_id[0] == '\0')
        return -1;

    uint8_t *buf = (uint8_t *)malloc(RESP_BUF_SIZE);
    if (!buf) return -1;

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = cfg->token,
        .timeout_sec = 5,
        .no_persist  = true
    };

    int n = plex_net_fetch(PLEX_TV_API "/resources?includeHttps=1&includeRelay=1",
                           buf, RESP_BUF_SIZE, &opts);
    if (n <= 0) {
        PLEX_LOG("[Auth] server URL refresh failed (using cached)\n");
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    JSON_Value *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        PLEX_LOG("[Auth] server URL refresh failed (using cached)\n");
        return -1;
    }

    JSON_Array *arr = json_value_get_array(root);
    if (!arr) {
        json_value_free(root);
        PLEX_LOG("[Auth] server URL refresh failed (using cached)\n");
        return -1;
    }

    size_t total = json_array_get_count(arr);
    int found = 0;
    for (size_t i = 0; i < total; i++) {
        JSON_Object *res = json_array_get_object(arr, i);
        if (!res) continue;

        const char *mid = json_object_get_string(res, "machineIdentifier");
        if (!mid || strcmp(mid, cfg->server_id) != 0) continue;

        JSON_Array *conns = json_object_get_array(res, "connections");
        const char *new_server = best_conn_uri(conns);
        const char *new_relay  = best_nonlocal_uri(conns);

        if (new_server) {
            strncpy(cfg->server_url, new_server, sizeof(cfg->server_url) - 1);
            cfg->server_url[sizeof(cfg->server_url) - 1] = '\0';
        }
        if (new_relay) {
            strncpy(cfg->relay_url, new_relay, sizeof(cfg->relay_url) - 1);
            cfg->relay_url[sizeof(cfg->relay_url) - 1] = '\0';
        }

        if (new_server || new_relay) {
            plex_config_save(cfg);
            PLEX_LOG("[Auth] server URLs refreshed: server_url=%s relay_url=%s\n",
                     cfg->server_url, cfg->relay_url);
        }

        found = 1;
        break;
    }

    json_value_free(root);

    if (!found) {
        PLEX_LOG("[Auth] server URL refresh failed (using cached)\n");
        return -1;
    }

    return 0;
}

