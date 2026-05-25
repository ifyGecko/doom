// -----------------------------------------------------------------------------
// DOOM engine "video" backend that talks to a remote client over TCP.
//
// Replaces the SDL2 implementation in i_video.c. The engine sees the same
// I_InitGraphics / I_FinishUpdate / I_SetPalette / I_StartTic / etc. API,
// but:
//
//   - I_NetBootstrap (called before D_DoomMain) opens a listening socket,
//     accepts one client, performs the HELLO handshake, receives the WAD
//     into a fresh temp directory, and sets DOOMWADDIR so the existing
//     IdentifyVersion() code finds it.
//
//   - I_InitGraphics simply marks the engine ready and tells the client
//     frames are about to flow.
//
//   - I_FinishUpdate serializes screens[0] and any pending palette change
//     and pushes them to the wire.
//
//   - I_StartTic non-blocking-drains queued input events from the client
//     and re-injects them via D_PostEvent.
//
//   - I_ShutdownGraphics sends a polite BYE and closes the socket.
//
// One engine process : one client connection. When the client disconnects
// the engine quits.
// -----------------------------------------------------------------------------

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"
#include "doomdef.h"

#include "../net/wire.h"
#include "../net/framing.h"


// ---- session state ---------------------------------------------------------

static int      client_fd     = -1;     // accepted client socket
static int      initialized   = 0;      // set by I_InitGraphics

// Engine-side palette, post-gamma, in B,G,R,A order ready to ship.
static uint8_t  cur_palette[DOOMNET_PALETTE_BYTES];
static int      palette_dirty = 1;

// Non-blocking RX buffer for input events during the game loop.
// Sized generously - a single message can be at most ~17 bytes (INPUT_EVENT).
static uint8_t  rx_storage[8 * 1024];
static net_rx_t rx = { rx_storage, sizeof rx_storage, 0 };

// Temp directory & WAD path used for cleanup at exit.
static char     temp_dir[256]  = {0};
static char     temp_wad[512]  = {0};


// ---- helpers ---------------------------------------------------------------

static int parse_int_arg(const char *flag, int fallback)
{
    int p = M_CheckParm((char *)flag);
    if (p > 0 && p < myargc - 1)
        return atoi(myargv[p + 1]);
    return fallback;
}

static const char *parse_str_arg(const char *flag, const char *fallback)
{
    int p = M_CheckParm((char *)flag);
    if (p > 0 && p < myargc - 1)
        return myargv[p + 1];
    return fallback;
}

static void cleanup_temp(void)
{
    if (temp_wad[0]) { unlink(temp_wad); temp_wad[0] = 0; }
    if (temp_dir[0]) { rmdir(temp_dir);  temp_dir[0] = 0; }
}

// strip "/foo/bar/" from a name; clamp to alnum + '.' + '_' + '-' so
// a malicious client can't escape the temp dir via path traversal.
//
// `in` is NOT required to be null-terminated; `in_len` is authoritative.
static int sanitize_basename(const char *in, size_t in_len,
                             char *out, size_t cap)
{
    size_t start = 0;
    size_t i;

    for (i = 0; i < in_len; i++) {
        if (in[i] == '/' || in[i] == '\\') start = i + 1;
    }
    if (start >= in_len) return -1;
    if (in_len - start + 1 > cap) return -1;
    for (i = start; i < in_len; i++) {
        char c = in[i];
        int ok = (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') ||
                 c == '.' || c == '_' || c == '-';
        if (!ok) return -1;
        out[i - start] = c;
    }
    out[in_len - start] = 0;
    return 0;
}

static void die(const char *what)
{
    fprintf(stderr, "I_NetBootstrap: %s: %s\n", what, strerror(errno));
    cleanup_temp();
    exit(1);
}


// ---- bootstrap (runs BEFORE D_DoomMain) -----------------------------------

void I_NetBootstrap(void)
{
    int                 listen_fd;
    int                 port;
    int                 one = 1;
    const char         *bind_addr;
    struct sockaddr_in  addr;
    socklen_t           addr_len;
    uint8_t             buf[DOOMNET_WAD_CHUNK + 64];
    uint8_t             type;
    uint32_t            len;
    int                 r;

    // A broken connection should surface as a send() error, not a fatal
    // signal that prevents I_Quit from running.
    signal(SIGPIPE, SIG_IGN);

    port      = parse_int_arg("-port", DOOMNET_DEFAULT_PORT);
    bind_addr = parse_str_arg("-bind", "0.0.0.0");

    // Socket setup ----------------------------------------------------------
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket");
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "I_NetBootstrap: bad -bind address '%s'\n", bind_addr);
        exit(1);
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) die("bind");
    if (listen(listen_fd, 1) < 0) die("listen");

    fprintf(stderr, "[engine] waiting for client on %s:%d ...\n", bind_addr, port);

    addr_len = sizeof addr;
    client_fd = accept(listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (client_fd < 0) die("accept");
    close(listen_fd);

    fprintf(stderr, "[engine] client connected from %s:%d\n",
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    net_set_nodelay(client_fd);

    // HELLO -----------------------------------------------------------------
    r = net_recv_blocking(client_fd, buf, sizeof buf, &type, &len);
    if (r != 1 || type != MSG_HELLO || len < 12) {
        fprintf(stderr, "[engine] bad HELLO\n");
        cleanup_temp();
        exit(1);
    }
    {
        uint16_t proto    = (uint16_t)(buf[0] | (buf[1] << 8));
        uint16_t scr_w    = (uint16_t)(buf[2] | (buf[3] << 8));
        uint16_t scr_h    = (uint16_t)(buf[4] | (buf[5] << 8));
        // u32 caps at offset 6 - reserved
        uint16_t wad_nlen = (uint16_t)(buf[10] | (buf[11] << 8));
        uint32_t wad_size;
        char     wad_basename[64];

        if (proto != DOOMNET_PROTO_VERSION) {
            fprintf(stderr, "[engine] proto mismatch: client=%u server=%u\n",
                    proto, DOOMNET_PROTO_VERSION);
            exit(1);
        }
        if (scr_w != DOOMNET_SCREEN_W || scr_h != DOOMNET_SCREEN_H) {
            fprintf(stderr, "[engine] screen size mismatch: client=%ux%u\n",
                    scr_w, scr_h);
            exit(1);
        }
        if (12 + (uint32_t)wad_nlen + 4 != len) {
            fprintf(stderr, "[engine] malformed HELLO payload\n");
            exit(1);
        }
        if (sanitize_basename((char *)buf + 12, wad_nlen,
                              wad_basename, sizeof wad_basename) < 0
            || strstr(wad_basename, ".wad") == NULL) {
            fprintf(stderr, "[engine] bad wad_name in HELLO\n");
            exit(1);
        }
        {
            const uint8_t *p = buf + 12 + wad_nlen;
            wad_size = (uint32_t)p[0]
                     | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16)
                     | ((uint32_t)p[3] << 24);
        }

        // HELLO_ACK
        {
            uint8_t ack[8];
            ack[0] = (uint8_t)(DOOMNET_PROTO_VERSION);
            ack[1] = (uint8_t)(DOOMNET_PROTO_VERSION >> 8);
            ack[2] = (uint8_t)(DOOMNET_SCREEN_W);
            ack[3] = (uint8_t)(DOOMNET_SCREEN_W >> 8);
            ack[4] = (uint8_t)(DOOMNET_SCREEN_H);
            ack[5] = (uint8_t)(DOOMNET_SCREEN_H >> 8);
            ack[6] = 0; ack[7] = 0;
            if (net_send(client_fd, MSG_HELLO_ACK, ack, 6) < 0) die("send HELLO_ACK");
        }

        // Temp dir + open output file ---------------------------------------
        snprintf(temp_dir, sizeof temp_dir, "/tmp/doom-net-%d", (int)getpid());
        if (mkdir(temp_dir, 0700) < 0 && errno != EEXIST) die("mkdir tmpdir");
        snprintf(temp_wad, sizeof temp_wad, "%s/%s", temp_dir, wad_basename);
        atexit(cleanup_temp);

        {
            FILE    *fp = fopen(temp_wad, "wb");
            uint32_t received = 0;

            if (!fp) die("fopen tmp wad");

            // WAD upload loop -----------------------------------------------
            fprintf(stderr, "[engine] receiving %s (%u bytes) ...\n",
                    wad_basename, (unsigned)wad_size);

            while (received < wad_size) {
                r = net_recv_blocking(client_fd, buf, sizeof buf, &type, &len);
                if (r != 1) { fprintf(stderr, "[engine] WAD upload aborted\n"); exit(1); }
                if (type == MSG_WAD_DONE) {
                    // Allow client to signal completion early; we'll validate below.
                    break;
                }
                if (type != MSG_WAD_CHUNK || len < 4) {
                    fprintf(stderr, "[engine] unexpected msg 0x%02x during upload\n", type);
                    exit(1);
                }
                {
                    uint32_t offset = (uint32_t)buf[0]
                                    | ((uint32_t)buf[1] << 8)
                                    | ((uint32_t)buf[2] << 16)
                                    | ((uint32_t)buf[3] << 24);
                    uint32_t chunk  = len - 4;
                    uint8_t  ackbuf[4];
                    if (offset != received) {
                        fprintf(stderr, "[engine] chunk offset %u expected %u\n",
                                (unsigned)offset, (unsigned)received);
                        exit(1);
                    }
                    if (received + chunk > wad_size) {
                        fprintf(stderr, "[engine] WAD overrun\n"); exit(1);
                    }
                    if (fwrite(buf + 4, 1, chunk, fp) != chunk) die("fwrite wad");
                    received += chunk;
                    ackbuf[0] = (uint8_t)(received);
                    ackbuf[1] = (uint8_t)(received >> 8);
                    ackbuf[2] = (uint8_t)(received >> 16);
                    ackbuf[3] = (uint8_t)(received >> 24);
                    if (net_send(client_fd, MSG_WAD_ACK, ackbuf, 4) < 0) die("send WAD_ACK");
                }
            }

            // After data, expect MSG_WAD_DONE if not already consumed.
            if (type != MSG_WAD_DONE) {
                r = net_recv_blocking(client_fd, buf, sizeof buf, &type, &len);
                if (r != 1 || type != MSG_WAD_DONE) {
                    fprintf(stderr, "[engine] expected WAD_DONE\n"); exit(1);
                }
            }
            if (received != wad_size) {
                fprintf(stderr, "[engine] WAD size mismatch: got %u expected %u\n",
                        (unsigned)received, (unsigned)wad_size);
                exit(1);
            }
            fclose(fp);
            fprintf(stderr, "[engine] WAD received OK\n");
        }

        // Point IdentifyVersion at the temp dir.
        setenv("DOOMWADDIR", temp_dir, 1);
    }

    // From here on, the engine is free to start. The fd stays open in
    // blocking mode until I_InitGraphics flips it to non-blocking.
}


// ---- engine API: lifecycle -------------------------------------------------

void I_InitGraphics(void)
{
    if (initialized) return;
    if (client_fd < 0) {
        fprintf(stderr, "I_InitGraphics: I_NetBootstrap was not called\n");
        exit(1);
    }
    signal(SIGINT, (void (*)(int)) I_Quit);

    if (net_set_nonblocking(client_fd) < 0) {
        fprintf(stderr, "I_InitGraphics: set non-blocking: %s\n", strerror(errno));
        exit(1);
    }

    // No payload - engine is up, frames imminent.
    if (net_send(client_fd, MSG_READY, NULL, 0) < 0) {
        fprintf(stderr, "I_InitGraphics: send READY: %s\n", strerror(errno));
        exit(1);
    }
    initialized = 1;
}

void I_ShutdownGraphics(void)
{
    if (client_fd >= 0) {
        // Best-effort polite BYE.
        net_send(client_fd, MSG_BYE_S, NULL, 0);
        close(client_fd);
        client_fd = -1;
    }
    cleanup_temp();
}


// ---- engine API: input -----------------------------------------------------

void I_StartFrame(void) { /* nothing */ }

void I_StartTic(void)
{
    uint8_t        type;
    uint32_t       len;
    const uint8_t *payload;
    int            r;

    if (!initialized) return;

    while ((r = net_try_recv(client_fd, &rx, &type, &len, &payload)) > 0) {
        switch (type) {
          case MSG_INPUT_EVENT: {
              event_t e;
              if (len < 13) break;
              {
                  uint8_t ev = payload[0];
                  int32_t d1 = (int32_t)((uint32_t)payload[1]
                                       | ((uint32_t)payload[2] << 8)
                                       | ((uint32_t)payload[3] << 16)
                                       | ((uint32_t)payload[4] << 24));
                  int32_t d2 = (int32_t)((uint32_t)payload[5]
                                       | ((uint32_t)payload[6] << 8)
                                       | ((uint32_t)payload[7] << 16)
                                       | ((uint32_t)payload[8] << 24));
                  int32_t d3 = (int32_t)((uint32_t)payload[9]
                                       | ((uint32_t)payload[10] << 8)
                                       | ((uint32_t)payload[11] << 16)
                                       | ((uint32_t)payload[12] << 24));
                  switch (ev) {
                    case DOOMNET_EV_KEYDOWN:  e.type = ev_keydown;  break;
                    case DOOMNET_EV_KEYUP:    e.type = ev_keyup;    break;
                    case DOOMNET_EV_MOUSE:    e.type = ev_mouse;    break;
                    case DOOMNET_EV_JOYSTICK: e.type = ev_joystick; break;
                    default: continue;
                  }
                  e.data1 = d1; e.data2 = d2; e.data3 = d3;
                  D_PostEvent(&e);
              }
              break;
          }

          case MSG_PING: {
              // Echo straight back as PONG so the client can measure RTT.
              net_send(client_fd, MSG_PONG, payload, len);
              break;
          }

          case MSG_BYE:
              fprintf(stderr, "[engine] client said BYE\n");
              I_Quit();
              break;

          default:
              // Forward-compat: ignore unknown message types.
              break;
        }
    }
    if (r < 0) {
        fprintf(stderr, "[engine] client disconnected\n");
        I_Quit();
    }
}


// ---- engine API: video out -------------------------------------------------

void I_UpdateNoBlit(void) { /* nothing */ }

void I_SetPalette(byte *palette_in)
{
    int i;
    int c;
    for (i = 0; i < 256; i++) {
        c = gammatable[usegamma][*palette_in++]; cur_palette[i*4 + 2] = (uint8_t)c; // R
        c = gammatable[usegamma][*palette_in++]; cur_palette[i*4 + 1] = (uint8_t)c; // G
        c = gammatable[usegamma][*palette_in++]; cur_palette[i*4 + 0] = (uint8_t)c; // B
        cur_palette[i*4 + 3] = 0xFF;                                                // A
    }
    palette_dirty = 1;
}

void I_FinishUpdate(void)
{
    static int lasttic;
    uint8_t    flags = 0;

    if (!initialized) return;

    // -devparm tic dots, same as original.
    if (devparm) {
        int now  = I_GetTime();
        int tics = now - lasttic;
        int i;
        lasttic = now;
        if (tics > 20) tics = 20;
        for (i = 0; i < tics * 2; i += 2)
            screens[0][(SCREENHEIGHT - 1) * SCREENWIDTH + i] = 0xff;
        for ( ; i < 20 * 2; i += 2)
            screens[0][(SCREENHEIGHT - 1) * SCREENWIDTH + i] = 0x0;
    }

    if (palette_dirty) {
        if (net_send(client_fd, MSG_PALETTE, cur_palette, DOOMNET_PALETTE_BYTES) < 0) {
            fprintf(stderr, "[engine] send PALETTE failed: %s\n", strerror(errno));
            I_Quit();
        }
        flags |= DOOMNET_FRAME_FLAG_PALETTE_DIRTY;
        palette_dirty = 0;
    }

    if (net_send2(client_fd, MSG_FRAME,
                  &flags, 1,
                  screens[0], DOOMNET_FRAME_BYTES) < 0) {
        fprintf(stderr, "[engine] send FRAME failed: %s\n", strerror(errno));
        I_Quit();
    }
}

// Used by the screen-wipe code; identical to the SDL version.
void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}
