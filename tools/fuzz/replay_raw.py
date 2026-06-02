#!/usr/bin/env python3
"""
Byte-faithful TCP replay of a fuzz corpus item against a running doom-engine.

Unlike split_corpus.py, this script does NOT interpret or repack the corpus
on the send side. It opens a TCP connection to the engine supervisor and
sends the entire corpus byte-for-byte; whatever bug the fuzzer found
because of a fuzz-mutated wire field (oversize len, bad chunk offset,
out-of-order frame, etc.) will be triggered exactly the same way.

On the receive side it decodes the engine's reply frames and pretty-prints
them to stderr so it is easy to see what the engine made of the corpus
(HELLO_ACK, WAD_ACKs, LOG lines, ERROR, BYE_S, etc.). Decoding the replies
does NOT affect what we send.

Usage:
    replay_raw.py <corpus.bin> [--host H] [--port N]
                  [--dump-rx FILE] [--timeout SECS] [--quiet]
"""

import argparse
import socket
import struct
import sys
import time

# Mirrors src/common/wire.h ---------------------------------------------------
MSG_HELLO_ACK   = 0x81
MSG_WAD_ACK     = 0x82
MSG_READY       = 0x83
MSG_PALETTE     = 0x84
MSG_FRAME       = 0x85
MSG_PONG        = 0x86
MSG_ERROR       = 0x87
MSG_BYE_S       = 0x88
MSG_CONFIG_OUT  = 0x89
MSG_LOG         = 0x8A

DOOMNET_HEADER_BYTES = 5
DOOMNET_MAX_PAYLOAD  = 2 * 1024 * 1024

LOG_LEVELS = {0: "INFO", 1: "WARN", 2: "ERROR"}


def fmt_hex(b: bytes, cap: int = 32) -> str:
    if len(b) <= cap:
        return b.hex()
    return b[:cap].hex() + f"... ({len(b)} bytes total)"


def decode_frame(mtype: int, payload: bytes) -> str:
    """Return a human-readable one-line description of an engine -> client frame."""
    if mtype == MSG_HELLO_ACK:
        if len(payload) >= 6:
            proto, w, h = struct.unpack_from("<HHH", payload, 0)
            return f"HELLO_ACK proto={proto} screen={w}x{h}"
        return f"HELLO_ACK (short, {len(payload)} bytes)"

    if mtype == MSG_WAD_ACK:
        if len(payload) >= 4:
            (received,) = struct.unpack_from("<I", payload, 0)
            return f"WAD_ACK received={received}"
        return f"WAD_ACK (short, {len(payload)} bytes)"

    if mtype == MSG_READY:
        return "READY (engine init complete, frames imminent)"

    if mtype == MSG_PALETTE:
        return f"PALETTE ({len(payload)} bytes, expect 1024 for 256 BGRA entries)"

    if mtype == MSG_FRAME:
        if len(payload) >= 1:
            flags = payload[0]
            return (f"FRAME flags=0x{flags:02x} pixels={len(payload)-1} bytes "
                    f"(expect {320*200})")
        return "FRAME (empty)"

    if mtype == MSG_PONG:
        if len(payload) >= 8:
            (ts,) = struct.unpack_from("<Q", payload, 0)
            return f"PONG client_time_us={ts}"
        return f"PONG (short, {len(payload)} bytes)"

    if mtype == MSG_ERROR:
        if len(payload) >= 2:
            (code,) = struct.unpack_from("<H", payload, 0)
            msg = payload[2:]
            try:
                txt = msg.decode("utf-8")
            except UnicodeDecodeError:
                txt = msg.decode("utf-8", "replace")
            return f"ERROR code={code} msg={txt!r}"
        return f"ERROR (short, {len(payload)} bytes)"

    if mtype == MSG_BYE_S:
        return "BYE_S (engine done)"

    if mtype == MSG_CONFIG_OUT:
        return f"CONFIG_OUT ({len(payload)} bytes of updated default.cfg)"

    if mtype == MSG_LOG:
        if len(payload) >= 12:
            (ts_us,) = struct.unpack_from("<Q", payload, 0)
            level    = payload[8]
            (msg_len,) = struct.unpack_from("<H", payload, 10)
            level_str = LOG_LEVELS.get(level, f"L{level}")
            msg = payload[12:12 + msg_len]
            try:
                txt = msg.decode("utf-8")
            except UnicodeDecodeError:
                txt = msg.decode("utf-8", "replace")
            return f"LOG [{level_str} @{ts_us}us] {txt}"
        return f"LOG (short, {len(payload)} bytes)"

    return f"<unknown msg type=0x{mtype:02x} len={len(payload)} data={fmt_hex(payload)}>"


def pretty_print(buf: bytearray, log) -> int:
    """Pretty-print as many complete frames as buf contains. Returns the
    number of bytes consumed; caller compacts the buffer for the next pass."""
    consumed = 0
    while len(buf) - consumed >= DOOMNET_HEADER_BYTES:
        msg_type = buf[consumed]
        (plen,)  = struct.unpack_from("<I", buf, consumed + 1)
        if plen > DOOMNET_MAX_PAYLOAD:
            log(f"[engine] <stream desync: oversize len={plen} at +{consumed}; "
                f"dropping remaining {len(buf) - consumed} bytes>")
            consumed = len(buf)
            break
        if len(buf) - consumed < DOOMNET_HEADER_BYTES + plen:
            break  # wait for more bytes
        payload = bytes(buf[consumed + DOOMNET_HEADER_BYTES :
                            consumed + DOOMNET_HEADER_BYTES + plen])
        log(f"[engine] {decode_frame(msg_type, payload)}")
        consumed += DOOMNET_HEADER_BYTES + plen
    return consumed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("corpus",     help="binary corpus / crash / hang file")
    ap.add_argument("--host",     default="127.0.0.1")
    ap.add_argument("--port",     type=int, default=6666)
    ap.add_argument("--dump-rx",  metavar="FILE",
        help="also write the raw engine reply bytes to FILE")
    ap.add_argument("--timeout",  type=float, default=5.0,
        help="seconds of recv silence after EOF/idle to consider the replay done (default: 5)")
    ap.add_argument("--quiet",    action="store_true",
        help="suppress decoded engine replies on stderr")
    args = ap.parse_args()

    with open(args.corpus, "rb") as f:
        data = f.read()

    def log(msg):
        if not args.quiet:
            sys.stderr.write(msg + "\n")
            sys.stderr.flush()

    log(f"[replay_raw] connecting to {args.host}:{args.port}")
    s = socket.create_connection((args.host, args.port), timeout=10)
    s.settimeout(args.timeout)
    try:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass

    log(f"[replay_raw] sending {len(data)} bytes of corpus verbatim")
    s.sendall(data)
    # Half-close the write side so the engine sees EOF on read once it has
    # drained the bytes - matches the fuzz harness's "AFL input pipe is
    # closed" behavior. The engine reads more after MSG_READY for input
    # events; without this it would block waiting for them. Half-shutdown
    # is enough; we still receive freely.
    try:
        s.shutdown(socket.SHUT_WR)
        log("[replay_raw] half-closed TX (engine will see EOF on read)")
    except OSError:
        pass

    rx_buf = bytearray()
    dump_f = open(args.dump_rx, "wb") if args.dump_rx else None
    total_rx = 0
    started_at = time.monotonic()
    try:
        while True:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                log(f"[replay_raw] recv timeout after {args.timeout}s; ending replay")
                break
            if not chunk:
                log("[replay_raw] engine closed connection")
                break
            total_rx += len(chunk)
            if dump_f is not None:
                dump_f.write(chunk)
            rx_buf.extend(chunk)
            used = pretty_print(rx_buf, log)
            if used:
                del rx_buf[:used]
    finally:
        if dump_f is not None:
            dump_f.close()
        try:
            s.close()
        except OSError:
            pass

    elapsed = time.monotonic() - started_at
    log(f"[replay_raw] done: received {total_rx} bytes in {elapsed:.2f}s")
    if rx_buf:
        log(f"[replay_raw] {len(rx_buf)} undecoded RX bytes left "
            f"(partial frame): {fmt_hex(bytes(rx_buf))}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
