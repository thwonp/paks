/* radio.c - Stub implementation for PlexMusic.pak */
#include "radio.h"

RadioState Radio_getState(void) {
    return RADIO_STATE_STOPPED;
}

bool Radio_isActive(void) {
    return false;
}

int Radio_getAudioSamples(int16_t* buffer, int max_samples) {
    (void)buffer;
    (void)max_samples;
    return 0;
}
