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

#include "wire.h"
#include "framing.h"


// ---- session state ---------------------------------------------------------

static int      client_fd       = -1;   // accepted client socket
static int      report_pipe_wfd = -1;   // child->supervisor pipe; closed after
                                        // we've reported our tempdir
static int      initialized     = 0;    // set by I_InitGraphics

// Called by the supervisor (i_supervisor.c) before I_NetBootstrap to hand in
// the already-accepted client fd and the write end of the report pipe.
void I_NetSetSessionFds(int fd, int report_wfd)
{
    client_fd       = fd;
    report_pipe_wfd = report_wfd;
}

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
static char     temp_config[512] = {0};  // set iff MSG_CONFIG was received


// ---- helpers ---------------------------------------------------------------

static void cleanup_temp(void)
{
    if (temp_config[0]) { unlink(temp_config); temp_config[0] = 0; }
    if (temp_wad[0])    { unlink(temp_wad);    temp_wad[0]    = 0; }
    if (temp_dir[0])    { rmdir(temp_dir);     temp_dir[0]    = 0; }
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

// Append client-supplied CLI args (received via MSG_ARGS) to the global
// myargv/myargc so D_DoomMain's existing M_CheckParm("-warp")/("-skill")
// lookups find them with no further changes to the game code.
//
// Payload layout (see wire.h MSG_ARGS):
//   u16 argc
//   for each i: u16 len; bytes[len]   (string is NOT null-terminated on wire)
//
// Strings are sanity-bounded and copied into freshly-allocated null-terminated
// buffers; the new myargv array is malloc'd and replaces the original (which
// came from main()'s stack-resident argv).
static void inject_args(const uint8_t *payload, uint32_t len)
{
    uint32_t  off = 0;
    uint16_t  add_argc;
    char    **new_argv;
    int       i;

    if (len < 2) {
        fprintf(stderr, "[engine] MSG_ARGS too short\n");
        cleanup_temp(); exit(1);
    }
    add_argc = (uint16_t)(payload[0] | (payload[1] << 8));
    off = 2;

    if (add_argc == 0) return;
    // Hard cap to keep the protocol from forcing the engine to allocate
    // unbounded memory. Way more than enough for "-skill N -warp E M".
    if (add_argc > 32) {
        fprintf(stderr, "[engine] MSG_ARGS argc=%u too large\n", add_argc);
        cleanup_temp(); exit(1);
    }

    new_argv = (char **)malloc(sizeof(char *) * (myargc + add_argc));
    if (!new_argv) die("malloc argv");
    for (i = 0; i < myargc; i++) new_argv[i] = myargv[i];

    for (i = 0; i < add_argc; i++) {
        uint16_t slen;
        char    *s;

        if (off + 2 > len) {
            fprintf(stderr, "[engine] MSG_ARGS truncated header\n");
            cleanup_temp(); exit(1);
        }
        slen = (uint16_t)(payload[off] | (payload[off + 1] << 8));
        off += 2;
        if (slen > 64 || off + slen > len) {
            fprintf(stderr, "[engine] MSG_ARGS bad string len\n");
            cleanup_temp(); exit(1);
        }

        s = (char *)malloc((size_t)slen + 1);
        if (!s) die("malloc arg str");
        memcpy(s, payload + off, slen);
        s[slen] = 0;
        off += slen;

        new_argv[myargc + i] = s;
    }

    myargv = new_argv;
    myargc += add_argc;

    fprintf(stderr, "[engine] received %u client arg(s)\n", (unsigned)add_argc);
}

// Write a MSG_CONFIG payload to <temp_dir>/default.cfg and append
// "-config <path>" to myargv so the existing M_LoadDefaults code finds it.
// Safe to call only after temp_dir has been created.
static void inject_config(const uint8_t *payload, uint32_t len)
{
    FILE    *fp;
    char    *arg_flag;
    char    *arg_path;
    char   **new_argv;
    int      i;

    if (len > DOOMNET_CONFIG_MAX) {
        fprintf(stderr, "[engine] MSG_CONFIG too large (%u bytes)\n", (unsigned)len);
        cleanup_temp(); exit(1);
    }
    if (!temp_dir[0]) {
        fprintf(stderr, "[engine] MSG_CONFIG before temp dir was created\n");
        cleanup_temp(); exit(1);
    }
    if (temp_config[0]) {
        fprintf(stderr, "[engine] duplicate MSG_CONFIG ignored\n");
        return;
    }

    snprintf(temp_config, sizeof temp_config, "%s/default.cfg", temp_dir);
    fp = fopen(temp_config, "wb");
    if (!fp) { temp_config[0] = 0; die("fopen tmp config"); }
    if (len && fwrite(payload, 1, len, fp) != len) {
        fclose(fp); die("fwrite tmp config");
    }
    fclose(fp);

    arg_flag = strdup("-config");
    arg_path = strdup(temp_config);
    if (!arg_flag || !arg_path) die("strdup config arg");

    new_argv = (char **)malloc(sizeof(char *) * (myargc + 2));
    if (!new_argv) die("malloc argv (config)");
    for (i = 0; i < myargc; i++) new_argv[i] = myargv[i];
    new_argv[myargc]     = arg_flag;
    new_argv[myargc + 1] = arg_path;
    myargv  = new_argv;
    myargc += 2;

    fprintf(stderr, "[engine] received config (%u bytes) -> %s\n",
            (unsigned)len, temp_config);
}


// ---- bootstrap (runs BEFORE D_DoomMain) -----------------------------------

void I_NetBootstrap(void)
{
    uint8_t             buf[DOOMNET_WAD_CHUNK + 64];
    uint8_t             type;
    uint32_t            len;
    int                 r;

    // A broken connection should surface as a send() error, not a fatal
    // signal that prevents I_Quit from running.
    signal(SIGPIPE, SIG_IGN);

    // The supervisor (i_supervisor.c) is the owner of the listening socket
    // and accepts each connection on our behalf; it hands us the already-
    // connected client fd via I_NetSetSessionFds before calling us.
    if (client_fd < 0) {
        fprintf(stderr,
                "I_NetBootstrap: no client fd; was I_NetSetSessionFds called?\n");
        exit(1);
    }

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

        // Report our temp dir to the supervisor so it can rm -rf the path
        // even if we die by signal before atexit runs. Write then close - the
        // supervisor uses EOF as the report-complete signal.
        if (report_pipe_wfd >= 0) {
            ssize_t to_write = (ssize_t)strlen(temp_dir);
            ssize_t off      = 0;
            while (off < to_write) {
                ssize_t w = write(report_pipe_wfd, temp_dir + off, to_write - off);
                if (w < 0) { if (errno == EINTR) continue; break; }
                off += w;
            }
            close(report_pipe_wfd);
            report_pipe_wfd = -1;
        }

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
                if (type == MSG_ARGS) {
                    // Client may interleave its CLI args (e.g. -warp, -skill)
                    // anywhere between HELLO_ACK and WAD_DONE. Process and
                    // continue receiving WAD data.
                    inject_args(buf, len);
                    continue;
                }
                if (type == MSG_CONFIG) {
                    // Optional per-session config blob. Write to the temp dir
                    // and inject "-config <path>" so the existing
                    // M_LoadDefaults("-config") lookup picks it up.
                    inject_config(buf, len);
                    continue;
                }
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
        // If the session had a per-user config, ship the (possibly
        // M_SaveDefaults-updated) version back so the client can persist it.
        if (temp_config[0]) {
            FILE *fp = fopen(temp_config, "rb");
            if (fp) {
                uint8_t *cfg;
                long     sz;
                fseek(fp, 0, SEEK_END);
                sz = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (sz > 0 && sz <= (long)DOOMNET_CONFIG_MAX
                    && (cfg = (uint8_t *)malloc((size_t)sz)) != NULL) {
                    if (fread(cfg, 1, (size_t)sz, fp) == (size_t)sz) {
                        net_send(client_fd, MSG_CONFIG_OUT, cfg, (uint32_t)sz);
                    }
                    free(cfg);
                }
                fclose(fp);
            }
        }
        // Best-effort polite BYE.
        net_send(client_fd, MSG_BYE_S, NULL, 0);
        close(client_fd);
        client_fd = -1;
    }
    cleanup_temp();
}


// ---- engine API: input -----------------------------------------------------

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
    static int last_frame_tic = -1;
    int        now;
    uint8_t    flags = 0;

    if (!initialized) return;

    // D_DoomLoop spins as fast as the CPU allows and calls I_FinishUpdate
    // every iteration. The original SDL backend was rate-limited by
    // SDL_RENDERER_PRESENTVSYNC; over the network we have no such governor.
    // Cap frame output to the game's native 35 Hz so we don't (a) saturate
    // the link with thousands of duplicate frames per second and (b) lock
    // the client in its receive loop and prevent it from polling input.
    now = I_GetTime();
    if (now == last_frame_tic) return;
    last_frame_tic = now;

    // -devparm tic dots, same as original.
    if (devparm) {
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
