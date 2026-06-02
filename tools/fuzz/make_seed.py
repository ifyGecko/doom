#!/usr/bin/env python3
"""
Generate AFL++ seed corpus for doom-engine-fuzz.

Each seed is a single byte stream that, when fed to the fuzz binary's
stdin (the harness wires the engine's client_fd to fd 0 in
harness_wraps.c), drives I_NetBootstrap through a full handshake + WAD
upload, after which D_DoomMain runs through M_LoadDefaults /
W_InitMultipleFiles / R_Init / P_Init.

Wire format reference: src/common/wire.h. All multi-byte ints little-endian.
Each message:  u8 type | u32 payload_len | payload

Usage:
    make_seed.py <miniwad.wad> <out_dir> [--cfg <default.cfg>]
"""

import argparse
import os
import struct
import sys

# Mirrors src/common/wire.h ---------------------------------------------------
MSG_HELLO       = 0x01
MSG_ARGS        = 0x02
MSG_WAD_CHUNK   = 0x03
MSG_WAD_DONE    = 0x04
MSG_CONFIG      = 0x08

DOOMNET_PROTO_VERSION = 1
DOOMNET_SCREEN_W      = 320
DOOMNET_SCREEN_H      = 200
DOOMNET_WAD_CHUNK     = 256 * 1024


def frame(msg_type: int, payload: bytes) -> bytes:
    return struct.pack("<BI", msg_type, len(payload)) + payload


def mk_hello(wad_name: str, wad_size: int, caps: int = 0) -> bytes:
    name = wad_name.encode("ascii")
    payload  = struct.pack("<HHHI", DOOMNET_PROTO_VERSION,
                           DOOMNET_SCREEN_W, DOOMNET_SCREEN_H, caps)
    payload += struct.pack("<H", len(name)) + name
    payload += struct.pack("<I", wad_size)
    return frame(MSG_HELLO, payload)


def mk_args(args: list) -> bytes:
    payload = struct.pack("<H", len(args))
    for a in args:
        b = a.encode("ascii")
        payload += struct.pack("<H", len(b)) + b
    return frame(MSG_ARGS, payload)


def mk_config(blob: bytes) -> bytes:
    return frame(MSG_CONFIG, blob)


def mk_wad_chunks(wad_bytes: bytes, chunk_size: int = DOOMNET_WAD_CHUNK) -> bytes:
    out = b""
    offset = 0
    while offset < len(wad_bytes):
        slice_ = wad_bytes[offset:offset + chunk_size]
        out += frame(MSG_WAD_CHUNK, struct.pack("<I", offset) + slice_)
        offset += len(slice_)
    return out


def mk_wad_done(total: int) -> bytes:
    return frame(MSG_WAD_DONE, struct.pack("<I", total))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("wad",     help="path to seed WAD (e.g. miniwad.wad)")
    ap.add_argument("out_dir", help="directory to write seed files into")
    ap.add_argument("--cfg",   help="path to a captured default.cfg (optional)")
    args = ap.parse_args()

    with open(args.wad, "rb") as f:
        wad = f.read()
    wad_basename = os.path.basename(args.wad)
    if not wad_basename.endswith(".wad"):
        wad_basename = "fuzz.wad"
    cfg = b""
    if args.cfg and os.path.exists(args.cfg):
        with open(args.cfg, "rb") as f:
            cfg = f.read()

    os.makedirs(args.out_dir, exist_ok=True)

    cli_args = ["-skill", "1", "-warp", "1", "1"]

    seeds = {}

    # 1. Full happy-path seed: HELLO + ARGS + CONFIG + chunks + DONE
    seeds["seed_full.bin"] = (
        mk_hello(wad_basename, len(wad))
        + mk_args(cli_args)
        + (mk_config(cfg) if cfg else b"")
        + mk_wad_chunks(wad)
        + mk_wad_done(len(wad))
    )

    # 2. No client-supplied args (default skill / map)
    seeds["seed_noargs.bin"] = (
        mk_hello(wad_basename, len(wad))
        + (mk_config(cfg) if cfg else b"")
        + mk_wad_chunks(wad)
        + mk_wad_done(len(wad))
    )

    # 3. No config blob
    seeds["seed_nocfg.bin"] = (
        mk_hello(wad_basename, len(wad))
        + mk_args(cli_args)
        + mk_wad_chunks(wad)
        + mk_wad_done(len(wad))
    )

    # 4. Truncated WAD upload - client says wad_size = N but only sends N/2 bytes
    #    and then WAD_DONE. Exercises the size-mismatch branch in I_NetBootstrap.
    half = len(wad) // 2
    seeds["seed_short.bin"] = (
        mk_hello(wad_basename, len(wad))
        + mk_wad_chunks(wad[:half])
        + mk_wad_done(len(wad))
    )

    # 5. Tiny seed: just HELLO advertising a 0-byte WAD. Lets AFL discover
    #    structural mutations near the head of the stream cheaply.
    seeds["seed_tiny.bin"] = (
        mk_hello("a.wad", 0) + mk_wad_done(0)
    )

    for name, data in seeds.items():
        path = os.path.join(args.out_dir, name)
        with open(path, "wb") as f:
            f.write(data)
        print(f"wrote {path} ({len(data)} bytes)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
