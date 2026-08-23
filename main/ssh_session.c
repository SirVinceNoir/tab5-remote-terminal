#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "esp_log.h"

#include <libssh2_config.h>
#include <libssh2.h>

#include "ssh_session.h"

static const char *TAG = "ssh_session";

struct ssh_session {
    int sock;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
};

static int waitsocket(int socket_fd, LIBSSH2_SESSION *session)
{
    struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
    fd_set fd;
    fd_set *writefd = NULL;
    fd_set *readfd = NULL;

    FD_ZERO(&fd);
    FD_SET(socket_fd, &fd);

    int dir = libssh2_session_block_directions(session);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) {
        readfd = &fd;
    }
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) {
        writefd = &fd;
    }

    return select(socket_fd + 1, readfd, writefd, NULL, &timeout);
}

static int connect_to_host(const char *host, uint16_t port)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *result = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int rc = getaddrinfo(host, port_str, &hints, &result);
    if (rc != 0 || result == NULL) {
        ESP_LOGE(TAG, "getaddrinfo('%s') failed: %d", host, rc);
        return -1;
    }

    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        freeaddrinfo(result);
        return -1;
    }

    if (connect(sock, result->ai_addr, result->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect() to %s:%u failed: errno=%d", host, port, errno);
        close(sock);
        freeaddrinfo(result);
        return -1;
    }

    freeaddrinfo(result);
    return sock;
}

ssh_session_t *ssh_session_connect(const char *host, uint16_t port, const char *username,
                                    const char *password, int cols, int rows)
{
    int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "libssh2_init() failed: %d", rc);
        return NULL;
    }

    int sock = connect_to_host(host, port);
    if (sock < 0) {
        libssh2_exit();
        return NULL;
    }
    ESP_LOGI(TAG, "TCP connected to %s:%u", host, port);

    LIBSSH2_SESSION *session = libssh2_session_init();
    if (session == NULL) {
        ESP_LOGE(TAG, "libssh2_session_init() returned NULL");
        close(sock);
        libssh2_exit();
        return NULL;
    }
    libssh2_session_set_blocking(session, 0);

    while ((rc = libssh2_session_handshake(session, sock)) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(sock, session);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "SSH handshake failed (%d)", rc);
        goto fail_session;
    }

    {
        size_t fp_len;
        int fp_type;
        const char *fp = libssh2_session_hostkey(session, &fp_len, &fp_type);
        if (fp != NULL) {
            /* Logged, not verified -- see the "host-key verification" gap
             * called out in ssh_session.h. */
            ESP_LOGW(TAG, "host key type=%d len=%u (NOT verified against a known_hosts store)",
                      fp_type, (unsigned int)fp_len);
        }
    }

    while ((rc = libssh2_userauth_password(session, username, password)) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(sock, session);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "password authentication failed (%d)", rc);
        goto fail_session;
    }
    ESP_LOGI(TAG, "authenticated as '%s'", username);

    LIBSSH2_CHANNEL *channel;
    do {
        channel = libssh2_channel_open_session(session);
        if (channel != NULL ||
            libssh2_session_last_error(session, NULL, NULL, 0) != LIBSSH2_ERROR_EAGAIN) {
            break;
        }
        waitsocket(sock, session);
    } while (1);
    if (channel == NULL) {
        ESP_LOGE(TAG, "could not open SSH channel");
        goto fail_session;
    }

    int prc;
    while ((prc = libssh2_channel_request_pty_ex(channel, "xterm", 5, NULL, 0, cols, rows, 0, 0))
           == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(sock, session);
    }
    if (prc != 0) {
        ESP_LOGE(TAG, "PTY request failed (%d)", prc);
        goto fail_channel;
    }

    while ((prc = libssh2_channel_shell(channel)) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(sock, session);
    }
    if (prc != 0) {
        ESP_LOGE(TAG, "shell request failed (%d)", prc);
        goto fail_channel;
    }

    ssh_session_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        goto fail_channel;
    }
    s->sock = sock;
    s->session = session;
    s->channel = channel;

    ESP_LOGI(TAG, "SSH session ready: %s@%s:%u", username, host, port);
    return s;

fail_channel:
    libssh2_channel_free(channel);
fail_session:
    libssh2_session_disconnect(session, "connect failed");
    libssh2_session_free(session);
    close(sock);
    libssh2_exit();
    return NULL;
}

static int ssh_transport_read(void *ctx, uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    ssh_session_t *s = (ssh_session_t *)ctx;

    fd_set fd;
    FD_ZERO(&fd);
    FD_SET(s->sock, &fd);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    int dir = libssh2_session_block_directions(s->session);
    fd_set *readfd = (dir & LIBSSH2_SESSION_BLOCK_INBOUND) ? &fd : NULL;
    fd_set *writefd = (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? &fd : NULL;
    select(s->sock + 1, readfd, writefd, NULL, &tv);
    /* Try the read regardless of select()'s verdict -- libssh2 may already
     * have decrypted data buffered with nothing new on the wire. */

    ssize_t n = libssh2_channel_read(s->channel, (char *)buf, max_len);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    if (n == 0 && libssh2_channel_eof(s->channel)) {
        return -1;
    }
    return (int)n;
}

static bool ssh_transport_write(void *ctx, const uint8_t *data, size_t len)
{
    ssh_session_t *s = (ssh_session_t *)ctx;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = libssh2_channel_write(s->channel, (const char *)data + sent, len - sent);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(s->sock, s->session);
            continue;
        }
        if (n < 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static void ssh_transport_close(void *ctx)
{
    ssh_session_t *s = (ssh_session_t *)ctx;
    if (s == NULL) {
        return;
    }
    libssh2_channel_close(s->channel);
    libssh2_channel_free(s->channel);
    libssh2_session_disconnect(s->session, "client disconnect");
    libssh2_session_free(s->session);
    close(s->sock);
    libssh2_exit();
    free(s);
}

session_transport_t ssh_session_as_transport(ssh_session_t *s)
{
    return (session_transport_t){
        .ctx = s,
        .read = ssh_transport_read,
        .write = ssh_transport_write,
        .close = ssh_transport_close,
    };
}
