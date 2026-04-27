#define _GNU_SOURCE
#include "plex_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "include/parson/parson.h"
#include "api.h"   /* LOG_error / LOG_info */

#define PLEX_CONFIG_FALLBACK_DIR "/mnt/SDCARD/.userdata/shared/plexmusic"
#define PLEX_CONFIG_FILENAME     "config.json"

/* Build the path to config.json into `out` (caller supplies buffer). */
static void config_path(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base) {
        snprintf(out, out_size, "%s/plexmusic/%s", base, PLEX_CONFIG_FILENAME);
    } else {
        snprintf(out, out_size, "%s/%s",
                 PLEX_CONFIG_FALLBACK_DIR, PLEX_CONFIG_FILENAME);
    }
}

/* Ensure the directory portion of a path exists (one level only). */
static void ensure_config_dir(const char *file_path)
{
    char dir[640];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    /* Create parent of directory first (best-effort) */
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
 * Public API
 * ------------------------------------------------------------------ */

int plex_config_load(PlexConfig *cfg)
{
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    char path[640];
    config_path(path, sizeof(path));

    JSON_Value *root = json_parse_file(path);
    if (!root) {
        LOG_error("[PlexConfig] Could not parse: %s\n", path);
        return -1;
    }

    JSON_Object *obj = json_value_get_object(root);
    if (!obj) {
        json_value_free(root);
        return -1;
    }

    const char *token       = json_object_get_string(obj, "token");
    const char *server_url  = json_object_get_string(obj, "server_url");
    const char *server_name = json_object_get_string(obj, "server_name");
    const char *server_id   = json_object_get_string(obj, "server_id");

    if (token)
        strncpy(cfg->token, token, sizeof(cfg->token) - 1);
    if (server_url)
        strncpy(cfg->server_url, server_url, sizeof(cfg->server_url) - 1);
    if (server_name)
        strncpy(cfg->server_name, server_name, sizeof(cfg->server_name) - 1);
    if (server_id)
        strncpy(cfg->server_id, server_id, sizeof(cfg->server_id) - 1);

    json_value_free(root);
    return 0;
}

int plex_config_save(const PlexConfig *cfg)
{
    if (!cfg) return -1;

    char path[640];
    config_path(path, sizeof(path));
    ensure_config_dir(path);

    JSON_Value  *root = json_value_init_object();
    JSON_Object *obj  = json_value_get_object(root);

    json_object_set_string(obj, "token",       cfg->token);
    json_object_set_string(obj, "server_url",  cfg->server_url);
    json_object_set_string(obj, "server_name", cfg->server_name);
    json_object_set_string(obj, "server_id",   cfg->server_id);

    char *json_str = json_serialize_to_string_pretty(root);
    json_value_free(root);
    if (!json_str) return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        json_free_serialized_string(json_str);
        return -1;
    }

    size_t len = strlen(json_str);
    ssize_t written = write(fd, json_str, len);
    int sync_err = fsync(fd);
    close(fd);
    json_free_serialized_string(json_str);

    if (written != (ssize_t)len || sync_err != 0) {
        LOG_error("[PlexConfig] Failed to write or fsync: %s\n", path);
        return -1;
    }
    return 0;
}

bool plex_config_is_valid(const PlexConfig *cfg)
{
    return cfg &&
           cfg->token[0]      != '\0' &&
           cfg->server_url[0] != '\0';
}
