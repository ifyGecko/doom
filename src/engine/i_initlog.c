// -----------------------------------------------------------------------------
// Engine-side initialization logger - implementation.
// See i_initlog.h for the contract.
// -----------------------------------------------------------------------------

#include "i_initlog.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "wire.h"
#include "framing.h"


// ---- session state ---------------------------------------------------------

static int       g_client_fd = -1;          // -1 until L_InitlogSetup
static int       g_wire_enabled = 0;        // gated by HELLO.caps + finish
static int       g_finished = 0;            // L_InitlogFinish already ran
static uint64_t  g_session_start_us = 0;    // CLOCK_MONOTONIC at setup


static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}


// ---- public lifecycle ------------------------------------------------------

void L_InitlogSetup(int client_fd)
{
    if (g_session_start_us != 0) return;    // idempotent
    g_client_fd        = client_fd;
    g_session_start_us = now_us();
    g_wire_enabled     = 0;
    g_finished         = 0;
}

void L_InitlogEnable(uint32_t caps)
{
    if (g_finished) return;
    if (g_client_fd < 0) return;
    if (caps & DOOMNET_CAP_INIT_LOG) g_wire_enabled = 1;
}

void L_InitlogFinish(void)
{
    if (g_finished) return;
    L_Infof("[init complete]");
    g_wire_enabled = 0;
    g_finished     = 1;
}

void L_InitlogFlush(void)
{
    fflush(stdout);
    fflush(stderr);
}


// ---- core emit -------------------------------------------------------------

// Trim leading and trailing ASCII whitespace (spaces, tabs, CR, LF) from
// a message destined for the wire. The local terminal output deliberately
// keeps the original formatting - the 1996 boot prints lean on indented
// sub-lines ("V_Init: ...", " adding foo.wad", etc.) for readability on
// the engine host - but on the client side the per-line "[engine T+...
// LEVEL] " prefix supplies its own visual structure, and the inherited
// indents just look misaligned. Trimming centralizes the cleanup so
// every L_* call site and any future addition gets it for free.
static void trim_for_wire(const char **msg, size_t *msg_len)
{
    const char *s = *msg;
    size_t      len = *msg_len;
    while (len > 0 &&
           (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++; len--;
    }
    while (len > 0 &&
           (s[len - 1] == ' ' || s[len - 1] == '\t'
            || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        len--;
    }
    *msg     = s;
    *msg_len = len;
}

static void send_log_line(uint8_t level, const char *msg, size_t msg_len)
{
    uint8_t  payload[DOOMNET_LOG_HEADER_BYTES + DOOMNET_LOG_MAX_MSG];
    uint64_t ts;
    uint16_t mlen;
    int      truncated = 0;

    if (!g_wire_enabled || g_client_fd < 0) return;

    trim_for_wire(&msg, &msg_len);
    if (msg_len == 0) return;
    if (msg_len > DOOMNET_LOG_MAX_MSG) {
        msg_len   = DOOMNET_LOG_MAX_MSG;
        truncated = 1;
    }
    mlen = (uint16_t)msg_len;
    if (truncated && mlen >= 3) {
        // Overwrite the last 3 bytes with "..." to make truncation visible.
        // We then copy msg up to mlen-3 below and patch the tail in payload.
    }

    ts = now_us() - g_session_start_us;
    payload[0] = (uint8_t)(ts);
    payload[1] = (uint8_t)(ts >> 8);
    payload[2] = (uint8_t)(ts >> 16);
    payload[3] = (uint8_t)(ts >> 24);
    payload[4] = (uint8_t)(ts >> 32);
    payload[5] = (uint8_t)(ts >> 40);
    payload[6] = (uint8_t)(ts >> 48);
    payload[7] = (uint8_t)(ts >> 56);
    payload[8] = level;
    payload[9] = 0;
    payload[10] = (uint8_t)(mlen);
    payload[11] = (uint8_t)(mlen >> 8);

    memcpy(payload + DOOMNET_LOG_HEADER_BYTES, msg, mlen);
    if (truncated && mlen >= 3) {
        payload[DOOMNET_LOG_HEADER_BYTES + mlen - 3] = '.';
        payload[DOOMNET_LOG_HEADER_BYTES + mlen - 2] = '.';
        payload[DOOMNET_LOG_HEADER_BYTES + mlen - 1] = '.';
    }

    // Best-effort send. A failed write on the log channel must never
    // abort the session, so we just disable further wire-side shipping
    // if the connection looks broken.
    if (net_send(g_client_fd, MSG_LOG, payload,
                 (uint32_t)(DOOMNET_LOG_HEADER_BYTES + mlen)) < 0) {
        g_wire_enabled = 0;
    }
}

static void emit(uint8_t level, FILE *stream, const char *fmt, va_list ap)
{
    char    buf[DOOMNET_LOG_MAX_MSG + 1];
    int     n;
    va_list ap_local;

    va_copy(ap_local, ap);
    n = vsnprintf(buf, sizeof buf, fmt, ap_local);
    va_end(ap_local);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)(sizeof buf) - 1;

    // Local terminal: preserve the original printf semantics. The
    // existing engine code does NOT prefix levels, so for INFO we keep
    // that behavior; WARN/ERROR get a tagged prefix for terminal use.
    if (level == DOOMNET_LOG_INFO) {
        fputs(buf, stream);
    } else {
        fprintf(stream, "%s: %.*s",
                level == DOOMNET_LOG_WARN ? "warning" : "error",
                n, buf);
    }
    // Match printf default behavior: ensure the local stream sees a
    // newline if the caller did not include one.
    if (n == 0 || buf[n - 1] != '\n') fputc('\n', stream);

    send_log_line(level, buf, (size_t)n);
}


// ---- public emit ----------------------------------------------------------

void L_Infof(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(DOOMNET_LOG_INFO, stdout, fmt, ap);
    va_end(ap);
}

void L_Warnf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(DOOMNET_LOG_WARN, stderr, fmt, ap);
    va_end(ap);
}

void L_Errorf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(DOOMNET_LOG_ERROR, stderr, fmt, ap);
    va_end(ap);
}
