#!/usr/bin/env python3
"""
Split an AFL++ corpus item / crash / hang back into the components a real
doom-client would have sent over the wire: a WAD blob, a default.cfg blob,
a list of CLI args, and the HELLO handshake fields.

Useful for taking an interesting finding out of fuzz/out/default/{queue,
crashes,hangs}/ and looking at what it contained. Note that this extraction
is INTERPRETIVE and may be lossy - every length, offset and order field in
the wire format is fuzz-mutable, but a real doom-client recomputes them all
from local state when it re-packs. Each run computes a verdict line up top
of report.txt indicating whether replay via doom-client is byte-identical
to the original corpus; when it is not, use tools/fuzz/replay_raw.py for
true byte-faithful TCP replay.

Usage:
    split_corpus.py <input.bin> <out_dir>

Writes into <out_dir>:
    <wad_basename>          - reconstructed WAD bytes (best effort)
    default.cfg             - reconstructed config blob (if MSG_CONFIG seen)
    args.txt                - one MSG_ARGS string per line
    hello.txt               - decoded HELLO header fields
    report.txt              - verdict, severity-graded findings, raw stream map
    replay.sh               - sample reproduction commands (raw + doom-client)
"""

import argparse
import os
import re
import struct
import sys

# Mirrors src/common/wire.h ---------------------------------------------------
MSG_HELLO       = 0x01
MSG_ARGS        = 0x02
MSG_WAD_CHUNK   = 0x03
MSG_WAD_DONE    = 0x04
MSG_INPUT_EVENT = 0x05
MSG_PING        = 0x06
MSG_BYE         = 0x07
MSG_CONFIG      = 0x08

MSG_NAMES = {
    MSG_HELLO:       "HELLO",
    MSG_ARGS:        "ARGS",
    MSG_WAD_CHUNK:   "WAD_CHUNK",
    MSG_WAD_DONE:    "WAD_DONE",
    MSG_INPUT_EVENT: "INPUT_EVENT",
    MSG_PING:        "PING",
    MSG_BYE:         "BYE",
    MSG_CONFIG:      "CONFIG",
}

DOOMNET_PROTO_VERSION = 1
DOOMNET_SCREEN_W      = 320
DOOMNET_SCREEN_H      = 200
DOOMNET_HEADER_BYTES  = 5
DOOMNET_MAX_PAYLOAD   = 2 * 1024 * 1024
DOOMNET_CONFIG_MAX    = 256 * 1024
DOOMNET_WAD_CHUNK     = 256 * 1024

# Set of frame types I_NetBootstrap accepts during the WAD upload loop.
UPLOAD_OK = {MSG_ARGS, MSG_CONFIG, MSG_WAD_CHUNK, MSG_WAD_DONE}

# Severity codes for the discrepancy report.
CRIT, WARN, INFO = "CRITICAL", "WARNING", "INFO"


# -----------------------------------------------------------------------------
# Parsing
# -----------------------------------------------------------------------------

class Message:
    """A successfully-parsed wire frame. Holds enough byte-level detail to
    let the discrepancy detector locate fields in the original corpus."""
    __slots__ = ("offset", "type", "length", "payload", "consumed")

    def __init__(self, offset, mtype, length, payload):
        self.offset   = offset                              # byte offset of header start
        self.type     = mtype                               # u8 type
        self.length   = length                              # u32 payload_len field
        self.payload  = payload                             # exactly `length` bytes
        self.consumed = DOOMNET_HEADER_BYTES + length       # bytes the frame occupies


class Parsed:
    """Result of walking the corpus stream once."""
    def __init__(self):
        self.messages = []                  # list[Message] in stream order
        self.skipped  = []                  # list[(offset, reason)]


def parse_stream(data: bytes) -> Parsed:
    """Walk the byte stream picking off framed messages. Resync on bad
    headers (oversize length) by skipping one byte and retrying."""
    p   = Parsed()
    pos = 0
    n   = len(data)
    while pos + DOOMNET_HEADER_BYTES <= n:
        msg_type = data[pos]
        (plen,)  = struct.unpack_from("<I", data, pos + 1)
        if plen > DOOMNET_MAX_PAYLOAD:
            p.skipped.append((pos, f"oversize len={plen}, resync"))
            pos += 1
            continue
        if pos + DOOMNET_HEADER_BYTES + plen > n:
            p.skipped.append((pos,
                f"truncated msg type=0x{msg_type:02x} len={plen} "
                f"have={n - pos - DOOMNET_HEADER_BYTES} bytes after header"))
            break
        payload = data[pos + DOOMNET_HEADER_BYTES : pos + DOOMNET_HEADER_BYTES + plen]
        p.messages.append(Message(pos, msg_type, plen, payload))
        pos += DOOMNET_HEADER_BYTES + plen
    if pos < n:
        p.skipped.append((pos, f"trailing {n - pos} bytes (incomplete header)"))
    return p


# -----------------------------------------------------------------------------
# Per-message decoders (return None on a malformed payload, else a dict).
# -----------------------------------------------------------------------------

def decode_hello(payload: bytes):
    if len(payload) < 12:
        return None
    proto, w, h, caps, wad_nlen = struct.unpack_from("<HHHIH", payload, 0)
    name_end = 12 + wad_nlen
    if name_end + 4 > len(payload):
        return None
    name      = payload[12:name_end]
    wad_size, = struct.unpack_from("<I", payload, name_end)
    well_formed = (12 + wad_nlen + 4 == len(payload))
    return {
        "proto": proto, "screen_w": w, "screen_h": h, "caps": caps,
        "wad_nlen": wad_nlen, "wad_name": name, "wad_size": wad_size,
        "well_formed": well_formed,
    }


def decode_args(payload: bytes):
    if len(payload) < 2:
        return None
    (argc,) = struct.unpack_from("<H", payload, 0)
    off, out = 2, []
    for _ in range(argc):
        if off + 2 > len(payload):
            return {"argc": argc, "strings": out, "truncated_at": off}
        (slen,) = struct.unpack_from("<H", payload, off)
        off += 2
        if off + slen > len(payload):
            return {"argc": argc, "strings": out, "truncated_at": off, "bad_len": slen}
        out.append(payload[off:off + slen])
        off += slen
    trailing = len(payload) - off
    return {"argc": argc, "strings": out, "truncated_at": None, "trailing": trailing}


def decode_wad_chunk(payload: bytes):
    if len(payload) < 4:
        return None
    (offset,) = struct.unpack_from("<I", payload, 0)
    return {"offset": offset, "data": payload[4:]}


def decode_wad_done(payload: bytes):
    if len(payload) < 4:
        return None
    (total,) = struct.unpack_from("<I", payload, 0)
    return {"total_size": total, "extra_bytes": len(payload) - 4}


def sanitize_basename(name: bytes) -> str:
    """Mirror i_video_net.c:sanitize_basename. Strip directory parts and
    clamp to alnum + . _ -. If the result is empty or has bad chars,
    fall back to 'replay.wad'."""
    if not name:
        return "replay.wad"
    s = name.split(b"/")[-1].split(b"\\")[-1]
    try:
        s = s.decode("ascii")
    except UnicodeDecodeError:
        return "replay.wad"
    if not re.fullmatch(r"[A-Za-z0-9._-]+", s):
        return "replay.wad"
    if not s.lower().endswith(".wad"):
        s += ".wad"
    return s


# -----------------------------------------------------------------------------
# Extracted state + discrepancy detector
# -----------------------------------------------------------------------------

class Extracted:
    """All info derivable from the parsed messages plus the findings the
    discrepancy detector turned up while looking at them."""
    def __init__(self):
        self.hello       = None         # decoded HELLO dict or None
        self.hello_msg   = None         # Message for the chosen HELLO
        self.args        = []           # list[bytes] of all MSG_ARGS strings
        self.config      = None         # bytes of MSG_CONFIG payload, or None
        self.config_msg  = None         # Message for the chosen MSG_CONFIG
        self.wad_chunks  = []           # list of (offset, data, src_msg)
        self.wad_done    = None         # decoded WAD_DONE dict or None
        self.wad_bytes   = b""          # reconstructed WAD payload
        self.wad_assembly_info = ""
        self.events      = 0
        self.frames_parsed = 0          # number of successfully framed messages
        self.findings    = []           # list of (severity, offset, message)
        self.skipped     = []           # mirror of Parsed.skipped


def add(findings, severity, offset, msg):
    findings.append((severity, offset, msg))


def extract(parsed: Parsed, data: bytes) -> Extracted:
    e = Extracted()
    e.skipped       = list(parsed.skipped)
    e.frames_parsed = len(parsed.messages)

    # Note bare-stream artifacts as INFO. (CRITICAL because the engine never
    # gets a chance to recover when bytes around frames are garbage.)
    for off, why in parsed.skipped:
        add(e.findings, INFO, off, f"stream-level: {why}")

    # First pass: pick HELLO, CONFIG, and find upload-phase violations.
    # Walk messages and act like the engine: HELLO first; everything until
    # WAD_DONE must be in UPLOAD_OK; WAD_DONE ends the upload.
    seen_hello   = False
    upload_done  = False
    wad_offsets_seen = []
    for m in parsed.messages:
        if m.type == MSG_HELLO:
            if seen_hello:
                add(e.findings, WARN, m.offset, "duplicate HELLO ignored")
                continue
            d = decode_hello(m.payload)
            if d is None:
                add(e.findings, CRIT, m.offset, "malformed HELLO payload (engine: bad HELLO -> exit 1)")
                continue
            if not d["well_formed"]:
                add(e.findings, CRIT, m.offset,
                    f"HELLO payload length mismatch: 12 + wad_nlen({d['wad_nlen']}) + 4 "
                    f"!= {m.length} (engine: malformed HELLO -> exit 1)")
            if d["proto"] != DOOMNET_PROTO_VERSION:
                add(e.findings, CRIT, m.offset,
                    f"HELLO.proto={d['proto']} (expected {DOOMNET_PROTO_VERSION}; "
                    f"engine: proto mismatch -> exit 1)")
            if d["screen_w"] != DOOMNET_SCREEN_W or d["screen_h"] != DOOMNET_SCREEN_H:
                add(e.findings, CRIT, m.offset,
                    f"HELLO.screen={d['screen_w']}x{d['screen_h']} "
                    f"(engine: screen size mismatch -> exit 1)")
            name_sanitized = sanitize_basename(d["wad_name"])
            if not d["wad_name"] or b".wad" not in d["wad_name"].lower():
                add(e.findings, CRIT, m.offset,
                    f"HELLO.wad_name={d['wad_name']!r} fails sanitize "
                    f"(engine: bad wad_name in HELLO -> exit 1)")
            e.hello, e.hello_msg = d, m
            if m.offset != 0:
                # Either: stream-skipped bytes before HELLO, or non-HELLO frame
                # came first. In the latter case, engine reads it as HELLO -> bad HELLO.
                add(e.findings, CRIT, m.offset,
                    f"HELLO at byte {m.offset} is not the first frame "
                    f"(engine: bad HELLO from the leading bytes -> exit 1)")
            seen_hello = True
            continue

        if not seen_hello:
            # Anything before HELLO is read as HELLO by the engine and rejected.
            add(e.findings, CRIT, m.offset,
                f"{MSG_NAMES.get(m.type, 'unknown')} (type=0x{m.type:02x}) "
                f"precedes HELLO (engine: bad HELLO -> exit 1)")

        if upload_done:
            # Bytes after WAD_DONE: engine has moved past bootstrap by the
            # time these would arrive; their fate depends on whether
            # I_InitGraphics flipped the fd non-blocking yet. Mark INFO.
            add(e.findings, INFO, m.offset,
                f"{MSG_NAMES.get(m.type, 'unknown')} after WAD_DONE; engine consumes "
                f"these via I_StartTic (input events) or ignores unknowns")
            if m.type == MSG_INPUT_EVENT:
                e.events += 1
            continue

        # We're between HELLO and WAD_DONE - the upload-loop window.
        if m.type not in UPLOAD_OK:
            add(e.findings, CRIT, m.offset,
                f"{MSG_NAMES.get(m.type, 'unknown')} (type=0x{m.type:02x}) "
                f"during upload phase (engine: unexpected msg during upload -> exit 1)")

        if m.type == MSG_ARGS:
            a = decode_args(m.payload)
            if a is None:
                add(e.findings, CRIT, m.offset, "MSG_ARGS too short to hold argc (engine: -> exit 1)")
                continue
            if a["truncated_at"] is not None:
                add(e.findings, CRIT, m.offset,
                    f"MSG_ARGS truncated: argc={a['argc']} but only {len(a['strings'])} "
                    f"strings recoverable (engine: bad string len -> exit 1)")
            if a.get("trailing", 0):
                add(e.findings, WARN, m.offset,
                    f"MSG_ARGS payload has {a['trailing']} trailing bytes (parsed argc consumed less than payload_len)")
            if a["argc"] > 32:
                add(e.findings, CRIT, m.offset,
                    f"MSG_ARGS argc={a['argc']} > 32 (engine: argc too large -> exit 1)")
            for s in a["strings"]:
                if len(s) > 64:
                    add(e.findings, CRIT, m.offset,
                        f"MSG_ARGS string of len {len(s)} > 64 (engine: bad string len -> exit 1)")
            e.args.extend(a["strings"])

        elif m.type == MSG_CONFIG:
            if e.config is not None:
                add(e.findings, WARN, m.offset, "duplicate MSG_CONFIG ignored")
            elif m.length > DOOMNET_CONFIG_MAX:
                add(e.findings, CRIT, m.offset,
                    f"MSG_CONFIG payload {m.length} > {DOOMNET_CONFIG_MAX} (engine: too large -> exit 1)")
                e.config = m.payload
                e.config_msg = m
            else:
                e.config, e.config_msg = m.payload, m

        elif m.type == MSG_WAD_CHUNK:
            c = decode_wad_chunk(m.payload)
            if c is None:
                add(e.findings, CRIT, m.offset, "MSG_WAD_CHUNK shorter than 4 bytes (engine: -> exit 1)")
                continue
            e.wad_chunks.append((c["offset"], c["data"], m))
            wad_offsets_seen.append((c["offset"], len(c["data"]), m.offset))

        elif m.type == MSG_WAD_DONE:
            d = decode_wad_done(m.payload)
            if d is None:
                add(e.findings, CRIT, m.offset, "MSG_WAD_DONE shorter than 4 bytes")
                continue
            if d["extra_bytes"]:
                add(e.findings, INFO, m.offset,
                    f"MSG_WAD_DONE payload has {d['extra_bytes']} extra trailing bytes")
            e.wad_done = d
            upload_done = True

    # Pass two: WAD chunk sequencing checks. The engine requires offset ==
    # received for every chunk; any deviation is a CRITICAL.
    expected = 0
    for off, length, frame_off in wad_offsets_seen:
        if off != expected:
            add(e.findings, CRIT, frame_off,
                f"WAD_CHUNK.offset={off} but engine 'received' counter was {expected} "
                f"(engine: chunk offset mismatch -> exit 1)")
            # Even one mismatch is fatal in the engine; reset our expected to
            # what would happen if the chunk had been accepted, so further
            # checks don't pile-on misleadingly.
        expected = off + length

    if e.wad_done is not None and e.hello is not None:
        if e.hello["wad_size"] != sum(len(b) for _, b, _ in e.wad_chunks):
            add(e.findings, CRIT, e.hello_msg.offset,
                f"HELLO.wad_size={e.hello['wad_size']} != sum of "
                f"WAD_CHUNK lengths={sum(len(b) for _, b, _ in e.wad_chunks)} "
                f"(engine: WAD overrun or WAD size mismatch -> exit 1)")
        if e.hello is not None and e.wad_done["total_size"] != e.hello["wad_size"]:
            add(e.findings, WARN, e.wad_done and (parsed.messages and 0) or 0,
                f"WAD_DONE.total_size={e.wad_done['total_size']} != "
                f"HELLO.wad_size={e.hello['wad_size']} "
                f"(engine ignores WAD_DONE.total_size; it uses HELLO.wad_size)")
    elif e.hello is not None and e.wad_done is None and e.wad_chunks:
        add(e.findings, CRIT, e.hello_msg.offset,
            "WAD_DONE missing; engine would block waiting for it (hang in fuzz)")
    elif e.hello is None:
        add(e.findings, CRIT, 0, "no HELLO at all (engine: bad HELLO -> exit 1)")

    e.wad_bytes, e.wad_assembly_info = reassemble_wad(
        [(o, b) for o, b, _ in e.wad_chunks],
        e.wad_done["total_size"] if e.wad_done else None,
    )
    return e


def reassemble_wad(parts, declared_size):
    if not parts:
        return b"", "no WAD_CHUNK messages"
    end = max(off + len(b) for off, b in parts)
    if declared_size is not None:
        end = max(end, declared_size)
    buf = bytearray(end)
    covered = [False] * end
    overlaps = 0
    for off, b in parts:
        if off + len(b) > end:
            continue
        for i in range(off, off + len(b)):
            if covered[i]:
                overlaps += 1
            covered[i] = True
        buf[off:off + len(b)] = b
    gaps = covered.count(False)
    return bytes(buf), (f"{len(parts)} chunk(s), {end} bytes total, "
                       f"{gaps} gap-bytes zero-filled, {overlaps} overlap-bytes")


# -----------------------------------------------------------------------------
# Canonical re-encode for the byte-faithful verdict
# -----------------------------------------------------------------------------

def encode_frame(mtype: int, payload: bytes) -> bytes:
    return struct.pack("<BI", mtype, len(payload)) + payload


def reencode_canonical(e: Extracted) -> bytes:
    """Reproduce what a real doom-client would send given the extracted
    components. Used only to compute the byte-faithful verdict; the verdict
    is YES iff this is byte-identical to the original corpus."""
    out = []

    if e.hello is not None:
        name = e.hello["wad_name"]
        nlen = e.hello["wad_nlen"]
        # Use the on-wire wad_size as the client would, but its actual value
        # depends on what extracted components we have. For faithful replay we
        # take the original HELLO field exactly.
        payload  = struct.pack("<HHHIH",
                               e.hello["proto"], e.hello["screen_w"],
                               e.hello["screen_h"], e.hello["caps"], nlen)
        # name on wire is exactly the bytes that were in HELLO (we use the
        # raw bytes, not the sanitized form).
        payload += name[:nlen] if len(name) >= nlen else name + b"\0" * (nlen - len(name))
        payload += struct.pack("<I", e.hello["wad_size"])
        out.append(encode_frame(MSG_HELLO, payload))

    if e.args:
        ap = struct.pack("<H", len(e.args))
        for s in e.args:
            ap += struct.pack("<H", len(s)) + s
        out.append(encode_frame(MSG_ARGS, ap))

    if e.config is not None:
        out.append(encode_frame(MSG_CONFIG, e.config))

    # Chunks: client always sends sequential 256K-aligned offsets.
    wad = e.wad_bytes
    if e.wad_chunks:
        off = 0
        N = DOOMNET_WAD_CHUNK
        while off < len(wad):
            slice_ = wad[off:off + N]
            out.append(encode_frame(MSG_WAD_CHUNK, struct.pack("<I", off) + slice_))
            off += len(slice_)

    if e.wad_done is not None:
        out.append(encode_frame(MSG_WAD_DONE,
                                struct.pack("<I", e.wad_done["total_size"])))

    return b"".join(out)


def compute_verdict(corpus: bytes, e: Extracted):
    canonical = reencode_canonical(e)
    if canonical == corpus:
        return True, "YES", "extracted components round-trip byte-identically"

    # Prefer a CRITICAL finding as the deciding reason if one exists.
    for sev, off, msg in e.findings:
        if sev == CRIT:
            return False, "NO", f"{msg} @ byte 0x{off:08x}"
    for sev, off, msg in e.findings:
        if sev == WARN:
            return False, "NO", f"{msg} @ byte 0x{off:08x}"

    # No graded finding fired but the streams differ - usually canonical
    # ordering / chunking. Report the first differing byte.
    diff_at = next((i for i in range(min(len(canonical), len(corpus)))
                   if canonical[i] != corpus[i]), min(len(canonical), len(corpus)))
    return False, "NO", (f"canonical re-encode differs at byte 0x{diff_at:08x} "
                         f"(canonical_len={len(canonical)}, corpus_len={len(corpus)}) "
                         f"- most likely WAD chunking, message ordering, or "
                         f"trailing-byte differences")


# -----------------------------------------------------------------------------
# Output
# -----------------------------------------------------------------------------

def write_outputs(args, corpus: bytes, e: Extracted, verdict_bool, verdict_label, verdict_reason):
    out_dir = args.out_dir
    os.makedirs(out_dir, exist_ok=True)

    wad_name = "replay.wad"
    if e.hello is not None:
        wad_name = sanitize_basename(e.hello["wad_name"])
    wad_path = os.path.join(out_dir, wad_name)
    with open(wad_path, "wb") as f:
        f.write(e.wad_bytes)

    cfg_path = None
    if e.config is not None:
        cfg_path = os.path.join(out_dir, "default.cfg")
        with open(cfg_path, "wb") as f:
            f.write(e.config)

    args_path = os.path.join(out_dir, "args.txt")
    with open(args_path, "wb") as f:
        for a in e.args:
            f.write(a + b"\n")

    hello_path = os.path.join(out_dir, "hello.txt")
    with open(hello_path, "w") as f:
        if e.hello is None:
            f.write("(no HELLO in stream)\n")
        else:
            h = e.hello
            f.write(f"proto      = {h['proto']}\n")
            f.write(f"screen_w   = {h['screen_w']}\n")
            f.write(f"screen_h   = {h['screen_h']}\n")
            f.write(f"caps       = 0x{h['caps']:08x}\n")
            f.write(f"wad_nlen   = {h['wad_nlen']}\n")
            f.write(f"wad_name   = {h['wad_name']!r}\n")
            f.write(f"wad_size   = {h['wad_size']} (HELLO field)\n")
            if e.wad_done is not None:
                f.write(f"wad_done   = {e.wad_done['total_size']} (WAD_DONE field)\n")
            f.write(f"sanitized  = {wad_name}\n")

    write_report(args, corpus, e, verdict_bool, verdict_label, verdict_reason,
                 wad_path, cfg_path)
    write_replay(args, e, wad_name, cfg_path, verdict_bool)
    print(f"wrote {wad_path} ({len(e.wad_bytes)} bytes)")
    if cfg_path:
        print(f"wrote {cfg_path} ({len(e.config)} bytes)")
    print(f"wrote {args_path} ({len(e.args)} args)")
    print(f"wrote {hello_path}")
    print(f"wrote {os.path.join(out_dir, 'report.txt')}")
    print(f"wrote {os.path.join(out_dir, 'replay.sh')}")
    print(f"verdict: BYTE-FAITHFUL VIA doom-client REPLAY: {verdict_label}")
    print(f"         reason: {verdict_reason}")
    if not verdict_bool:
        print("         use tools/fuzz/replay_raw.py for byte-faithful TCP replay")


def write_report(args, corpus, e, verdict_bool, verdict_label, verdict_reason,
                 wad_path, cfg_path):
    path = os.path.join(args.out_dir, "report.txt")
    crit = [x for x in e.findings if x[0] == CRIT]
    warn = [x for x in e.findings if x[0] == WARN]
    info = [x for x in e.findings if x[0] == INFO]

    with open(path, "w") as f:
        f.write("===========================================================\n")
        f.write(f"verdict: BYTE-FAITHFUL VIA doom-client REPLAY: {verdict_label}\n")
        f.write(f"         reason: {verdict_reason}\n")
        if not verdict_bool:
            f.write( "         For exact reproduction, use:\n")
            f.write( "             tools/fuzz/replay_raw.py <this-corpus-file> --host 127.0.0.1 --port 6666\n")
        f.write("===========================================================\n\n")

        f.write(f"input            : {args.input}\n")
        f.write(f"input_size       : {len(corpus)} bytes\n")
        f.write(f"frames_parsed    : {e.frames_parsed}\n")
        f.write(f"hello            : {'yes' if e.hello else 'no'}\n")
        f.write(f"args_count       : {len(e.args)}\n")
        f.write(f"config_present   : {'yes' if e.config else 'no'}\n")
        if e.config is not None:
            f.write(f"config_bytes     : {len(e.config)}\n")
        f.write(f"wad_chunks       : {len(e.wad_chunks)}\n")
        f.write(f"wad_done         : "
                f"{e.wad_done['total_size'] if e.wad_done else 'absent'}\n")
        f.write(f"wad_assembly     : {e.wad_assembly_info}\n")
        f.write(f"wad_out_bytes    : {len(e.wad_bytes)}\n")
        f.write(f"input_events     : {e.events}  (post-WAD_DONE MSG_INPUT_EVENT count)\n")
        f.write(f"counts           : {len(crit)} CRITICAL, {len(warn)} WARNING, "
                f"{len(info)} INFO\n\n")

        for label, items in (("CRITICAL findings (reproduction via doom-client will NOT match)",  crit),
                             ("WARNING findings (reproduction probably differs in detail)",       warn),
                             ("INFO findings (informational, harmless or ignored by engine)",     info)):
            f.write(f"--- {label} ---\n")
            if not items:
                f.write("  (none)\n")
            for _, off, msg in items:
                f.write(f"  @0x{off:08x}: {msg}\n")
            f.write("\n")


def write_replay(args, e, wad_name, cfg_path, verdict_bool):
    path      = os.path.join(args.out_dir, "replay.sh")
    out_abs   = os.path.abspath(args.out_dir)
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    corpus_abs = os.path.abspath(args.input)

    str_args = []
    for a in e.args:
        try:
            str_args.append(a.decode("ascii"))
        except UnicodeDecodeError:
            str_args.append(a.decode("ascii", "replace"))

    skill_val = None
    warp_a = warp_b = None
    leftover = []
    i = 0
    while i < len(str_args):
        a = str_args[i]
        if a == "-skill" and i + 1 < len(str_args):
            skill_val = str_args[i + 1]; i += 2; continue
        if a == "-warp" and i + 1 < len(str_args):
            warp_a = str_args[i + 1]
            if i + 2 < len(str_args) and not str_args[i + 2].startswith("-"):
                warp_b = str_args[i + 2]; i += 3
            else:
                i += 2
            continue
        leftover.append(a); i += 1

    parts = [
        '"$ROOT/build/doom-client"',
        f"    --wad {out_abs}/{wad_name}",
    ]
    if cfg_path is not None:
        parts.append(f"    --config {out_abs}/default.cfg")
    if skill_val is not None:
        parts.append(f"    --skill {skill_val}")
    if warp_a is not None:
        parts.append(f"    --warp {warp_a} {warp_b}" if warp_b else f"    --warp {warp_a}")
    parts.append('    --host 127.0.0.1 --port "$PORT"')
    client_cmd = " \\\n".join(parts)

    captured = " ".join(str_args) if str_args else "(none)"
    leftovers = " ".join(leftover) if leftover else "(none)"
    verdict_note = (
        "# BYTE-FAITHFUL replay via doom-client: YES.\n"
        "# The doom-client invocation below should reproduce the engine\n"
        "# state of the original corpus exactly.\n"
        if verdict_bool else
        "# BYTE-FAITHFUL replay via doom-client: NO.\n"
        "# The doom-client path WILL NOT reproduce the original engine\n"
        "# state - it canonicalizes the wire stream. Use replay_raw.py\n"
        "# below for exact reproduction. doom-client is included only as\n"
        "# a 'closest valid client behavior' approximation.\n"
    )

    text = (
        "#!/usr/bin/env bash\n"
        "# Generated by split_corpus.py\n"
        "#\n"
        f"{verdict_note}"
        "set -eu\n"
        f'ROOT="${{ROOT:-{repo_root}}}"\n'
        'PORT="${PORT:-6666}"\n'
        f'CORPUS="${{CORPUS:-{corpus_abs}}}"\n'
        '\n'
        '# 1. Start the engine supervisor in the background:\n'
        '"$ROOT/build/doom-engine" -port "$PORT" &\n'
        'ENGINE_PID=$!\n'
        "trap 'kill -TERM \"$ENGINE_PID\" 2>/dev/null || true' EXIT\n"
        'sleep 0.3\n'
        '\n'
        '# ---- OPTION A: byte-faithful raw replay -----------------------\n'
        '# Streams the corpus bytes verbatim to the engine. Always exact.\n'
        f'python3 "$ROOT/tools/fuzz/replay_raw.py" "$CORPUS" \\\n'
        '    --host 127.0.0.1 --port "$PORT"\n'
        '\n'
        '# ---- OPTION B: doom-client replay (interpretive) --------------\n'
        '# Uncomment to drive the engine with the extracted components.\n'
        '# Args from MSG_ARGS in the corpus:\n'
        f'#       {captured}\n'
        f'#    Client has no direct flag for: {leftovers}\n'
        '#\n'
    )
    # Comment out the client cmd lines if verdict was NO so the user has to
    # opt in explicitly.
    if verdict_bool:
        text += client_cmd + "\n"
    else:
        for line in client_cmd.splitlines():
            text += "# " + line + "\n"

    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, 0o755)


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input",   help="binary corpus/crash/hang file")
    ap.add_argument("out_dir", help="directory to write components into")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    parsed = parse_stream(data)
    e      = extract(parsed, data)
    ok, label, reason = compute_verdict(data, e)
    write_outputs(args, data, e, ok, label, reason)
    return 0


if __name__ == "__main__":
    sys.exit(main())
