// -----------------------------------------------------------------------------
// TCP message framing helpers, shared between doom-engine and doom-client.
// -----------------------------------------------------------------------------

#ifndef DOOMNET_FRAMING_H
#define DOOMNET_FRAMING_H

#include <stddef.h>
#include <stdint.h>

// Receive buffer state for non-blocking framed reads. The caller owns the
// backing buffer; net_try_recv manages 'have' and 'consumed'.
//
// 'consumed' tracks bytes whose ownership has been handed back to the
// caller (via a previous successful net_try_recv) but not yet reclaimed.
// Compaction happens at the START of the NEXT net_try_recv call, so a
// payload pointer returned by net_try_recv stays valid until the caller
// invokes net_try_recv again on the same rx.
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   have;
    size_t   consumed;
} net_rx_t;

// Blocking send. Returns 0 on success, -1 on error (errno set).
// Uses writev so header+payload reach the wire as a single syscall when
// the kernel buffer has room.
int net_send(int fd, uint8_t type, const void *payload, uint32_t len);

// Blocking send of a message whose payload is two adjacent buffers. Used
// to send (frame-flags + pixels) without an intermediate copy.
int net_send2(int fd, uint8_t type,
              const void *p1, uint32_t n1,
              const void *p2, uint32_t n2);

// Blocking read of exactly n bytes from a blocking fd.
// Returns 1 on success, 0 on clean EOF, -1 on error.
int net_read_exact(int fd, void *buf, size_t n);

// Blocking read of one whole message. Uses caller-provided buf as payload
// storage. Returns 1 on success, 0 on EOF before any header byte, -1 on
// error or oversized payload.
int net_recv_blocking(int fd, void *buf, size_t cap,
                      uint8_t *out_type, uint32_t *out_len);

// Non-blocking attempt to extract one message from rx (after pulling any
// available bytes from fd into rx->buf).
//   returns  1 = message extracted; *out_type/*out_len/*out_payload set.
//                The payload pointer is valid until the next call.
//   returns  0 = no full message yet (would block).
//   returns -1 = error or EOF.
//
// Requires fd to be in non-blocking mode.
int net_try_recv(int fd, net_rx_t *rx,
                 uint8_t *out_type, uint32_t *out_len,
                 const uint8_t **out_payload);

// Switch fd to non-blocking mode and enable TCP_NODELAY.
int net_set_nonblocking(int fd);
int net_set_nodelay(int fd);

#endif
