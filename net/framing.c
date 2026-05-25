// -----------------------------------------------------------------------------
// TCP framing helpers - implementation
// -----------------------------------------------------------------------------

#include "framing.h"
#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

// Wait up to timeout_ms for fd to become writable. Used to convert EAGAIN
// on a non-blocking send into a brief pause rather than a hard failure.
static int wait_writable(int fd, int timeout_ms)
{
    struct pollfd p;
    int r;
    p.fd = fd;
    p.events = POLLOUT;
    do { r = poll(&p, 1, timeout_ms); } while (r < 0 && errno == EINTR);
    if (r <= 0) return -1;
    return (p.revents & POLLOUT) ? 0 : -1;
}

// Write entire iovec. Loops on partial writes and EAGAIN.
static int writev_all(int fd, struct iovec *iov, int iov_n, size_t total)
{
    size_t sent = 0;
    int idx = 0;
    while (sent < total) {
        ssize_t w = writev(fd, &iov[idx], iov_n - idx);
        if (w > 0) {
            sent += (size_t)w;
            size_t left = (size_t)w;
            while (left && idx < iov_n) {
                if (left >= iov[idx].iov_len) {
                    left -= iov[idx].iov_len;
                    idx++;
                } else {
                    iov[idx].iov_base = (uint8_t *)iov[idx].iov_base + left;
                    iov[idx].iov_len -= left;
                    left = 0;
                }
            }
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_writable(fd, 2000) < 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static void write_header(uint8_t hdr[DOOMNET_HEADER_BYTES], uint8_t type, uint32_t len)
{
    hdr[0] = type;
    hdr[1] = (uint8_t)(len);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len >> 16);
    hdr[4] = (uint8_t)(len >> 24);
}

int net_send(int fd, uint8_t type, const void *payload, uint32_t len)
{
    uint8_t hdr[DOOMNET_HEADER_BYTES];
    struct iovec iov[2];

    if (len > DOOMNET_MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    write_header(hdr, type, len);

    iov[0].iov_base = hdr;
    iov[0].iov_len  = DOOMNET_HEADER_BYTES;
    iov[1].iov_base = (void *)payload;
    iov[1].iov_len  = len;
    return writev_all(fd, iov, 2, DOOMNET_HEADER_BYTES + len);
}

int net_send2(int fd, uint8_t type,
              const void *p1, uint32_t n1,
              const void *p2, uint32_t n2)
{
    uint8_t hdr[DOOMNET_HEADER_BYTES];
    struct iovec iov[3];
    uint32_t len = n1 + n2;

    if (len > DOOMNET_MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    write_header(hdr, type, len);

    iov[0].iov_base = hdr;
    iov[0].iov_len  = DOOMNET_HEADER_BYTES;
    iov[1].iov_base = (void *)p1;
    iov[1].iov_len  = n1;
    iov[2].iov_base = (void *)p2;
    iov[2].iov_len  = n2;
    return writev_all(fd, iov, 3, DOOMNET_HEADER_BYTES + len);
}

int net_read_exact(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r > 0) { p += r; n -= (size_t)r; got += (size_t)r; continue; }
        if (r == 0) return got == 0 ? 0 : -1;   // EOF mid-read = error
        if (errno == EINTR) continue;
        return -1;
    }
    return 1;
}

int net_recv_blocking(int fd, void *buf, size_t cap,
                      uint8_t *out_type, uint32_t *out_len)
{
    uint8_t hdr[DOOMNET_HEADER_BYTES];
    uint32_t len;
    int r = net_read_exact(fd, hdr, DOOMNET_HEADER_BYTES);
    if (r <= 0) return r;
    *out_type = hdr[0];
    len = (uint32_t)hdr[1]
        | ((uint32_t)hdr[2] << 8)
        | ((uint32_t)hdr[3] << 16)
        | ((uint32_t)hdr[4] << 24);
    if (len > cap || len > DOOMNET_MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    *out_len = len;
    if (len == 0) return 1;
    return net_read_exact(fd, buf, len) > 0 ? 1 : -1;
}

int net_try_recv(int fd, net_rx_t *rx,
                 uint8_t *out_type, uint32_t *out_len,
                 const uint8_t **out_payload)
{
    uint8_t  type;
    uint32_t len;
    size_t   total;

    // Reclaim space that was handed out (and is therefore now stale) by the
    // previous successful call. Doing this here, BEFORE we touch the buffer,
    // is what makes the documented "valid until next call" contract hold.
    if (rx->consumed > 0) {
        size_t remaining = rx->have - rx->consumed;
        if (remaining) memmove(rx->buf, rx->buf + rx->consumed, remaining);
        rx->have     = remaining;
        rx->consumed = 0;
    }

    // Pull whatever is immediately available into rx->buf.
    while (rx->have < rx->cap) {
        ssize_t r = recv(fd, rx->buf + rx->have, rx->cap - rx->have, 0);
        if (r > 0) { rx->have += (size_t)r; continue; }
        if (r == 0) return -1;                            // EOF
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        return -1;
    }

    if (rx->have < DOOMNET_HEADER_BYTES) return 0;
    type = rx->buf[0];
    len  = (uint32_t)rx->buf[1]
         | ((uint32_t)rx->buf[2] << 8)
         | ((uint32_t)rx->buf[3] << 16)
         | ((uint32_t)rx->buf[4] << 24);
    if (len > DOOMNET_MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    total = DOOMNET_HEADER_BYTES + len;
    if (total > rx->cap) { errno = EMSGSIZE; return -1; }
    if (rx->have < total) return 0;

    *out_type    = type;
    *out_len     = len;
    *out_payload = rx->buf + DOOMNET_HEADER_BYTES;

    // Mark the bytes as handed-out. They stay in place (so *out_payload is
    // valid) and will be discarded by the compaction at the top of the
    // next call.
    rx->consumed = total;
    return 1;
}

int net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int net_set_nodelay(int fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}
