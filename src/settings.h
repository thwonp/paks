/*
 * settings.h - Stub for PlexMusic.pak
 *
 * player.c calls Settings_getBassFilterHz() and Settings_getSoftLimiterThreshold()
 * in its audio callback for speaker processing. Stubs return "off" values so the
 * processing paths are skipped. App-specific settings for PlexMusic will be added
 * in a later task.
 */
#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <stdbool.h>

/* Speaker bass filter (high-pass cutoff in Hz, 0 = off) */
int   Settings_getBassFilterHz(void);

/* Speaker soft limiter threshold (0.0 = off) */
float Settings_getSoftLimiterThreshold(void);

/* Screen off timeout (seconds; 0 = disabled) */
int Settings_getScreenOffTimeout(void);

#endif /* __SETTINGS_H__ */
