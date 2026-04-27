#define _GNU_SOURCE
#include "plex_auth.h"
#include "plex_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/parson/parson.h"
#include "api.h"
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
 * plex_auth_get_servers
 * GET https://plex.tv/api/v2/resources?includeHttps=1
 * Filters for provides containing "server".
 * Picks first connection URI as server_url.
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

        /* Pick first connection URI */
        JSON_Array *conns = json_object_get_array(res, "connections");
        if (!conns || json_array_get_count(conns) == 0) continue;

        JSON_Object *first_conn = json_array_get_object(conns, 0);
        if (!first_conn) continue;
        const char *uri = json_object_get_string(first_conn, "uri");
        if (!uri || !*uri) continue;

        PlexServer *sv = &servers[*count];
        memset(sv, 0, sizeof(*sv));

        const char *name = json_object_get_string(res, "name");
        const char *cid  = json_object_get_string(res, "clientIdentifier");
        int owned        = json_object_get_boolean(res, "owned");  /* -1 = missing */

        if (name) strncpy(sv->name, name, sizeof(sv->name) - 1);
        if (cid)  strncpy(sv->id,   cid,  sizeof(sv->id) - 1);
        strncpy(sv->url, uri, sizeof(sv->url) - 1);
        sv->owned = (owned == 1);

        (*count)++;
    }

    json_value_free(root);
    return 0;
}
