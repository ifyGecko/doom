// -----------------------------------------------------------------------------
// Engine-side initialization logger.
//
// Provides L_Infof / L_Warnf / L_Errorf, printf-style helpers used during the
// engine's init phase (I_NetBootstrap through I_InitGraphics). Each call:
//
//   1. Writes the formatted line to the engine's own stdout/stderr, exactly
//      like the original printf it replaces, so the engine terminal stays
//      informative regardless of client logging state.
//
//   2. If the connected client requested init logs by setting
//      DOOMNET_CAP_INIT_LOG in its HELLO.caps, frames the line as a
//      MSG_LOG message and ships it to the client.
//
// The wire side is bracketed by L_InitlogSetup (called once per session,
// from the supervisor child as soon as the client fd is known) and
// L_InitlogFinish (called from I_InitGraphics just before MSG_READY).
// After L_InitlogFinish, L_* calls degrade to plain stdout/stderr writes
// only - the game loop is explicitly out of scope for client-side logs.
// -----------------------------------------------------------------------------

#ifndef DOOM_I_INITLOG_H
#define DOOM_I_INITLOG_H

#include <stdint.h>

// Called by the supervisor child after I_NetSetSessionFds but before the
// HELLO is parsed. Stashes the client fd and records the session start
// timestamp. Safe to call once per process; subsequent calls are ignored.
void L_InitlogSetup(int client_fd);

// Called from I_NetBootstrap after HELLO has been parsed. If
// (caps & DOOMNET_CAP_INIT_LOG), wire-side log shipping is enabled until
// L_InitlogFinish. Anything L_*-logged before this point reaches stdout
// only.
void L_InitlogEnable(uint32_t caps);

// Called from I_InitGraphics just before MSG_READY. Emits a final
// "[init complete]" INFO line, then disables wire-side shipping. All
// subsequent L_* calls degrade to stdout/stderr only.
void L_InitlogFinish(void);

// Force any in-flight output to disk/terminal. Currently a thin wrapper
// over fflush; the wire side sends synchronously and so has nothing to
// flush. Exposed for I_Error to call before exit so a fatal init failure
// still leaves the engine terminal coherent.
void L_InitlogFlush(void);

// Printf-style emitters. Format spec follows printf(3). A trailing
// newline in the format string is harmless - the engine strips it before
// framing the MSG_LOG.
void L_Infof (const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void L_Warnf (const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void L_Errorf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
