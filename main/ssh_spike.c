#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <libssh2_config.h>
#include <libssh2.h>

#include "ssh_spike.h"

static const char *TAG = "ssh_spike";

/* Adapted from libssh2_esp's examples/ssh2_exec reference (itself adapted
 * from libssh2/example/ssh2_exec.c), trimmed for this project: no LittleFS
 * known_hosts persistence (just logs the fingerprint -- this is a
 * connectivity/auth spike, not the hardened Phase 5 SSH client), no pubkey
 * path, credentials from Kconfig instead of argv. */

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

void ssh_spike_start(void)
{
    if (strlen(CONFIG_TAB5_SSH_USERNAME) == 0) {
        ESP_LOGW(TAG, "TAB5_SSH_USERNAME is empty -- set it via 'idf.py menuconfig' "
                       "under Tab5 Remote Terminal, then reflash");
        return;
    }

    int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "SSH SPIKE: FAIL -- libssh2_init() returned %d", rc);
        return;
    }

    int sock = connect_to_host(CONFIG_TAB5_SSH_HOST, CONFIG_TAB5_SSH_PORT);
    if (sock < 0) {
        ESP_LOGE(TAG, "SSH SPIKE: FAIL -- could not connect to %s:%d",
                  CONFIG_TAB5_SSH_HOST, CONFIG_TAB5_SSH_PORT);
        libssh2_exit();
        return;
    }
    ESP_LOGI(TAG, "TCP connected to %s:%d", CONFIG_TAB5_SSH_HOST, CONFIG_TAB5_SSH_PORT);

    LIBSSH2_SESSION *session = libssh2_session_init();
    if (session == NULL) {
        ESP_LOGE(TAG, "SSH SPIKE: FAIL -- libssh2_session_init() returned NULL");
        close(sock);
        libssh2_exit();
        return;
    }
    libssh2_session_set_blocking(session, 0);

    while ((rc = libssh2_session_handshake(session, sock)) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(sock, session);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "SSH SPIKE: FAIL -- SSH handshake failed (%d)", rc);
        goto shutdown;
    }
    ESP_LOGI(TAG, "SSH handshake complete");

    {
        size_t fp_len;
        int fp_type;
        const char *fingerprint = libssh2_session_hostkey(session, &fp_len, &fp_type);
        if (fingerprint != NULL) {
            ESP_LOGI(TAG, "host key type=%d, len=%u (not verified -- spike only)",
                      fp_type, (unsigned int)fp_len);
        }
    }

    while ((rc = libssh2_userauth_password(session, CONFIG_TAB5_SSH_USERNAME,
                                            CONFIG_TAB5_SSH_PASSWORD)) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(sock, session);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "SSH SPIKE: FAIL -- password authentication failed (%d)", rc);
        goto shutdown;
    }
    ESP_LOGI(TAG, "authenticated as '%s'", CONFIG_TAB5_SSH_USERNAME);

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
        ESP_LOGE(TAG, "SSH SPIKE: FAIL -- could not open channel");
        goto shutdown;
    }

    while ((rc = libssh2_channel_exec(channel, CONFIG_TAB5_SSH_COMMAND)) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(sock, session);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "SSH SPIKE: FAIL -- exec of '%s' failed (%d)", CONFIG_TAB5_SSH_COMMAND, rc);
        libssh2_channel_free(channel);
        goto shutdown;
    }

    char buffer[512];
    ssize_t nread;
    for (;;) {
        do {
            nread = libssh2_channel_read(channel, buffer, sizeof(buffer) - 1);
            if (nread > 0) {
                buffer[nread] = '\0';
                ESP_LOGI(TAG, "output: %s", buffer);
            }
        } while (nread > 0);

        if (nread == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(sock, session);
        } else {
            break;
        }
    }

    while (libssh2_channel_close(channel) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(sock, session);
    }
    int exit_code = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);

    ESP_LOGI(TAG, "SSH SPIKE: PASS -- ran '%s' on %s, exit code %d",
              CONFIG_TAB5_SSH_COMMAND, CONFIG_TAB5_SSH_HOST, exit_code);

shutdown:
    libssh2_session_disconnect(session, "spike complete");
    libssh2_session_free(session);
    close(sock);
    libssh2_exit();
}
