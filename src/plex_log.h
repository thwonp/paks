#pragma once
#include <stdarg.h>

void plex_log_init(void);
void plex_log_write(int is_error, const char *fmt, ...);
void plex_log_flush(void);

/* Info-level: buffered, no flush. */
#define PLEX_LOG(fmt, ...)       plex_log_write(0, fmt, ##__VA_ARGS__)
/* Error-level: buffered + immediate flush so errors survive crashes. */
#define PLEX_LOG_ERROR(fmt, ...) plex_log_write(1, fmt, ##__VA_ARGS__)
