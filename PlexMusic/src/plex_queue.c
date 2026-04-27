#include <string.h>
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

    if (g_queue.tracks[g_queue.current_index].local_path[0] != '\0')
        g_queue.stream_url[0] = '\0';
    else
        plex_api_get_stream_url(cfg, &g_queue.tracks[g_queue.current_index],
                                g_queue.stream_url, sizeof(g_queue.stream_url));
}

bool plex_queue_next(const PlexConfig *cfg)
{
    if (!g_queue.active || g_queue.current_index >= g_queue.count - 1)
        return false;

    g_queue.current_index++;
    if (g_queue.tracks[g_queue.current_index].local_path[0] != '\0')
        g_queue.stream_url[0] = '\0';
    else
        plex_api_get_stream_url(cfg, &g_queue.tracks[g_queue.current_index],
                                g_queue.stream_url, sizeof(g_queue.stream_url));
    return true;
}

bool plex_queue_prev(const PlexConfig *cfg)
{
    if (!g_queue.active || g_queue.current_index <= 0)
        return false;

    g_queue.current_index--;
    if (g_queue.tracks[g_queue.current_index].local_path[0] != '\0')
        g_queue.stream_url[0] = '\0';
    else
        plex_api_get_stream_url(cfg, &g_queue.tracks[g_queue.current_index],
                                g_queue.stream_url, sizeof(g_queue.stream_url));
    return true;
}

bool plex_queue_has_next(void)
{
    return g_queue.active && g_queue.current_index < g_queue.count - 1;
}

bool plex_queue_has_prev(void)
{
    return g_queue.active && g_queue.current_index > 0;
}

void plex_queue_clear(void)
{
    memset(&g_queue, 0, sizeof(g_queue));
}
