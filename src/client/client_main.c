// -----------------------------------------------------------------------------
// doom-client - thin SDL2 front-end for a remote doom-engine.
//
// Connects to the engine over TCP, uploads a WAD, then loops:
//   - drain SDL input events; forward each as MSG_INPUT_EVENT
//   - drain network messages; render MSG_FRAME, apply MSG_PALETTE
// -----------------------------------------------------------------------------

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "client_video.h"
#include "client_input.h"
#include "wire.h"
#include "framing.h"


// Initialization-log state. Filled in by CLI parsing; consumed by do_hello
// (caps bit), the pre-READY blocking recv wrappers, and the main loop.
static int   log_enabled       = 0;     // any --log* flag set
static FILE *log_fp            = NULL;  // open iff --log-file was given
static const char *log_file_path = NULL;


static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --wad PATH [--host HOST] [--port N] [--multiply N] [--grabmouse]\n"
        "       [--skill N] [--warp E M | --warp M] [--config PATH] [--config-out PATH]\n"
        "       [--log] [--log-file PATH]\n"
        "  --wad PATH        Local WAD file to upload to the engine\n"
        "  --host HOST       Engine host (default 127.0.0.1)\n"
        "  --port N          Engine port (default %d)\n"
        "  --multiply N      Window scale factor (default 3)\n"
        "  --grabmouse       Capture the mouse for relative motion\n"
        "  --skill N         Start at skill 1..5 (1=baby, 5=nightmare)\n"
        "  --warp E M        Warp to episode E map M (DOOM 1 / Ultimate / Registered)\n"
        "  --warp M          Warp to map M (DOOM 2 / commercial)\n"
        "  --config PATH     Upload this default.cfg to the engine as the\n"
        "                    session's starting config\n"
        "  --config-out PATH Write the engine's updated default.cfg back to\n"
        "                    this path at clean shutdown\n"
        "  --log             Print engine init-phase log lines on this client's\n"
        "                    stderr (handshake through window open)\n"
        "  --log-file PATH   Same as --log, and also append each line to PATH\n"
        "                    (truncates PATH at startup; rename if you want to\n"
        "                    keep a previous run's log)\n",
        prog, DOOMNET_DEFAULT_PORT);
}


// Decode and print a MSG_LOG payload. Payload layout matches wire.h:
//   u64 monotonic_us, u8 level, u8 reserved, u16 msg_len, bytes msg.
// Output goes to stderr (always) and, if --log-file is in use, also to
// the log file. The wire side does not include a trailing newline; we
// add one when rendering.
static void handle_log_msg(const uint8_t *payload, uint32_t len)
{
    uint64_t    ts;
    uint8_t     level;
    uint16_t    mlen;
    const char *tag;
    char        line[DOOMNET_LOG_MAX_MSG + 64];
    int         n;

    if (len < DOOMNET_LOG_HEADER_BYTES) return;
    ts = (uint64_t)payload[0]
       | ((uint64_t)payload[1] << 8)
       | ((uint64_t)payload[2] << 16)
       | ((uint64_t)payload[3] << 24)
       | ((uint64_t)payload[4] << 32)
       | ((uint64_t)payload[5] << 40)
       | ((uint64_t)payload[6] << 48)
       | ((uint64_t)payload[7] << 56);
    level = payload[8];
    mlen  = (uint16_t)(payload[10] | (payload[11] << 8));
    if (DOOMNET_LOG_HEADER_BYTES + mlen > len) return;

    switch (level) {
      case DOOMNET_LOG_WARN:  tag = "WARN ";  break;
      case DOOMNET_LOG_ERROR: tag = "ERROR"; break;
      default:                tag = "INFO ";  break;
    }

    n = snprintf(line, sizeof line,
                 "[engine T+%llu.%06llus %s] %.*s\n",
                 (unsigned long long)(ts / 1000000ULL),
                 (unsigned long long)(ts % 1000000ULL),
                 tag, (int)mlen,
                 (const char *)(payload + DOOMNET_LOG_HEADER_BYTES));
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = (int)(sizeof line) - 1;

    fwrite(line, 1, (size_t)n, stderr);
    if (log_fp) {
        fwrite(line, 1, (size_t)n, log_fp);
        fflush(log_fp);
    }
}


// Wrapper around net_recv_blocking that transparently passes MSG_LOG
// messages through handle_log_msg and returns the next non-LOG message.
// Used during the pre-READY phase (HELLO_ACK, WAD_ACK, READY) where the
// engine may interleave init-log messages with the response the client
// is waiting for.
static int recv_filtered_blocking(int fd, void *buf, size_t cap,
                                  uint8_t *out_type, uint32_t *out_len)
{
    for (;;) {
        int r = net_recv_blocking(fd, buf, cap, out_type, out_len);
        if (r != 1) return r;
        if (*out_type != MSG_LOG) return r;
        handle_log_msg((const uint8_t *)buf, *out_len);
    }
}


static int tcp_connect(const char *host, int port)
{
    struct addrinfo  hints;
    struct addrinfo *res, *ai;
    char             port_str[16];
    int              fd = -1;
    int              gai;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof port_str, "%d", port);
    gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo(%s:%d): %s\n", host, port, gai_strerror(gai));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "connect %s:%d: %s\n", host, port, strerror(errno));
        return -1;
    }
    net_set_nodelay(fd);
    return fd;
}


// Extract just the file basename from a path.
static const char *basename_of(const char *path)
{
    const char *p, *base = path;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}


static int do_hello(int fd, const char *wad_path, uint32_t wad_size)
{
    const char *base = basename_of(wad_path);
    size_t      base_len = strlen(base);
    uint8_t     payload[64 + 16];
    size_t      off = 0;

    if (base_len > 60) { fprintf(stderr, "WAD basename too long\n"); return -1; }

    // u16 proto_ver
    payload[off++] = (uint8_t)DOOMNET_PROTO_VERSION;
    payload[off++] = (uint8_t)(DOOMNET_PROTO_VERSION >> 8);
    // u16 screen_w
    payload[off++] = (uint8_t)DOOMNET_SCREEN_W;
    payload[off++] = (uint8_t)(DOOMNET_SCREEN_W >> 8);
    // u16 screen_h
    payload[off++] = (uint8_t)DOOMNET_SCREEN_H;
    payload[off++] = (uint8_t)(DOOMNET_SCREEN_H >> 8);
    // u32 caps - request init log shipping iff the user asked for it.
    {
        uint32_t caps = log_enabled ? DOOMNET_CAP_INIT_LOG : 0u;
        payload[off++] = (uint8_t)(caps);
        payload[off++] = (uint8_t)(caps >> 8);
        payload[off++] = (uint8_t)(caps >> 16);
        payload[off++] = (uint8_t)(caps >> 24);
    }
    // u16 wad_name_len
    payload[off++] = (uint8_t)base_len;
    payload[off++] = (uint8_t)(base_len >> 8);
    // wad_name
    memcpy(payload + off, base, base_len);
    off += base_len;
    // u32 wad_total_size
    payload[off++] = (uint8_t)wad_size;
    payload[off++] = (uint8_t)(wad_size >> 8);
    payload[off++] = (uint8_t)(wad_size >> 16);
    payload[off++] = (uint8_t)(wad_size >> 24);

    if (net_send(fd, MSG_HELLO, payload, (uint32_t)off) < 0) {
        fprintf(stderr, "send HELLO: %s\n", strerror(errno));
        return -1;
    }

    // Expect HELLO_ACK
    {
        uint8_t  buf[DOOMNET_LOG_HEADER_BYTES + DOOMNET_LOG_MAX_MSG + 16];
        uint8_t  type;
        uint32_t len;
        int      r = recv_filtered_blocking(fd, buf, sizeof buf, &type, &len);
        if (r != 1 || type != MSG_HELLO_ACK || len < 6) {
            fprintf(stderr, "bad HELLO_ACK (r=%d type=0x%02x len=%u)\n", r, type, len);
            return -1;
        }
        {
            uint16_t proto = (uint16_t)(buf[0] | (buf[1] << 8));
            if (proto != DOOMNET_PROTO_VERSION) {
                fprintf(stderr, "engine proto mismatch: server=%u client=%u\n",
                        proto, DOOMNET_PROTO_VERSION);
                return -1;
            }
        }
    }
    return 0;
}


static int upload_wad(int fd, const char *path, uint32_t total)
{
    FILE     *fp = fopen(path, "rb");
    uint32_t  sent = 0;
    uint8_t  *buf;
    // Sized to hold the largest MSG_LOG payload that may be interleaved
    // with each WAD_ACK while init logging is enabled.
    uint8_t   ackbuf[DOOMNET_LOG_HEADER_BYTES + DOOMNET_LOG_MAX_MSG + 16];

    if (!fp) {
        fprintf(stderr, "fopen %s: %s\n", path, strerror(errno));
        return -1;
    }

    buf = (uint8_t *)malloc(DOOMNET_WAD_CHUNK + 4);
    if (!buf) { fclose(fp); return -1; }

    while (sent < total) {
        uint32_t chunk = total - sent;
        size_t   got;
        uint8_t  type;
        uint32_t len;
        int      r;

        if (chunk > DOOMNET_WAD_CHUNK) chunk = DOOMNET_WAD_CHUNK;

        // u32 offset prefix
        buf[0] = (uint8_t)sent;
        buf[1] = (uint8_t)(sent >> 8);
        buf[2] = (uint8_t)(sent >> 16);
        buf[3] = (uint8_t)(sent >> 24);

        got = fread(buf + 4, 1, chunk, fp);
        if (got != chunk) {
            fprintf(stderr, "wad read short: %zu of %u at %u\n", got, chunk, sent);
            free(buf); fclose(fp); return -1;
        }
        if (net_send(fd, MSG_WAD_CHUNK, buf, 4 + chunk) < 0) {
            fprintf(stderr, "send WAD_CHUNK: %s\n", strerror(errno));
            free(buf); fclose(fp); return -1;
        }
        // Wait for ACK before next chunk (simple flow control).
        r = recv_filtered_blocking(fd, ackbuf, sizeof ackbuf, &type, &len);
        if (r != 1 || type != MSG_WAD_ACK || len < 4) {
            fprintf(stderr, "bad WAD_ACK\n");
            free(buf); fclose(fp); return -1;
        }
        sent += chunk;
        fprintf(stderr, "\r[client] uploading %s: %u / %u",
                basename_of(path), (unsigned)sent, (unsigned)total);
        fflush(stderr);
    }
    fprintf(stderr, "\n");
    free(buf);
    fclose(fp);

    // WAD_DONE: u32 total_size
    {
        uint8_t done[4];
        done[0] = (uint8_t)total;
        done[1] = (uint8_t)(total >> 8);
        done[2] = (uint8_t)(total >> 16);
        done[3] = (uint8_t)(total >> 24);
        if (net_send(fd, MSG_WAD_DONE, done, sizeof done) < 0) {
            fprintf(stderr, "send WAD_DONE: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}


// Send -warp / -skill CLI flags to the engine as a MSG_ARGS message. Each arg
// becomes a separate string the engine appends to its argv, so the unchanged
// D_DoomMain code that already looks for "-warp" / "-skill" via M_CheckParm
// picks them up. No-op (returns 0 without sending anything) when neither
// flag was supplied on the client command line.
static int send_args(int fd,
                     const char *skill,
                     const char *warp_a,
                     const char *warp_b)
{
    uint8_t  buf[256];
    size_t   off = 2;          // reserve space for u16 argc at [0..1]
    uint16_t argc = 0;

    // Inline helper: append a length-prefixed string to buf and bump argc.
    // We use a do/while so the macro is a single statement.
    #define APPEND_STR(s) do {                                  \
        size_t _len = strlen(s);                                \
        if (_len > 60 || off + 2 + _len > sizeof buf) {         \
            fprintf(stderr, "send_args: arg too long\n");       \
            return -1;                                          \
        }                                                       \
        buf[off++] = (uint8_t)_len;                             \
        buf[off++] = (uint8_t)(_len >> 8);                      \
        memcpy(buf + off, (s), _len);                           \
        off += _len;                                            \
        argc++;                                                 \
    } while (0)

    if (skill) {
        APPEND_STR("-skill");
        APPEND_STR(skill);
    }
    if (warp_a) {
        APPEND_STR("-warp");
        APPEND_STR(warp_a);
        if (warp_b) APPEND_STR(warp_b);
    }

    #undef APPEND_STR

    if (argc == 0) return 0;

    buf[0] = (uint8_t)argc;
    buf[1] = (uint8_t)(argc >> 8);

    if (net_send(fd, MSG_ARGS, buf, (uint32_t)off) < 0) {
        fprintf(stderr, "send MSG_ARGS: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}


// Read the local file at `path` and ship it to the engine as MSG_CONFIG.
// Sent after MSG_ARGS and before WAD upload; the engine writes it to the
// per-session temp dir and threads -config <path> through to M_LoadDefaults.
// Returns 0 on success (or if `path` is NULL), -1 on failure.
static int send_config(int fd, const char *path)
{
    FILE    *fp;
    uint8_t *buf;
    long     sz;

    if (!path) return 0;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "fopen %s: %s\n", path, strerror(errno));
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0 || (uint32_t)sz > DOOMNET_CONFIG_MAX) {
        fprintf(stderr, "config %s: bad size %ld\n", path, sz);
        fclose(fp); return -1;
    }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fprintf(stderr, "config %s: short read\n", path);
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);

    if (net_send(fd, MSG_CONFIG, buf, (uint32_t)sz) < 0) {
        fprintf(stderr, "send MSG_CONFIG: %s\n", strerror(errno));
        free(buf); return -1;
    }
    free(buf);
    fprintf(stderr, "[client] uploaded config %s (%ld bytes)\n", path, sz);
    return 0;
}

static int await_ready(int fd)
{
    uint8_t  buf[DOOMNET_LOG_HEADER_BYTES + DOOMNET_LOG_MAX_MSG + 16];
    uint8_t  type;
    uint32_t len;
    int      r = recv_filtered_blocking(fd, buf, sizeof buf, &type, &len);
    if (r != 1 || type != MSG_READY) {
        fprintf(stderr, "expected READY, got type=0x%02x r=%d\n", type, r);
        return -1;
    }
    return 0;
}


int main(int argc, char **argv)
{
    const char *host       = "127.0.0.1";
    int         port       = DOOMNET_DEFAULT_PORT;
    const char *wad_path   = NULL;
    int         multiply   = 3;
    int         grab_mouse = 0;
    const char *skill      = NULL;     // user-supplied "-skill" value, or NULL
    const char *warp_a     = NULL;     // first "-warp" value (map or episode)
    const char *warp_b     = NULL;     // optional second "-warp" value (map)
    const char *config_in  = NULL;     // optional default.cfg to upload
    const char *config_out = NULL;     // optional path to write engine's
                                       // updated default.cfg back to
    int         fd;
    struct stat st;
    uint32_t    wad_size;
    int         i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) { host = argv[++i]; }
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) { port = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--wad") && i + 1 < argc) { wad_path = argv[++i]; }
        else if (!strcmp(argv[i], "--multiply") && i + 1 < argc) { multiply = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--grabmouse")) { grab_mouse = 1; }
        else if (!strcmp(argv[i], "--skill") && i + 1 < argc) { skill = argv[++i]; }
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) { config_in = argv[++i]; }
        else if (!strcmp(argv[i], "--config-out") && i + 1 < argc) { config_out = argv[++i]; }
        else if (!strcmp(argv[i], "--log")) { log_enabled = 1; }
        else if (!strcmp(argv[i], "--log-file") && i + 1 < argc) {
            log_enabled = 1;
            log_file_path = argv[++i];
        }
        else if (!strcmp(argv[i], "--warp")  && i + 1 < argc) {
            // Accept either "--warp M" (commercial) or "--warp E M" (others).
            // The engine decides which form is valid based on its gamemode;
            // we just forward the values verbatim.
            warp_a = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                warp_b = argv[++i];
            }
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    if (!wad_path) { usage(argv[0]); return 1; }
    if (stat(wad_path, &st) < 0) {
        fprintf(stderr, "stat %s: %s\n", wad_path, strerror(errno));
        return 1;
    }
    if (st.st_size <= 0 || st.st_size > (off_t)0xFFFFFFFF) {
        fprintf(stderr, "WAD size invalid\n");
        return 1;
    }
    wad_size = (uint32_t)st.st_size;

    if (log_file_path) {
        // Truncate any previous log; the user is responsible for renaming
        // a previous run's file if they want to keep it.
        log_fp = fopen(log_file_path, "w");
        if (!log_fp) {
            fprintf(stderr, "fopen %s: %s\n", log_file_path, strerror(errno));
            return 1;
        }
    }

    fprintf(stderr, "[client] connecting to %s:%d\n", host, port);
    fd = tcp_connect(host, port);
    if (fd < 0) { if (log_fp) fclose(log_fp); return 1; }

    if (do_hello(fd, wad_path, wad_size) < 0)            { close(fd); return 1; }
    if (send_args(fd, skill, warp_a, warp_b) < 0)        { close(fd); return 1; }
    if (send_config(fd, config_in) < 0)                  { close(fd); return 1; }
    if (upload_wad(fd, wad_path, wad_size) < 0)          { close(fd); return 1; }
    if (await_ready(fd) < 0)                             { close(fd); return 1; }
    fprintf(stderr, "[client] engine ready, opening window\n");

    if (client_video_init(DOOMNET_SCREEN_W, DOOMNET_SCREEN_H, multiply, grab_mouse) < 0) {
        close(fd); return 1;
    }

    if (net_set_nonblocking(fd) < 0) {
        fprintf(stderr, "set non-blocking: %s\n", strerror(errno));
        client_video_shutdown(); close(fd); return 1;
    }

    {
        // RX buffer big enough for header + one full frame.
        static uint8_t rx_storage[DOOMNET_HEADER_BYTES + DOOMNET_FRAME_BYTES + 64];
        net_rx_t       rx = { rx_storage, sizeof rx_storage, 0 };
        int            running = 1;

        while (running) {
            // 1. SDL input -> wire
            if (client_input_poll(fd) < 0) { running = 0; break; }

            // 2. wire -> render. Process at most one full frame per outer
            // iteration so SDL polling stays responsive even if frames are
            // backed up in the kernel buffer.
            {
                int rendered = 0;
                while (!rendered) {
                    uint8_t        type;
                    uint32_t       len;
                    const uint8_t *payload;
                    int            r = net_try_recv(fd, &rx, &type, &len, &payload);
                    if (r == 0) break;
                    if (r < 0) {
                        fprintf(stderr, "[client] engine disconnected\n");
                        running = 0; break;
                    }
                    switch (type) {
                      case MSG_PALETTE:
                        if (len >= DOOMNET_PALETTE_BYTES)
                            client_video_set_palette(payload);
                        break;
                      case MSG_FRAME:
                        if (len >= 1 + DOOMNET_FRAME_BYTES) {
                            client_video_present(payload + 1);
                            rendered = 1;
                        }
                        break;
                      case MSG_BYE_S:
                        fprintf(stderr, "[client] engine said BYE\n");
                        running = 0; break;
                      case MSG_CONFIG_OUT:
                        if (config_out) {
                            FILE *fp = fopen(config_out, "wb");
                            if (!fp) {
                                fprintf(stderr,
                                    "[client] fopen %s: %s\n",
                                    config_out, strerror(errno));
                            } else {
                                if (len && fwrite(payload, 1, len, fp) != len)
                                    fprintf(stderr,
                                        "[client] write %s: short\n", config_out);
                                fclose(fp);
                                fprintf(stderr,
                                    "[client] saved config to %s (%u bytes)\n",
                                    config_out, (unsigned)len);
                            }
                        }
                        break;
                      case MSG_PONG:
                        /* not yet used */
                        break;
                      case MSG_LOG:
                        // Should not arrive after MSG_READY (the engine
                        // disables wire-side logging at I_InitGraphics),
                        // but render it defensively if the engine emits
                        // one anyway.
                        handle_log_msg(payload, len);
                        break;
                      default:
                        break;
                    }
                    if (!running) break;
                }
            }
            if (!running) break;

            // 3. brief wait for either socket data or SDL events.
            {
                struct pollfd p;
                p.fd = fd; p.events = POLLIN;
                poll(&p, 1, 2);
            }
        }
    }

    client_video_shutdown();
    close(fd);
    if (log_fp) { fclose(log_fp); log_fp = NULL; }
    return 0;
}
