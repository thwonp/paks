/*
 * radio.h - Stub for PlexMusic.pak
 *
 * The music player engine (player.c) has a shared audio callback that checks
 * Radio_isActive() before handling normal playback. These stubs satisfy the
 * linker; radio is never active in this pak so all code paths that branch on
 * it are effectively dead.
 */
#ifndef __RADIO_H__
#define __RADIO_H__

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    RADIO_STATE_STOPPED = 0,
    RADIO_STATE_CONNECTING,
    RADIO_STATE_BUFFERING,
    RADIO_STATE_PLAYING,
    RADIO_STATE_ERROR
} RadioState;

RadioState Radio_getState(void);
bool       Radio_isActive(void);
int        Radio_getAudioSamples(int16_t* buffer, int max_samples);

#endif /* __RADIO_H__ */
