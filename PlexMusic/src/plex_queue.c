#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plex_queue.h"
#include "plex_api.h"

/* Global singleton queue */
static PlexQueue g_queue;

PlexQueue *plex_queue_get(void)
{
    return &g_queue;
}

void plex_queue_set(const PlexConfig *cfg,
                    const PlexTrack *tracks, int count, int start_index)
{
    if (!tracks || count <= 0)
        return;

    if (count > PLEX_QUEUE_MAX_TRACKS)
        count = PLEX_QUEUE_MAX_TRACKS;

    memcpy(g_queue.tracks, tracks, count * sizeof(PlexTrack));
    g_queue.count = count;
    g_queue.current_index = (start_index >= 0 && start_index < count) ? start_index : 0;
    g_queue.active = true;
    g_queue.shuffle = false;
    g_queue.repeat_mode = REPEAT_OFF;
    for (int i = 0; i < count; i++) g_queue.shuffle_order[i] = i;

    if (g_queue.tracks[g_queue.current_index].local_path[0] != '\0')
        g_queue.stream_url[0] = '\0';
    else if (cfg->stream_bitrate_kbps > 0)
        plex_api_get_transcode_url(cfg, &g_queue.tracks[g_queue.current_index],
                                   cfg->stream_bitrate_kbps,
                                   g_queue.stream_url, sizeof(g_queue.stream_url));
    else
        plex_api_get_stream_url(cfg, &g_queue.tracks[g_queue.current_index],
                                g_queue.stream_url, sizeof(g_queue.stream_url));
}

bool plex_queue_next(const PlexConfig *cfg)
{
    if (!g_queue.active)
        return false;

    if (g_queue.current_index >= g_queue.count - 1) {
        if (g_queue.repeat_mode == REPEAT_ALL)
            g_queue.current_index = 0;
        else
            return false;
    } else {
        g_queue.current_index++;
    }

    const PlexTrack *t = &g_queue.tracks[g_queue.shuffle_order[g_queue.current_index]];
    if (t->local_path[0] != '\0')
        g_queue.stream_url[0] = '\0';
    else if (cfg->stream_bitrate_kbps > 0)
        plex_api_get_transcode_url(cfg, t,
                                   cfg->stream_bitrate_kbps,
                                   g_queue.stream_url, sizeof(g_queue.stream_url));
    else
        plex_api_get_stream_url(cfg, t,
                                g_queue.stream_url, sizeof(g_queue.stream_url));
    return true;
}

bool plex_queue_prev(const PlexConfig *cfg)
{
    if (!g_queue.active || g_queue.current_index <= 0)
        return false;

    g_queue.current_index--;
    const PlexTrack *t = &g_queue.tracks[g_queue.shuffle_order[g_queue.current_index]];
    if (t->local_path[0] != '\0')
        g_queue.stream_url[0] = '\0';
    else if (cfg->stream_bitrate_kbps > 0)
        plex_api_get_transcode_url(cfg, t,
                                   cfg->stream_bitrate_kbps,
                                   g_queue.stream_url, sizeof(g_queue.stream_url));
    else
        plex_api_get_stream_url(cfg, t,
                                g_queue.stream_url, sizeof(g_queue.stream_url));
    return true;
}

bool plex_queue_has_next(void)
{
    return g_queue.active &&
           (g_queue.current_index < g_queue.count - 1 ||
            g_queue.repeat_mode == REPEAT_ALL);
}

bool plex_queue_has_prev(void)
{
    return g_queue.active && g_queue.current_index > 0;
}

void plex_queue_clear(void)
{
    memset(&g_queue, 0, sizeof(g_queue));
}

const PlexTrack *plex_queue_current_track(void)
{
    if (!g_queue.active)
        return NULL;
    return &g_queue.tracks[g_queue.shuffle_order[g_queue.current_index]];
}

void plex_queue_toggle_shuffle(void)
{
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = true;
    }

    if (!g_queue.active || g_queue.count <= 0)
        return;

    int count = g_queue.count;

    if (!g_queue.shuffle) {
        /* Fisher-Yates shuffle */
        for (int i = count - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = g_queue.shuffle_order[i];
            g_queue.shuffle_order[i] = g_queue.shuffle_order[j];
            g_queue.shuffle_order[j] = tmp;
        }
        /* Keep the currently-playing track at current_index */
        int cur = g_queue.current_index;
        for (int i = 0; i < count; i++) {
            if (g_queue.shuffle_order[i] == cur) {
                /* Swap position i with current_index */
                int tmp = g_queue.shuffle_order[i];
                g_queue.shuffle_order[i] = g_queue.shuffle_order[cur];
                g_queue.shuffle_order[cur] = tmp;
                break;
            }
        }
        g_queue.shuffle = true;
    } else {
        /* Save the real track index we're currently at */
        int real_idx = g_queue.shuffle_order[g_queue.current_index];
        int saved_rating_key = g_queue.tracks[real_idx].rating_key;

        /* Restore identity order */
        for (int i = 0; i < count; i++)
            g_queue.shuffle_order[i] = i;

        /* Find where the current track now lives */
        for (int i = 0; i < count; i++) {
            if (g_queue.tracks[i].rating_key == saved_rating_key) {
                g_queue.current_index = i;
                break;
            }
        }
        g_queue.shuffle = false;
    }
}

void plex_queue_cycle_repeat(void)
{
    g_queue.repeat_mode = (RepeatMode)((g_queue.repeat_mode + 1) % 3);
}
