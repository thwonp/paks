#ifndef PLEX_CONFIG_H
#define PLEX_CONFIG_H

#include <stdbool.h>
#include "plex_models.h"

/*
 * Load config from $SHARED_USERDATA_PATH/plexmusic/config.json.
 * Returns 0 on success, -1 if file doesn't exist or parse error.
 * cfg is zero-initialized on failure.
 */
int plex_config_load(PlexConfig *cfg);

/*
 * Save config to $SHARED_USERDATA_PATH/plexmusic/config.json.
 * Creates directory if needed. Returns 0 on success.
 */
int plex_config_save(const PlexConfig *cfg);

/* Returns true if cfg has a non-empty token and server_url. */
bool plex_config_is_valid(const PlexConfig *cfg);

/*
 * Return a mutable pointer to the application-global PlexConfig.
 * Defined in main.c. Modules call this to update the live config after
 * saving so the browse module sees the new values immediately.
 */
PlexConfig *plex_config_get_mutable(void);

#endif /* PLEX_CONFIG_H */
