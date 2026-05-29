// -----------------------------------------------------------------------------
// DOOM remote-play wire protocol
// -----------------------------------------------------------------------------
//
// Single TCP connection between doom-client (input + display) and doom-engine
// (headless game logic). All multi-byte integers are little-endian.
//
// Every message on the wire is framed as:
//
//     [ u8  type ]
//     [ u32 payload_len (LE) ]
//     [ payload bytes ... ]
//
// Header is always exactly DOOMNET_HEADER_BYTES bytes.
//
// -----------------------------------------------------------------------------

#ifndef DOOMNET_WIRE_H
#define DOOMNET_WIRE_H

#include <stdint.h>

#define DOOMNET_PROTO_VERSION   1u
#define DOOMNET_MAGIC           0x444E4554u   // "DNET" in LE
#define DOOMNET_DEFAULT_PORT    6666

// DOOM framebuffer dimensions are baked into the protocol.
#define DOOMNET_SCREEN_W        320
#define DOOMNET_SCREEN_H        200
#define DOOMNET_FRAME_BYTES     (DOOMNET_SCREEN_W * DOOMNET_SCREEN_H)

// Palette wire format: 256 entries of B,G,R,A.
#define DOOMNET_PALETTE_ENTRIES 256
#define DOOMNET_PALETTE_BYTES   (DOOMNET_PALETTE_ENTRIES * 4)

// Upper bound on one message payload. Sized to fit a frame (64000 bytes)
// plus headroom for the largest WAD upload chunk we use.
#define DOOMNET_WAD_CHUNK       (256u * 1024u)
#define DOOMNET_MAX_PAYLOAD     (2u * 1024u * 1024u)

#define DOOMNET_HEADER_BYTES    5

// Message type IDs. High bit set => server-to-client direction.
enum {
    // client -> server
    MSG_HELLO       = 0x01,  // HELLO payload (see below)
    MSG_ARGS        = 0x02,  // { u16 argc; (u16 len + bytes)*argc } - optional
    MSG_WAD_CHUNK   = 0x03,  // { u32 offset; bytes data }
    MSG_WAD_DONE    = 0x04,  // { u32 total_size }
    MSG_INPUT_EVENT = 0x05,  // { u8 ev_type; i32 d1; i32 d2; i32 d3 }
    MSG_PING        = 0x06,  // { u64 client_time_us }
    MSG_BYE         = 0x07,  // empty
    MSG_CONFIG      = 0x08,  // raw bytes of default.cfg (<= DOOMNET_CONFIG_MAX)
                             // optional; if sent must arrive after HELLO_ACK
                             // and before WAD_DONE. Engine writes the bytes
                             // to its per-session temp dir and points the
                             // existing -config CLI lookup at the file.

    // server -> client
    MSG_HELLO_ACK   = 0x81,  // { u16 proto_ver; u16 screen_w; u16 screen_h }
    MSG_WAD_ACK     = 0x82,  // { u32 offset_received }
    MSG_READY       = 0x83,  // empty - engine is running, frames imminent
    MSG_PALETTE     = 0x84,  // 256*4 bytes BGRA, post-gamma
    MSG_FRAME       = 0x85,  // { u8 flags; bytes pixels[FRAME_BYTES] }
    MSG_PONG        = 0x86,  // { u64 client_time_us } - echoed from PING
    MSG_ERROR       = 0x87,  // { u16 code; bytes msg }
    MSG_BYE_S       = 0x88,  // empty
    MSG_CONFIG_OUT  = 0x89   // raw bytes of default.cfg as written by the
                             // engine at clean shutdown (M_SaveDefaults).
                             // Sent immediately before MSG_BYE_S so a client
                             // can persist updated defaults. Omitted if no
                             // config file ever existed for the session.
};

// Hard upper bound on a single MSG_CONFIG / MSG_CONFIG_OUT payload. The real
// default.cfg is well under 8 KiB; we allow some headroom for future keys.
#define DOOMNET_CONFIG_MAX      (256u * 1024u)

// FRAME flags
#define DOOMNET_FRAME_FLAG_PALETTE_DIRTY  0x01u

// Wire-format event types. Pinned values so the engine's evtype_t enum can
// reshuffle without breaking the protocol.
#define DOOMNET_EV_KEYDOWN   0u
#define DOOMNET_EV_KEYUP     1u
#define DOOMNET_EV_MOUSE     2u

// MSG_HELLO payload (client -> server):
//
//   u16 proto_ver
//   u16 screen_w               -- client expects these dimensions
//   u16 screen_h
//   u32 caps                   -- reserved for v2 feature bits, send 0
//   u16 wad_name_len
//   bytes wad_name[wad_name_len]  -- canonical basename, e.g. "doom1.wad"
//   u32 wad_total_size            -- total bytes the client will upload
//
// The wad_name is only a basename hint for the engine's temp copy and may be
// any "*.wad" name. The engine identifies the game mode from the WAD contents
// (see D_DetectGameMode in d_main.c), so the filename does not need to match
// any particular IWAD name.

#endif
