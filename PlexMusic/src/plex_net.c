#define _GNU_SOURCE
#include "plex_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <zlib.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "psa/crypto.h"

#include "plex_log.h"

/*
 * Resolve host:port, connect with timeout. Returns connected fd >= 0, or -1.
 * Restores blocking mode on the returned fd.
 */
static int tcp_connect_timeout(const char *host, const char *port_str, int timeout_sec)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        if (res) freeaddrinfo(res);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    if (rc != 0) {
        /* Wait for connection to complete or timeout */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { timeout_sec, 0 };
        int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            /* timeout or error */
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            close(fd);
            return -1;
        }
    }

    /* Restore blocking */
    fcntl(fd, F_SETFL, flags);
    return fd;
}

/* Call once before any TLS operation — required by mbedTLS 3.x with PSA/TLS 1.3 */
static void plex_net_psa_init(void)
{
    static bool done = false;
    if (!done) {
        psa_crypto_init();
        done = true;
    }
}

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

#define PLEX_NET_MAX_REDIRECTS  5
#define PLEX_NET_DEFAULT_TIMEOUT 10
#define PLEX_MAX_URL            2048

#define PLEX_CLIENT_IDENTIFIER  "nextui-plex-music"
#define PLEX_PRODUCT            "PlexMusic"
#define PLEX_VERSION            "0.1.0"
#define PLEX_PLATFORM           "Linux"

/* TLS 1.3: non-fatal retryable errors */
#define SSL_RETRYABLE(r) \
    ((r) == MBEDTLS_ERR_SSL_WANT_READ  || \
     (r) == MBEDTLS_ERR_SSL_WANT_WRITE || \
     (r) == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)

/* ------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------ */

typedef struct {
    mbedtls_net_context     net;
    mbedtls_ssl_context     ssl;
    mbedtls_ssl_config      conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool initialized;
} PlexSSLCtx;

/* ------------------------------------------------------------------
 * Safe URL logging helper
 * ------------------------------------------------------------------ */

/* Returns a pointer to url, but with host+path only (no query string).
 * Writes into out_buf (caller provides, sized >= len of url + 1).
 * Copies up to the first '?' character. */
static void url_path_only(const char *url, char *out_buf, int out_size)
{
    const char *q = strchr(url, '?');
    int len = q ? (int)(q - url) : (int)strlen(url);
    if (len >= out_size) len = out_size - 1;
    memcpy(out_buf, url, len);
    out_buf[len] = '\0';
}

/* ------------------------------------------------------------------
 * URL parser
 * ------------------------------------------------------------------ */

static int parse_url(const char *url,
                     char *host, int host_size,
                     int  *port,
                     char *path, int path_size,
                     bool *is_https)
{
    if (!url || !host || !path || host_size < 1 || path_size < 1)
        return -1;

    *is_https = false;
    *port     = 80;
    host[0]   = '\0';
    path[0]   = '\0';

    const char *start = url;
    if (strncmp(url, "https://", 8) == 0) {
        start     = url + 8;
        *is_https = true;
        *port     = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
    }

    const char *path_start = strchr(start, '/');
    if (path_start) {
        snprintf(path, path_size, "%s", path_start);
    } else {
        strncpy(path, "/", path_size - 1);
        path[path_size - 1] = '\0';
        path_start = start + strlen(start);
    }

    const char *port_start = strchr(start, ':');
    if (port_start && port_start < path_start) {
        *port = atoi(port_start + 1);
        int len = (int)(port_start - start);
        if (len >= host_size) len = host_size - 1;
        memcpy(host, start, len);
        host[len] = '\0';
    } else {
        int len = (int)(path_start - start);
        if (len >= host_size) len = host_size - 1;
        memcpy(host, start, len);
        host[len] = '\0';
    }

    return 0;
}

/* ------------------------------------------------------------------
 * SSL context helpers
 * ------------------------------------------------------------------ */

static void ssl_ctx_free(PlexSSLCtx *ctx)
{
    if (!ctx) return;
    if (ctx->initialized)
        mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);
    free(ctx);
}

static PlexSSLCtx *ssl_ctx_connect(const char *host, int port, int timeout_sec)
{
    plex_net_psa_init();

    PlexSSLCtx *ctx = (PlexSSLCtx *)calloc(1, sizeof(PlexSSLCtx));
    if (!ctx) return NULL;

    const char *pers = "plex_net_fetch";
    mbedtls_net_init(&ctx->net);
    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->conf);
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    if (mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                               (const unsigned char *)pers, strlen(pers)) != 0 ||
        mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        ssl_ctx_free(ctx);
        return NULL;
    }

    /* conf_* calls must come before mbedtls_ssl_setup, which copies the config */
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    /* Force TLS 1.2 max: avoids PSA-dependent TLS 1.3 path in mbedTLS 3.x */
    mbedtls_ssl_conf_max_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);

    if (mbedtls_ssl_setup(&ctx->ssl, &ctx->conf) != 0) {
        ssl_ctx_free(ctx);
        return NULL;
    }

    mbedtls_ssl_set_hostname(&ctx->ssl, host);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int tcp_fd = tcp_connect_timeout(host, port_str, timeout_sec);
    if (tcp_fd < 0) {
        ssl_ctx_free(ctx);
        return NULL;
    }
    ctx->net.fd = tcp_fd;

    struct timeval tv = { timeout_sec, 0 };
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
                        mbedtls_net_send, mbedtls_net_recv, NULL);

    int ret;
    int retries = 0;
    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) break;
        if (!SSL_RETRYABLE(ret) || ++retries > 100) {
            PLEX_LOG_ERROR("[PlexNet] SSL handshake failed: -0x%04X host=%s\n", -ret, host);
            ssl_ctx_free(ctx);
            return NULL;
        }
        usleep(100000);
    }

    ctx->initialized = true;
    return ctx;
}

/* ------------------------------------------------------------------
 * Internal SSL/plain read/write helpers
 * ------------------------------------------------------------------ */

static int net_send_all(PlexSSLCtx *ssl_ctx, int sock_fd,
                        const char *data, int len)
{
    if (ssl_ctx) {
        int retries = 0;
        int sent;
        do {
            sent = mbedtls_ssl_write(&ssl_ctx->ssl,
                                     (const unsigned char *)data, len);
        } while (SSL_RETRYABLE(sent) && ++retries < 10);
        return sent;
    }
    return (int)send(sock_fd, data, len, 0);
}

static int net_recv_byte(PlexSSLCtx *ssl_ctx, int sock_fd, char *c)
{
    if (ssl_ctx) {
        int r;
        int retries = 0;
        do {
            r = mbedtls_ssl_read(&ssl_ctx->ssl, (unsigned char *)c, 1);
        } while (SSL_RETRYABLE(r) && ++retries < 150 && (usleep(10000), 1));
        return (r == 1) ? 1 : 0;
    }
    return (int)recv(sock_fd, c, 1, 0);
}

static int net_recv_buf(PlexSSLCtx *ssl_ctx, int sock_fd,
                        uint8_t *buf, int len)
{
    if (ssl_ctx) {
        int r;
        int retries = 0;
        do {
            r = mbedtls_ssl_read(&ssl_ctx->ssl, buf, len);
        } while (SSL_RETRYABLE(r) && ++retries < 50 && (usleep(10000), 1));
        return r;
    }
    return (int)recv(sock_fd, buf, len, 0);
}

/* ------------------------------------------------------------------
 * Request builder
 * ------------------------------------------------------------------ */

/*
 * Build the HTTP request into `out` (caller supplies buffer).
 * Returns 0 on success, -1 if the buffer is too small.
 */
static int build_request(const char *method_str,
                         const char *path,
                         const char *host,
                         const char *token,
                         const char *body,          /* POST body or NULL */
                         char *out, int out_size)
{
    /* Plex requires Content-Type for POST */
    char extra_headers[512] = {0};
    if (body && *body) {
        snprintf(extra_headers, sizeof(extra_headers),
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: %d\r\n",
                 (int)strlen(body));
    }

    char token_header[320] = {0};
    if (token && *token) {
        snprintf(token_header, sizeof(token_header),
                 "X-Plex-Token: %s\r\n", token);
    }

    int n = snprintf(out, out_size,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: application/json\r\n"
        "X-Plex-Client-Identifier: " PLEX_CLIENT_IDENTIFIER "\r\n"
        "X-Plex-Product: " PLEX_PRODUCT "\r\n"
        "X-Plex-Version: " PLEX_VERSION "\r\n"
        "X-Plex-Platform: " PLEX_PLATFORM "\r\n"
        "%s"          /* token header, may be empty */
        "%s"          /* content-type / content-length, may be empty */
        "Connection: close\r\n"
        "\r\n"
        "%s",         /* body, may be empty */
        method_str, path, host,
        token_header,
        extra_headers,
        body ? body : "");

    return (n < 0 || n >= out_size) ? -1 : 0;
}

/* ------------------------------------------------------------------
 * Forward declarations for redirect recursion
 * ------------------------------------------------------------------ */

static int plex_net_fetch_internal(const char *url,
                                   uint8_t *buffer, int buffer_size,
                                   const PlexNetOptions *opts,
                                   int redirect_depth);

static int plex_net_download_file_internal(const char *url,
                                           const char *filepath,
                                           volatile int *progress_pct,
                                           volatile bool *should_cancel,
                                           const PlexNetOptions *opts,
                                           int redirect_depth);

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

int plex_net_fetch(const char *url, uint8_t *buffer, int buffer_size,
                   const PlexNetOptions *opts)
{
    int r = plex_net_fetch_internal(url, buffer, buffer_size, opts, 0);
    if (r < 0) {
        usleep(1000000);   /* 1 s pause — lets antenna reconnect */
        r = plex_net_fetch_internal(url, buffer, buffer_size, opts, 0);
    }
    return r;
}

/* ------------------------------------------------------------------
 * Internal implementation
 * ------------------------------------------------------------------ */

static int plex_net_fetch_internal(const char *url,
                                   uint8_t *buffer, int buffer_size,
                                   const PlexNetOptions *opts,
                                   int redirect_depth)
{
    if (!url || !buffer || buffer_size <= 0) return -1;
    if (redirect_depth >= PLEX_NET_MAX_REDIRECTS) {
        PLEX_LOG_ERROR("[PlexNet] Too many redirects\n");
        return -1;
    }

    int timeout_sec = (opts && opts->timeout_sec > 0)
                      ? opts->timeout_sec
                      : PLEX_NET_DEFAULT_TIMEOUT;

    const char *token  = opts ? opts->token  : NULL;
    const char *body   = opts ? opts->body   : NULL;
    bool is_post = (opts && opts->method == PLEX_HTTP_POST);
    const char *method_str = is_post ? "POST" : "GET";

    char *host = (char *)malloc(256);
    char *path = (char *)malloc(1024);
    if (!host || !path) { free(host); free(path); return -1; }

    int  port;
    bool is_https;
    if (parse_url(url, host, 256, &port, path, 1024, &is_https) != 0) {
        char safe_url[PLEX_MAX_URL];
        url_path_only(url, safe_url, sizeof(safe_url));
        PLEX_LOG_ERROR("[PlexNet] Failed to parse URL: %s\n", safe_url);
        free(host); free(path); return -1;
    }

    /* Build request string */
    char *req = (char *)malloc(4096);
    if (!req) { free(host); free(path); return -1; }
    if (build_request(method_str, path, host, token, body, req, 4096) != 0) {
        char safe_url[PLEX_MAX_URL];
        url_path_only(url, safe_url, sizeof(safe_url));
        PLEX_LOG_ERROR("[PlexNet] Request too large for URL: %s\n", safe_url);
        free(req); free(host); free(path); return -1;
    }

    PlexSSLCtx *ssl_ctx = NULL;
    int sock_fd = -1;

    if (is_https) {
        ssl_ctx = ssl_ctx_connect(host, port, timeout_sec);
        if (!ssl_ctx) {
            PLEX_LOG_ERROR("[PlexNet] SSL connect failed: %s:%d\n", host, port);
            free(req); free(host); free(path); return -1;
        }
        sock_fd = ssl_ctx->net.fd;
    } else {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        sock_fd = tcp_connect_timeout(host, port_str, timeout_sec);
        if (sock_fd < 0) {
            PLEX_LOG_ERROR("[PlexNet] connect failed: %s:%d\n", host, port);
            free(req); free(host); free(path); return -1;
        }
        struct timeval tv = { timeout_sec, 0 };
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    /* Send request */
    if (net_send_all(ssl_ctx, sock_fd, req, (int)strlen(req)) < 0) {
        PLEX_LOG_ERROR("[PlexNet] Failed to send request\n");
        goto cleanup_fail;
    }
    free(req); req = NULL;

    /* Read response headers */
#define HDR_BUF_SIZE 8192
    char *hdr = (char *)malloc(HDR_BUF_SIZE);
    if (!hdr) goto cleanup_fail;

    int hdr_pos = 0;
    bool hdr_done = false;

    while (hdr_pos < HDR_BUF_SIZE - 1) {
        char c;
        if (net_recv_byte(ssl_ctx, sock_fd, &c) != 1) break;
        hdr[hdr_pos++] = c;
        if (hdr_pos >= 4 &&
            hdr[hdr_pos-4] == '\r' && hdr[hdr_pos-3] == '\n' &&
            hdr[hdr_pos-2] == '\r' && hdr[hdr_pos-1] == '\n') {
            hdr_done = true;
            break;
        }
    }
    hdr[hdr_pos] = '\0';

    if (!hdr_done) {
        PLEX_LOG_ERROR("[PlexNet] Incomplete HTTP headers\n");
        free(hdr);
        goto cleanup_fail;
    }

    /* Check for redirect */
    char *first_line_end = strstr(hdr, "\r\n");
    bool is_redirect = false;
    if (first_line_end) {
        is_redirect = (strstr(hdr, " 301 ") && strstr(hdr, " 301 ") < first_line_end) ||
                      (strstr(hdr, " 302 ") && strstr(hdr, " 302 ") < first_line_end) ||
                      (strstr(hdr, " 303 ") && strstr(hdr, " 303 ") < first_line_end) ||
                      (strstr(hdr, " 307 ") && strstr(hdr, " 307 ") < first_line_end) ||
                      (strstr(hdr, " 308 ") && strstr(hdr, " 308 ") < first_line_end);
    }

    if (is_redirect) {
        char *loc = strcasestr(hdr, "\nLocation:");
        if (loc) {
            loc += 10;
            while (*loc == ' ') loc++;
            char *end = loc;
            while (*end && *end != '\r' && *end != '\n') end++;
            char redir_url[1024];
            int rlen = (int)(end - loc);
            if (rlen >= (int)sizeof(redir_url)) rlen = (int)sizeof(redir_url) - 1;
            strncpy(redir_url, loc, rlen);
            redir_url[rlen] = '\0';

            free(hdr);
            if (ssl_ctx) ssl_ctx_free(ssl_ctx); else close(sock_fd);
            free(host); free(path);

            return plex_net_fetch_internal(redir_url, buffer, buffer_size,
                                           opts, redirect_depth + 1);
        }
        PLEX_LOG_ERROR("[PlexNet] Redirect without Location header\n");
        free(hdr);
        goto cleanup_fail;
    }

    /* Check HTTP status */
    int http_status = 0;
    {
        char *sp = strstr(hdr, "HTTP/");
        if (sp) {
            char *space = strchr(sp, ' ');
            if (space) http_status = atoi(space + 1);
        }
    }
    if (http_status >= 400) {
        char safe_url[PLEX_MAX_URL];
        url_path_only(url, safe_url, sizeof(safe_url));
        PLEX_LOG_ERROR("[PlexNet] HTTP %d for URL: %s\n", http_status, safe_url);
        free(hdr);
        goto cleanup_fail;
    }

    /* Detect chunked transfer encoding */
    bool is_chunked = false;
    {
        char *te = strcasestr(hdr, "\nTransfer-Encoding:");
        if (te) {
            char *le = strstr(te + 1, "\r\n");
            char *ck = strcasestr(te, "chunked");
            if (ck && (!le || ck < le)) is_chunked = true;
        }
    }

    /* Read body */
    int total_read = 0;

    if (is_chunked) {
        char csz_buf[24];
        int  csz_pos;

        while (total_read < buffer_size - 1) {
            /* Read chunk size line */
            csz_pos = 0;
            while (csz_pos < (int)sizeof(csz_buf) - 1) {
                char c;
                if (net_recv_byte(ssl_ctx, sock_fd, &c) != 1) goto chunked_done;
                if (c == '\r') continue;
                if (c == '\n') break;
                csz_buf[csz_pos++] = c;
            }
            csz_buf[csz_pos] = '\0';
            long chunk_sz = strtol(csz_buf, NULL, 16);
            if (chunk_sz <= 0) break;

            /* Read chunk data */
            long chunk_read = 0;
            while (chunk_read < chunk_sz && total_read < buffer_size - 1) {
                int to_read = (int)(chunk_sz - chunk_read);
                if (total_read + to_read > buffer_size - 1)
                    to_read = buffer_size - 1 - total_read;
                int r = net_recv_buf(ssl_ctx, sock_fd,
                                     buffer + total_read, to_read);
                if (r <= 0) goto chunked_done;
                total_read += r;
                chunk_read += r;
            }

            /* Discard overflow */
            while (chunk_read < chunk_sz) {
                char discard[256];
                int to_discard = (int)(chunk_sz - chunk_read);
                if (to_discard > (int)sizeof(discard))
                    to_discard = (int)sizeof(discard);
                int r = net_recv_buf(ssl_ctx, sock_fd,
                                     (uint8_t *)discard, to_discard);
                if (r <= 0) goto chunked_done;
                chunk_read += r;
            }

            /* Skip trailing CRLF */
            char crlf[2];
            for (int i = 0; i < 2; i++) {
                char c;
                if (net_recv_byte(ssl_ctx, sock_fd, &c) != 1) goto chunked_done;
                crlf[i] = c;
            }
            (void)crlf;
        }
        chunked_done:;
    } else {
        while (total_read < buffer_size - 1) {
            int r = net_recv_buf(ssl_ctx, sock_fd,
                                 buffer + total_read,
                                 buffer_size - total_read - 1);
            if (r <= 0) break;
            total_read += r;
        }
    }

    /* Gzip decompression */
    bool is_gzip = false;
    {
        char *ce = strcasestr(hdr, "\nContent-Encoding:");
        if (ce) {
            ce += 18;
            while (*ce == ' ') ce++;
            if (strncasecmp(ce, "gzip", 4) == 0) is_gzip = true;
        }
    }
    if (!is_gzip && total_read >= 2 &&
        buffer[0] == 0x1f && buffer[1] == 0x8b)
        is_gzip = true;

    if (is_gzip && total_read > 0) {
        uint8_t *decomp = (uint8_t *)malloc(buffer_size);
        if (decomp) {
            z_stream strm;
            memset(&strm, 0, sizeof(strm));
            strm.next_in  = buffer;
            strm.avail_in = total_read;
            strm.next_out  = decomp;
            strm.avail_out = buffer_size - 1;
            if (inflateInit2(&strm, MAX_WBITS + 16) == Z_OK) {
                int zr = inflate(&strm, Z_FINISH);
                if (zr == Z_STREAM_END || zr == Z_OK) {
                    total_read = (int)strm.total_out;
                    memcpy(buffer, decomp, total_read);
                }
                inflateEnd(&strm);
            }
            free(decomp);
        }
    }

    free(hdr);
    if (ssl_ctx) ssl_ctx_free(ssl_ctx); else close(sock_fd);
    free(host); free(path);
    return total_read;

cleanup_fail:
    if (ssl_ctx) ssl_ctx_free(ssl_ctx);
    else if (sock_fd >= 0) close(sock_fd);
    free(req);
    free(host); free(path);
    return -1;
}

/* ------------------------------------------------------------------
 * plex_net_download_file — streaming download to file
 * ------------------------------------------------------------------ */

#define DOWNLOAD_CHUNK_SIZE (32 * 1024)  /* 32 KB read buffer */

static int plex_net_download_file_internal(const char *url,
                                           const char *filepath,
                                           volatile int *progress_pct,
                                           volatile bool *should_cancel,
                                           const PlexNetOptions *opts,
                                           int redirect_depth)
{
    if (!url || !filepath) return -1;
    if (redirect_depth >= PLEX_NET_MAX_REDIRECTS) {
        PLEX_LOG_ERROR("[PlexNet] download: too many redirects\n");
        return -1;
    }

    int timeout_sec = (opts && opts->timeout_sec > 0)
                      ? opts->timeout_sec
                      : PLEX_NET_DEFAULT_TIMEOUT;

    const char *token = opts ? opts->token : NULL;

    char *host = (char *)malloc(256);
    char *path = (char *)malloc(1024);
    if (!host || !path) { free(host); free(path); return -1; }

    int  port;
    bool is_https;
    if (parse_url(url, host, 256, &port, path, 1024, &is_https) != 0) {
        char safe_url[PLEX_MAX_URL];
        url_path_only(url, safe_url, sizeof(safe_url));
        PLEX_LOG_ERROR("[PlexNet] download: failed to parse URL: %s\n", safe_url);
        free(host); free(path); return -1;
    }

    /* Build GET request (no POST body, no Accept: application/json for binary) */
    char *req = (char *)malloc(4096);
    if (!req) { free(host); free(path); return -1; }

    char token_header[320] = {0};
    if (token && *token) {
        snprintf(token_header, sizeof(token_header),
                 "X-Plex-Token: %s\r\n", token);
    }

    int n = snprintf(req, 4096,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: */*\r\n"
        "Accept-Encoding: identity\r\n"
        "X-Plex-Client-Identifier: " PLEX_CLIENT_IDENTIFIER "\r\n"
        "X-Plex-Product: " PLEX_PRODUCT "\r\n"
        "X-Plex-Version: " PLEX_VERSION "\r\n"
        "X-Plex-Platform: " PLEX_PLATFORM "\r\n"
        "%s"   /* optional token header */
        "Connection: close\r\n"
        "\r\n",
        path, host, token_header);
    if (n < 0 || n >= 4096) {
        PLEX_LOG_ERROR("[PlexNet] download: request too large\n");
        free(req); free(host); free(path); return -1;
    }

    PlexSSLCtx *ssl_ctx = NULL;
    int sock_fd = -1;
    FILE *outfile = NULL;
    uint8_t *chunk_buf = NULL;
    char *hdr = NULL;
    int result = -1;

    if (is_https) {
        ssl_ctx = ssl_ctx_connect(host, port, timeout_sec);
        if (!ssl_ctx) {
            PLEX_LOG_ERROR("[PlexNet] download: SSL connect failed: %s:%d\n", host, port);
            free(req); free(host); free(path); return -1;
        }
        sock_fd = ssl_ctx->net.fd;
    } else {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        sock_fd = tcp_connect_timeout(host, port_str, timeout_sec);
        if (sock_fd < 0) {
            PLEX_LOG_ERROR("[PlexNet] connect failed: %s:%d\n", host, port);
            free(req); free(host); free(path); return -1;
        }
        struct timeval tv = { timeout_sec, 0 };
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    /* Send request */
    if (net_send_all(ssl_ctx, sock_fd, req, (int)strlen(req)) < 0) {
        PLEX_LOG_ERROR("[PlexNet] download: failed to send request\n");
        goto dl_cleanup;
    }
    free(req); req = NULL;

    /* Read response headers */
    hdr = (char *)malloc(HDR_BUF_SIZE);
    if (!hdr) goto dl_cleanup;

    int hdr_pos = 0;
    bool hdr_done = false;

    while (hdr_pos < HDR_BUF_SIZE - 1) {
        if (should_cancel && *should_cancel) goto dl_cleanup;
        char c;
        if (net_recv_byte(ssl_ctx, sock_fd, &c) != 1) break;
        hdr[hdr_pos++] = c;
        if (hdr_pos >= 4 &&
            hdr[hdr_pos-4] == '\r' && hdr[hdr_pos-3] == '\n' &&
            hdr[hdr_pos-2] == '\r' && hdr[hdr_pos-1] == '\n') {
            hdr_done = true;
            break;
        }
    }
    hdr[hdr_pos] = '\0';

    if (!hdr_done) {
        PLEX_LOG_ERROR("[PlexNet] download: incomplete headers\n");
        goto dl_cleanup;
    }

    /* Check for redirect */
    {
        char *first_line_end = strstr(hdr, "\r\n");
        bool is_redirect = false;
        if (first_line_end) {
            is_redirect = (strstr(hdr, " 301 ") && strstr(hdr, " 301 ") < first_line_end) ||
                          (strstr(hdr, " 302 ") && strstr(hdr, " 302 ") < first_line_end) ||
                          (strstr(hdr, " 303 ") && strstr(hdr, " 303 ") < first_line_end) ||
                          (strstr(hdr, " 307 ") && strstr(hdr, " 307 ") < first_line_end) ||
                          (strstr(hdr, " 308 ") && strstr(hdr, " 308 ") < first_line_end);
        }
        if (is_redirect) {
            char *loc = strcasestr(hdr, "\nLocation:");
            if (loc) {
                loc += 10;
                while (*loc == ' ') loc++;
                char *end = loc;
                while (*end && *end != '\r' && *end != '\n') end++;
                char redir_url[1024];
                int rlen = (int)(end - loc);
                if (rlen >= (int)sizeof(redir_url)) rlen = (int)sizeof(redir_url) - 1;
                strncpy(redir_url, loc, rlen);
                redir_url[rlen] = '\0';

                free(hdr); hdr = NULL;
                if (ssl_ctx) ssl_ctx_free(ssl_ctx); else close(sock_fd);
                ssl_ctx = NULL; sock_fd = -1;
                free(host); free(path);

                return plex_net_download_file_internal(redir_url, filepath,
                                                       progress_pct, should_cancel,
                                                       opts, redirect_depth + 1);
            }
            PLEX_LOG_ERROR("[PlexNet] download: redirect without Location\n");
            goto dl_cleanup;
        }
    }

    /* Check HTTP status */
    {
        int http_status = 0;
        char *sp = strstr(hdr, "HTTP/");
        if (sp) {
            char *space = strchr(sp, ' ');
            if (space) http_status = atoi(space + 1);
        }
        if (http_status >= 400) {
            char safe_url[PLEX_MAX_URL];
            url_path_only(url, safe_url, sizeof(safe_url));
            PLEX_LOG_ERROR("[PlexNet] download: HTTP %d for: %s\n", http_status, safe_url);
            goto dl_cleanup;
        }
    }

    /* Parse Content-Length */
    long content_length = -1;
    {
        char *cl = strcasestr(hdr, "\nContent-Length:");
        if (cl) {
            cl += 16;
            while (*cl == ' ') cl++;
            content_length = atol(cl);
        }
    }

    /* Detect chunked transfer */
    bool is_chunked = false;
    {
        char *te = strcasestr(hdr, "\nTransfer-Encoding:");
        if (te) {
            char *le = strstr(te + 1, "\r\n");
            char *ck = strcasestr(te, "chunked");
            if (ck && (!le || ck < le)) is_chunked = true;
        }
    }

    free(hdr); hdr = NULL;

    /* Open output file */
    outfile = fopen(filepath, "wb");
    if (!outfile) {
        PLEX_LOG_ERROR("[PlexNet] download: failed to open file: %s\n", filepath);
        goto dl_cleanup;
    }

    chunk_buf = (uint8_t *)malloc(DOWNLOAD_CHUNK_SIZE);
    if (!chunk_buf) goto dl_cleanup;

    long total_written = 0;

    if (is_chunked) {
        char csz_buf[24];
        int  csz_pos;
        int  read_retries = 0;
        const int max_retries = 50;

        while (1) {
            if (should_cancel && *should_cancel) goto dl_cleanup;

            /* Read chunk size line */
            csz_pos = 0;
            while (csz_pos < (int)sizeof(csz_buf) - 1) {
                char c;
                int r;
                r = net_recv_byte(ssl_ctx, sock_fd, &c);
                if (r != 1) {
                    if (++read_retries > max_retries) goto chunked_done;
                    usleep(10000);
                    continue;
                }
                read_retries = 0;
                if (c == '\r') continue;
                if (c == '\n') break;
                csz_buf[csz_pos++] = c;
            }
            csz_buf[csz_pos] = '\0';
            long chunk_sz = strtol(csz_buf, NULL, 16);
            if (chunk_sz <= 0) break;

            /* Read chunk data */
            long chunk_read = 0;
            while (chunk_read < chunk_sz) {
                if (should_cancel && *should_cancel) goto chunked_done;
                int to_read = (int)(chunk_sz - chunk_read);
                if (to_read > DOWNLOAD_CHUNK_SIZE) to_read = DOWNLOAD_CHUNK_SIZE;
                int r = net_recv_buf(ssl_ctx, sock_fd, chunk_buf, to_read);
                if (r <= 0) goto chunked_done;
                fwrite(chunk_buf, 1, r, outfile);
                chunk_read  += r;
                total_written += r;
                if (progress_pct && content_length > 0) {
                    *progress_pct = (int)(total_written * 100 / content_length);
                }
            }

            /* Skip trailing CRLF */
            for (int i = 0; i < 2; i++) {
                char c;
                if (net_recv_byte(ssl_ctx, sock_fd, &c) != 1) goto chunked_done;
            }
        }
        chunked_done:;
    } else {
        int read_retries = 0;
        const int max_retries = 50;

        while (1) {
            if (should_cancel && *should_cancel) goto dl_cleanup;

            int r = net_recv_buf(ssl_ctx, sock_fd, chunk_buf, DOWNLOAD_CHUNK_SIZE);
            if (r == 0) break; /* EOF */
            if (r < 0) {
                if (SSL_RETRYABLE(r) || r == -1) {
                    if (++read_retries > max_retries) break;
                    usleep(10000);
                    continue;
                }
                break;
            }
            read_retries = 0;

            fwrite(chunk_buf, 1, r, outfile);
            total_written += r;

            /* Update progress from Content-Length */
            if (progress_pct && content_length > 0) {
                int pct = (int)((total_written * 100) / content_length);
                if (pct > 100) pct = 100;
                *progress_pct = pct;
            }
        }
    }

    free(chunk_buf); chunk_buf = NULL;
    fclose(outfile); outfile = NULL;

    if (total_written > 0) {
        if (progress_pct) *progress_pct = 100;
        long clamped = total_written > INT_MAX ? INT_MAX : total_written;
        result = (int)clamped;
    }

dl_cleanup:
    if (outfile) fclose(outfile);
    free(chunk_buf);
    free(hdr);
    free(req);
    if (ssl_ctx) ssl_ctx_free(ssl_ctx);
    else if (sock_fd >= 0) close(sock_fd);
    free(host); free(path);
    return result;
}

int plex_net_download_file(const char *url, const char *filepath,
                           volatile int *progress_pct,
                           volatile bool *should_cancel,
                           const PlexNetOptions *opts)
{
    int r = plex_net_download_file_internal(url, filepath, progress_pct,
                                            should_cancel, opts, 0);
    if (r < 0 && !(should_cancel && *should_cancel)) {
        usleep(1000000);
        r = plex_net_download_file_internal(url, filepath, progress_pct,
                                            should_cancel, opts, 0);
    }
    return r;
}
