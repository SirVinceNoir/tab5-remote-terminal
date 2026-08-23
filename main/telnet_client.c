#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include "esp_log.h"
#include "telnet_client.h"

static const char *TAG = "telnet_client";

#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240

#define OPT_ECHO      1
#define OPT_SGA       3
#define OPT_TERM_TYPE 24
#define OPT_NAWS      31

typedef enum { TN_STATE_DATA, TN_STATE_IAC, TN_STATE_CMD_OPT, TN_STATE_SB, TN_STATE_SB_IAC } tn_state_t;

struct telnet_client {
    int sock;

    tn_state_t state;
    uint8_t pending_cmd;

    uint8_t sb_opt;
    bool sb_have_opt;
    uint8_t sb_buf[16];
    int sb_len;

    bool naws_enabled;
    uint16_t naws_cols, naws_rows;
};

static void tn_send_cmd(telnet_client_t *tc, uint8_t cmd, uint8_t opt)
{
    uint8_t buf[3] = { IAC, cmd, opt };
    send(tc->sock, buf, sizeof(buf), 0);
}

static void tn_send_naws(telnet_client_t *tc)
{
    uint8_t buf[9] = {
        IAC, SB, OPT_NAWS,
        (uint8_t)(tc->naws_cols >> 8), (uint8_t)(tc->naws_cols & 0xFF),
        (uint8_t)(tc->naws_rows >> 8), (uint8_t)(tc->naws_rows & 0xFF),
        IAC, SE,
    };
    send(tc->sock, buf, sizeof(buf), 0);
}

static void tn_handle_subneg(telnet_client_t *tc)
{
    if (tc->sb_opt == OPT_TERM_TYPE && tc->sb_len >= 1 && tc->sb_buf[0] == 1 /* SEND */) {
        static const char term[] = "xterm";
        uint8_t hdr[4] = { IAC, SB, OPT_TERM_TYPE, 0 /* IS */ };
        uint8_t trailer[2] = { IAC, SE };
        send(tc->sock, hdr, sizeof(hdr), 0);
        send(tc->sock, term, strlen(term), 0);
        send(tc->sock, trailer, sizeof(trailer), 0);
    }
}

static void tn_handle_will(telnet_client_t *tc, uint8_t opt)
{
    tn_send_cmd(tc, (opt == OPT_ECHO || opt == OPT_SGA) ? DO : DONT, opt);
}

static void tn_handle_wont(telnet_client_t *tc, uint8_t opt)
{
    tn_send_cmd(tc, DONT, opt);
}

static void tn_handle_do(telnet_client_t *tc, uint8_t opt)
{
    if (opt == OPT_SGA || opt == OPT_TERM_TYPE) {
        tn_send_cmd(tc, WILL, opt);
    } else if (opt == OPT_NAWS) {
        tn_send_cmd(tc, WILL, opt);
        tc->naws_enabled = true;
        if (tc->naws_cols != 0 && tc->naws_rows != 0) {
            tn_send_naws(tc);
        }
    } else {
        tn_send_cmd(tc, WONT, opt);
    }
}

static void tn_handle_dont(telnet_client_t *tc, uint8_t opt)
{
    tn_send_cmd(tc, WONT, opt);
}

/* Feeds one raw byte off the wire through the telnet protocol state
 * machine. Returns true and fills *out if this byte is real session data
 * the caller should keep; returns false if it was protocol overhead
 * (consumed here, possibly triggering an inline reply). */
static bool tn_process_byte(telnet_client_t *tc, uint8_t c, uint8_t *out)
{
    switch (tc->state) {
    case TN_STATE_DATA:
        if (c == IAC) {
            tc->state = TN_STATE_IAC;
            return false;
        }
        *out = c;
        return true;

    case TN_STATE_IAC:
        if (c == IAC) {
            tc->state = TN_STATE_DATA;
            *out = 0xFF; /* escaped literal 0xFF in the data stream */
            return true;
        }
        if (c == WILL || c == WONT || c == DO || c == DONT) {
            tc->pending_cmd = c;
            tc->state = TN_STATE_CMD_OPT;
            return false;
        }
        if (c == SB) {
            tc->sb_len = 0;
            tc->sb_have_opt = false;
            tc->state = TN_STATE_SB;
            return false;
        }
        tc->state = TN_STATE_DATA; /* NOP, AYT, GA, etc. -- no option byte follows */
        return false;

    case TN_STATE_CMD_OPT:
        tc->state = TN_STATE_DATA;
        switch (tc->pending_cmd) {
        case WILL: tn_handle_will(tc, c); break;
        case WONT: tn_handle_wont(tc, c); break;
        case DO:   tn_handle_do(tc, c);   break;
        case DONT: tn_handle_dont(tc, c); break;
        default: break;
        }
        return false;

    case TN_STATE_SB:
        if (!tc->sb_have_opt) {
            tc->sb_opt = c;
            tc->sb_have_opt = true;
            return false;
        }
        if (c == IAC) {
            tc->state = TN_STATE_SB_IAC;
            return false;
        }
        if (tc->sb_len < (int)sizeof(tc->sb_buf)) {
            tc->sb_buf[tc->sb_len++] = c;
        }
        return false;

    case TN_STATE_SB_IAC:
        if (c == SE) {
            tn_handle_subneg(tc);
            tc->state = TN_STATE_DATA;
        } else if (c == IAC) {
            if (tc->sb_len < (int)sizeof(tc->sb_buf)) {
                tc->sb_buf[tc->sb_len++] = 0xFF;
            }
            tc->state = TN_STATE_SB;
        } else {
            tc->state = TN_STATE_DATA; /* malformed -- bail back to data mode */
        }
        return false;
    }
    return false;
}

telnet_client_t *telnet_client_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
    telnet_client_t *tc = calloc(1, sizeof(*tc));
    if (tc == NULL) {
        return NULL;
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int gai_ret = getaddrinfo(host, port_str, &hints, &res);
    if (gai_ret != 0 || res == NULL) {
        ESP_LOGE(TAG, "getaddrinfo('%s') failed: %d", host, gai_ret);
        free(tc);
        return NULL;
    }

    tc->sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (tc->sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        freeaddrinfo(res);
        free(tc);
        return NULL;
    }

    int flags = fcntl(tc->sock, F_GETFL, 0);
    fcntl(tc->sock, F_SETFL, flags | O_NONBLOCK);

    int conn_ret = connect(tc->sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (conn_ret < 0 && errno != EINPROGRESS) {
        ESP_LOGE(TAG, "connect() failed: errno %d", errno);
        close(tc->sock);
        free(tc);
        return NULL;
    }

    if (conn_ret < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(tc->sock, &wfds);
        struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        int sel = select(tc->sock + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            ESP_LOGE(TAG, "connect() to %s:%u timed out", host, port);
            close(tc->sock);
            free(tc);
            return NULL;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(tc->sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            ESP_LOGE(TAG, "connect() to %s:%u failed: errno %d", host, port, so_error);
            close(tc->sock);
            free(tc);
            return NULL;
        }
    }

    /* telnet_client_read() drives its own per-call timeout via select();
     * it wants a blocking recv() once select() says data is ready. */
    fcntl(tc->sock, F_SETFL, flags);

    tc->state = TN_STATE_DATA;
    ESP_LOGI(TAG, "connected to %s:%u", host, port);
    return tc;
}

int telnet_client_read(telnet_client_t *tc, uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(tc->sock, &rfds);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };

    int sel = select(tc->sock + 1, &rfds, NULL, NULL, &tv);
    if (sel < 0) {
        return -1;
    }
    if (sel == 0) {
        return 0;
    }

    uint8_t raw[512];
    size_t raw_max = max_len < sizeof(raw) ? max_len : sizeof(raw);
    int n = recv(tc->sock, raw, raw_max, 0);
    if (n <= 0) {
        return -1;
    }

    size_t out_len = 0;
    for (int i = 0; i < n; i++) {
        uint8_t decoded;
        if (tn_process_byte(tc, raw[i], &decoded)) {
            buf[out_len++] = decoded;
        }
    }
    return (int)out_len;
}

bool telnet_client_write(telnet_client_t *tc, const uint8_t *data, size_t len)
{
    uint8_t buf[256];
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (out >= sizeof(buf) - 2) {
            if (send(tc->sock, buf, out, 0) < 0) {
                return false;
            }
            out = 0;
        }
        buf[out++] = data[i];
        if (data[i] == 0xFF) {
            buf[out++] = 0xFF; /* byte-stuff a literal IAC in outbound data */
        }
    }
    if (out > 0 && send(tc->sock, buf, out, 0) < 0) {
        return false;
    }
    return true;
}

void telnet_client_send_naws(telnet_client_t *tc, uint16_t cols, uint16_t rows)
{
    tc->naws_cols = cols;
    tc->naws_rows = rows;
    if (tc->naws_enabled) {
        tn_send_naws(tc);
    }
}

void telnet_client_close(telnet_client_t *tc)
{
    if (tc == NULL) {
        return;
    }
    close(tc->sock);
    free(tc);
}

static int telnet_transport_read(void *ctx, uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    return telnet_client_read((telnet_client_t *)ctx, buf, max_len, timeout_ms);
}

static bool telnet_transport_write(void *ctx, const uint8_t *data, size_t len)
{
    return telnet_client_write((telnet_client_t *)ctx, data, len);
}

static void telnet_transport_close(void *ctx)
{
    telnet_client_close((telnet_client_t *)ctx);
}

session_transport_t telnet_client_as_transport(telnet_client_t *tc)
{
    return (session_transport_t){
        .ctx = tc,
        .read = telnet_transport_read,
        .write = telnet_transport_write,
        .close = telnet_transport_close,
    };
}
