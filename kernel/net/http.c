/*
 * http.c - HTTP Client
 */

#include "net.h"
#include "log.h"
#include "string.h"
#include "../mem.h"
#include <stdint.h>

/* ================================================================
 * Parse URL into hostname, port, and path
 * Format: http://hostname[:port][/path]
 * ================================================================ */
static int http_parse_url(const char *url, char *hostname, int hostname_size,
                           uint16_t *port, char *path, int path_size) {
    if (!url || !hostname || !port || !path) return -1;

    /* Skip "http://" prefix */
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    /* Extract hostname */
    const char *host_start = p;
    const char *host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '\0') {
        host_end++;
    }
    int host_len = (int)(host_end - host_start);
    if (host_len <= 0 || host_len >= hostname_size) return -1;
    memcpy(hostname, host_start, (size_t)host_len);
    hostname[host_len] = '\0';

    p = host_end;

    /* Extract port */
    *port = HTTP_DEFAULT_PORT;
    if (*p == ':') {
        p++;
        uint16_t custom_port = 0;
        while (*p >= '0' && *p <= '9') {
            custom_port = (uint16_t)(custom_port * 10 + (uint16_t)(*p - '0'));
            p++;
        }
        if (custom_port > 0) {
            *port = custom_port;
        }
    }

    /* Extract path */
    if (*p == '/') {
        const char *path_start = p;
        int path_len = 0;
        while (*p && path_len < path_size - 1) {
            p++;
            path_len++;
        }
        if (path_len > 0) {
            memcpy(path, path_start, (size_t)path_len);
            path[path_len] = '\0';
        } else {
            path[0] = '/';
            path[1] = '\0';
        }
    } else {
        path[0] = '/';
        path[1] = '\0';
    }

    return 0;
}

/* ================================================================
 * http_get - Perform HTTP GET request
 * ================================================================ */
int http_get(const char *url, char *response_buf, size_t buf_size) {
    if (!url || !response_buf || buf_size == 0) return -1;

    char hostname[256];
    uint16_t port;
    char path[512];

    if (http_parse_url(url, hostname, (int)sizeof(hostname),
                        &port, path, (int)sizeof(path)) < 0) {
        log_printf(LOG_LEVEL_ERR, "http: failed to parse URL '%s'\n", url);
        return -1;
    }

    log_printf(LOG_LEVEL_DEBUG, "http: GET %s:%d%s\n", hostname, port, path);

    /* Resolve hostname via DNS */
    uint8_t server_ip[4];
    if (dns_query(hostname, server_ip) < 0) {
        log_printf(LOG_LEVEL_ERR, "http: DNS lookup failed for %s\n",
                   hostname);
        return -1;
    }

    /* Create TCP socket and connect */
    int sock = tcp_socket_create();
    if (sock < 0) {
        log_printf(LOG_LEVEL_ERR, "http: failed to create socket\n");
        return -1;
    }

    if (tcp_connect(sock, server_ip, port) < 0) {
        log_printf(LOG_LEVEL_ERR, "http: failed to connect to %d.%d.%d.%d:%d\n",
                   server_ip[0], server_ip[1], server_ip[2], server_ip[3],
                   port);
        tcp_close(sock);
        return -1;
    }

    /* Wait for connection to establish (poll for SYN-ACK) */
    {
        int retry;
        for (retry = 0; retry < 50; retry++) {
            net_poll();
            uint8_t peer_ip[4];
            uint16_t peer_port;
            if (tcp_getpeername(sock, peer_ip, &peer_port) == 0) {
                /* Socket is connected */
                break;
            }
        }
    }

    /* Build HTTP request */
    char request[1024];
    int req_len = snprintf(request, sizeof(request),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           path, hostname);

    if (req_len < 0 || req_len >= (int)sizeof(request)) {
        log_printf(LOG_LEVEL_ERR, "http: request too long\n");
        tcp_close(sock);
        return -1;
    }

    /* Send request */
    if (tcp_send(sock, request, req_len) < 0) {
        log_printf(LOG_LEVEL_ERR, "http: failed to send request\n");
        tcp_close(sock);
        return -1;
    }

    /* Receive response */
    size_t total = 0;
    int header_done = 0;
    int retry;

    for (retry = 0; retry < 200; retry++) {
        net_poll();

        char tmp[512];
        int n = tcp_recv(sock, tmp, (int)sizeof(tmp));
        if (n < 0) break;
        if (n == 0) {
            /* No data yet, keep polling */
            continue;
        }

        if (!header_done) {
            /* Find end of headers (\r\n\r\n) */
            int i;
            int header_end = -1;
            for (i = 0; i < n - 3; i++) {
                if (tmp[i] == '\r' && tmp[i + 1] == '\n' &&
                    tmp[i + 2] == '\r' && tmp[i + 3] == '\n') {
                    header_end = i + 4;
                    break;
                }
            }

            if (header_end >= 0) {
                /* Copy body after headers */
                header_done = 1;
                int body_start = header_end;
                int body_len = n - body_start;
                if (body_len > 0 && total < buf_size) {
                    size_t copy = (size_t)body_len;
                    if (total + copy > buf_size) copy = buf_size - total;
                    memcpy(response_buf + total, tmp + body_start, copy);
                    total += copy;
                }
            }
        } else {
            /* Already past headers, copy body directly */
            if (n > 0 && total < buf_size) {
                size_t copy = (size_t)n;
                if (total + copy > buf_size) copy = buf_size - total;
                memcpy(response_buf + total, tmp, copy);
                total += copy;
            }
        }

        if (header_done && total >= buf_size) break;
    }

    tcp_close(sock);

    if (total > 0) {
        log_printf(LOG_LEVEL_DEBUG, "http: received %u bytes\n",
                   (unsigned int)total);
        return (int)total;
    }

    log_printf(LOG_LEVEL_ERR, "http: no response body\n");
    return -1;
}