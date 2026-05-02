#include "plex_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define PLEX_LOG_BUF_SIZE (64 * 1024)

static FILE *g_log = NULL;

void plex_log_init(void)
{
    const char *logs_path = getenv("LOGS_PATH");
    char path[256];
    if (logs_path && *logs_path)
        snprintf(path, sizeof(path), "%s/plexmusic.txt", logs_path);
    else
        snprintf(path, sizeof(path), "/tmp/plexmusic.txt");
    g_log = fopen(path, "w");
    if (g_log)
        setvbuf(g_log, NULL, _IOLBF, 0);  /* line-buffered: each \n flushes, survives crashes */
}

void plex_log_write(int is_error, const char *fmt, ...)
{
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    if (is_error)
        fflush(g_log);
}

void plex_log_flush(void)
{
    if (g_log) {
        fflush(g_log);
        fclose(g_log);
        g_log = NULL;
    }
}
