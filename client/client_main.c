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
#include "../net/wire.h"
#include "../net/framing.h"


static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --wad PATH [--host HOST] [--port N] [--multiply N] [--grabmouse]\n"
        "  --wad PATH      Local WAD file to upload to the engine\n"
        "  --host HOST     Engine host (default 127.0.0.1)\n"
        "  --port N        Engine port (default %d)\n"
        "  --multiply N    Window scale factor (default 3)\n"
        "  --grabmouse     Capture the mouse for relative motion\n",
        prog, DOOMNET_DEFAULT_PORT);
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
    // u32 caps = 0
    payload[off++] = 0; payload[off++] = 0; payload[off++] = 0; payload[off++] = 0;
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
        uint8_t  buf[64];
        uint8_t  type;
        uint32_t len;
        int      r = net_recv_blocking(fd, buf, sizeof buf, &type, &len);
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
    uint8_t   ackbuf[64];

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
        r = net_recv_blocking(fd, ackbuf, sizeof ackbuf, &type, &len);
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


static int await_ready(int fd)
{
    uint8_t  buf[64];
    uint8_t  type;
    uint32_t len;
    int      r = net_recv_blocking(fd, buf, sizeof buf, &type, &len);
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

    fprintf(stderr, "[client] connecting to %s:%d\n", host, port);
    fd = tcp_connect(host, port);
    if (fd < 0) return 1;

    if (do_hello(fd, wad_path, wad_size) < 0) { close(fd); return 1; }
    if (upload_wad(fd, wad_path, wad_size) < 0) { close(fd); return 1; }
    if (await_ready(fd) < 0)                  { close(fd); return 1; }
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

            // 2. wire -> render
            for (;;) {
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
                    if (len >= 1 + DOOMNET_FRAME_BYTES)
                        client_video_present(payload + 1);
                    break;
                  case MSG_BYE_S:
                    fprintf(stderr, "[client] engine said BYE\n");
                    running = 0; break;
                  case MSG_PONG:
                    /* not yet used */
                    break;
                  default:
                    break;
                }
                if (!running) break;
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
    return 0;
}
