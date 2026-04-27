/* settings.c - Stub implementation for PlexMusic.pak */
#include "settings.h"

/* Return 0 = no bass filter (off) */
int Settings_getBassFilterHz(void) {
    return 0;
}

/* Return 0.0 = no soft limiter (off) */
float Settings_getSoftLimiterThreshold(void) {
    return 0.0f;
}

/* Return 0 = screen off timeout disabled */
int Settings_getScreenOffTimeout(void) {
    return 0;
}
