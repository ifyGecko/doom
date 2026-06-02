// -----------------------------------------------------------------------------
// shmpath_preload.so - fuzz-iteration shim
// -----------------------------------------------------------------------------
//
// Two jobs:
//
// 1. Path rewrite: redirect /tmp/doom-net-* to /dev/shm/doom-net-*.
//    The engine's I_NetBootstrap creates a per-session scratch dir at
//    /tmp/doom-net-<pid>/ and writes the uploaded WAD and optional
//    default.cfg there. We move those to tmpfs without touching the
//    engine source.
//
// 2. Drop writes to fd 0: under the harness (__wrap_I_Supervise in
//    harness_wraps.c) the engine's client_fd is set to 0, which in the
//    AFL child is the read-only stdin pipe carrying the test case. The
//    engine writes HELLO_ACK / WAD_ACK / frames back to "the client",
//    but a write to a read-only pipe returns EBADF and trips the
//    engine's die() paths after the first chunk, terminating the
//    iteration before R_Init / P_Init ever run. Discard those writes
//    here so the engine sails through the entire upload and parses
//    every byte of the WAD.
//
// 3. Translate recv/recvfrom on fd 0 to read(): the engine uses
//    recv() (framing.c) but fd 0 is a pipe, not a socket, so the
//    bare recv would fail with ENOTSOCK on the very first read.
//
// Intercepted symbols:
//   open, open64       - direct callers (w_wad.c uses open() on the WAD)
//   openat
//   fopen, fopen64     - i_video_net.c, m_misc.c (M_LoadDefaults/M_SaveDefaults)
//   mkdir              - i_video_net.c creates the temp dir
//   rmdir, unlink      - i_video_net.c cleanup_temp
//   access, stat, lstat, __xstat, __lxstat - existence checks (d_main.c, etc.)
//   write              - discard if fd == 0
//   writev             - discard if fd == 0
//   send               - discard if fd == 0
//   recv, recvfrom     - translate to read() if fd == 0 (pipe, not socket)
//
// -----------------------------------------------------------------------------

// We define both open() and open64() (and fopen/fopen64) as separate symbols
// so dlsym(RTLD_NEXT, ...) on each variant returns the matching libc symbol.
// glibc with _FILE_OFFSET_BITS=64 makes open() an asm-level alias of open64,
// which collides with our two definitions. Drop LFS in this TU only - the
// engine's own TUs keep the meson default.
#undef _FILE_OFFSET_BITS
#undef _TIME_BITS
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

static const char SRC_PREFIX[] = "/tmp/doom-net-";
static const char DST_PREFIX[] = "/dev/shm/doom-net-";

// Returns the (possibly rewritten) path. If rewriting, copies into `buf`
// and returns `buf`; otherwise returns the original `path`. `buf` must be
// at least PATH_MAX bytes.
static const char *rewrite(const char *path, char *buf, size_t bufsz)
{
    size_t src_len = sizeof(SRC_PREFIX) - 1;
    size_t dst_len = sizeof(DST_PREFIX) - 1;
    size_t tail_len;

    if (path == NULL) return path;
    if (strncmp(path, SRC_PREFIX, src_len) != 0) return path;

    tail_len = strlen(path + src_len);
    if (dst_len + tail_len + 1 > bufsz) return path;  // too long; punt

    memcpy(buf, DST_PREFIX, dst_len);
    memcpy(buf + dst_len, path + src_len, tail_len + 1);
    return buf;
}

// ---- open / open64 / openat ------------------------------------------------

typedef int (*open_fn_t)(const char *, int, ...);
typedef int (*openat_fn_t)(int, const char *, int, ...);

int open(const char *pathname, int flags, ...)
{
    static open_fn_t real_open = NULL;
    char             buf[4096];
    const char      *p;
    mode_t           mode = 0;

    if (!real_open) real_open = (open_fn_t)dlsym(RTLD_NEXT, "open");

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    p = rewrite(pathname, buf, sizeof buf);
    return real_open(p, flags, mode);
}

int open64(const char *pathname, int flags, ...)
{
    static open_fn_t real_open64 = NULL;
    char             buf[4096];
    const char      *p;
    mode_t           mode = 0;

    if (!real_open64) real_open64 = (open_fn_t)dlsym(RTLD_NEXT, "open64");
    if (!real_open64) {
        // Older glibc without open64 - fall back to open().
        if (flags & (O_CREAT | O_TMPFILE)) {
            va_list ap;
            va_start(ap, flags);
            mode = (mode_t)va_arg(ap, int);
            va_end(ap);
        }
        p = rewrite(pathname, buf, sizeof buf);
        return open(p, flags, mode);
    }

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    p = rewrite(pathname, buf, sizeof buf);
    return real_open64(p, flags, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
    static openat_fn_t real_openat = NULL;
    char               buf[4096];
    const char        *p;
    mode_t             mode = 0;

    if (!real_openat) real_openat = (openat_fn_t)dlsym(RTLD_NEXT, "openat");

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    p = rewrite(pathname, buf, sizeof buf);
    return real_openat(dirfd, p, flags, mode);
}

// ---- fopen / fopen64 -------------------------------------------------------

typedef FILE *(*fopen_fn_t)(const char *, const char *);

FILE *fopen(const char *pathname, const char *mode)
{
    static fopen_fn_t real_fopen = NULL;
    char              buf[4096];
    const char       *p;

    if (!real_fopen) real_fopen = (fopen_fn_t)dlsym(RTLD_NEXT, "fopen");
    p = rewrite(pathname, buf, sizeof buf);
    return real_fopen(p, mode);
}

FILE *fopen64(const char *pathname, const char *mode)
{
    static fopen_fn_t real_fopen64 = NULL;
    char              buf[4096];
    const char       *p;

    if (!real_fopen64) real_fopen64 = (fopen_fn_t)dlsym(RTLD_NEXT, "fopen64");
    if (!real_fopen64) return fopen(pathname, mode);
    p = rewrite(pathname, buf, sizeof buf);
    return real_fopen64(p, mode);
}

// ---- mkdir / rmdir / unlink ------------------------------------------------

typedef int (*mkdir_fn_t)(const char *, mode_t);
typedef int (*rmdir_fn_t)(const char *);
typedef int (*unlink_fn_t)(const char *);

int mkdir(const char *pathname, mode_t mode)
{
    static mkdir_fn_t real_mkdir = NULL;
    char              buf[4096];
    const char       *p;
    if (!real_mkdir) real_mkdir = (mkdir_fn_t)dlsym(RTLD_NEXT, "mkdir");
    p = rewrite(pathname, buf, sizeof buf);
    return real_mkdir(p, mode);
}

int rmdir(const char *pathname)
{
    static rmdir_fn_t real_rmdir = NULL;
    char              buf[4096];
    const char       *p;
    if (!real_rmdir) real_rmdir = (rmdir_fn_t)dlsym(RTLD_NEXT, "rmdir");
    p = rewrite(pathname, buf, sizeof buf);
    return real_rmdir(p);
}

int unlink(const char *pathname)
{
    static unlink_fn_t real_unlink = NULL;
    char               buf[4096];
    const char        *p;
    if (!real_unlink) real_unlink = (unlink_fn_t)dlsym(RTLD_NEXT, "unlink");
    p = rewrite(pathname, buf, sizeof buf);
    return real_unlink(p);
}

// ---- access / stat (existence checks) --------------------------------------
//
// d_main.c does `access(iwad, R_OK)` to verify the -iwad path before
// W_AddFile opens it. Engine also has `access(lbmname, 0)` for screenshot
// path. Rewrite the path so the check sees the file we created in /dev/shm.

typedef int (*access_fn_t)(const char *, int);
typedef int (*stat_fn_t)(const char *, struct stat *);
typedef int (*xstat_fn_t)(int, const char *, struct stat *);

int access(const char *pathname, int mode)
{
    static access_fn_t real_access = NULL;
    char               buf[4096];
    const char        *p;
    if (!real_access) real_access = (access_fn_t)dlsym(RTLD_NEXT, "access");
    p = rewrite(pathname, buf, sizeof buf);
    return real_access(p, mode);
}

int stat(const char *pathname, struct stat *st)
{
    static stat_fn_t real_stat = NULL;
    char             buf[4096];
    const char      *p;
    if (!real_stat) real_stat = (stat_fn_t)dlsym(RTLD_NEXT, "stat");
    p = rewrite(pathname, buf, sizeof buf);
    return real_stat(p, st);
}

int lstat(const char *pathname, struct stat *st)
{
    static stat_fn_t real_lstat = NULL;
    char             buf[4096];
    const char      *p;
    if (!real_lstat) real_lstat = (stat_fn_t)dlsym(RTLD_NEXT, "lstat");
    p = rewrite(pathname, buf, sizeof buf);
    return real_lstat(p, st);
}

// Older glibc routes stat() through __xstat(version, path, st). Catch that
// path too for any TU still compiled against old headers.
int __xstat(int ver, const char *pathname, struct stat *st)
{
    static xstat_fn_t real_xstat = NULL;
    char              buf[4096];
    const char       *p;
    if (!real_xstat) real_xstat = (xstat_fn_t)dlsym(RTLD_NEXT, "__xstat");
    if (!real_xstat) return stat(pathname, st);
    p = rewrite(pathname, buf, sizeof buf);
    return real_xstat(ver, p, st);
}

int __lxstat(int ver, const char *pathname, struct stat *st)
{
    static xstat_fn_t real_lxstat = NULL;
    char              buf[4096];
    const char       *p;
    if (!real_lxstat) real_lxstat = (xstat_fn_t)dlsym(RTLD_NEXT, "__lxstat");
    if (!real_lxstat) return lstat(pathname, st);
    p = rewrite(pathname, buf, sizeof buf);
    return real_lxstat(ver, p, st);
}

// ---- write / writev / send  (drop sends-to-the-client) ---------------------
//
// Under the harness, fd 0 IS the "client socket" from the engine's
// perspective (set by __wrap_I_Supervise in harness_wraps.c). In the AFL
// child, fd 0 is the read end of a pipe carrying the test case, so any
// actual write would EBADF and trip the engine's die() paths long before
// R_Init runs. Pretend the write fully succeeded.

typedef ssize_t (*write_fn_t)(int, const void *, size_t);
typedef ssize_t (*writev_fn_t)(int, const struct iovec *, int);
typedef ssize_t (*send_fn_t)(int, const void *, size_t, int);

ssize_t write(int fd, const void *buf, size_t count)
{
    static write_fn_t real_write = NULL;
    if (fd == 0) return (ssize_t)count;
    if (!real_write) real_write = (write_fn_t)dlsym(RTLD_NEXT, "write");
    return real_write(fd, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    static writev_fn_t real_writev = NULL;
    if (fd == 0) {
        ssize_t total = 0;
        int     i;
        for (i = 0; i < iovcnt; i++) total += (ssize_t)iov[i].iov_len;
        return total;
    }
    if (!real_writev) real_writev = (writev_fn_t)dlsym(RTLD_NEXT, "writev");
    return real_writev(fd, iov, iovcnt);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    static send_fn_t real_send = NULL;
    if (sockfd == 0) return (ssize_t)len;
    if (!real_send) real_send = (send_fn_t)dlsym(RTLD_NEXT, "send");
    return real_send(sockfd, buf, len, flags);
}

// ---- recv  (fd 0 is a pipe, not a socket - fall back to read) --------------
//
// The engine uses recv() for client reads (framing.c: net_read_exact and
// net_try_recv). Under fuzzing fd 0 is AFL's input pipe, and recv() on a
// non-socket returns ENOTSOCK. Translate recv-on-fd-0 to read().

typedef ssize_t (*recv_fn_t)(int, void *, size_t, int);
typedef ssize_t (*recvfrom_fn_t)(int, void *, size_t, int,
                                 struct sockaddr *, socklen_t *);
typedef ssize_t (*read_fn_t)(int, void *, size_t);

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    static recv_fn_t real_recv = NULL;
    static read_fn_t real_read = NULL;
    if (sockfd == 0) {
        if (!real_read) real_read = (read_fn_t)dlsym(RTLD_NEXT, "read");
        (void)flags;
        return real_read(sockfd, buf, len);
    }
    if (!real_recv) real_recv = (recv_fn_t)dlsym(RTLD_NEXT, "recv");
    return real_recv(sockfd, buf, len, flags);
}

// glibc commonly implements recv() in terms of recvfrom() and the engine's
// recv() call can be resolved directly to the recvfrom syscall, bypassing
// our recv() hook. Catch it here too.
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    static recvfrom_fn_t real_recvfrom = NULL;
    static read_fn_t     real_read     = NULL;
    if (sockfd == 0) {
        if (!real_read) real_read = (read_fn_t)dlsym(RTLD_NEXT, "read");
        (void)flags; (void)src_addr; (void)addrlen;
        return real_read(sockfd, buf, len);
    }
    if (!real_recvfrom) real_recvfrom = (recvfrom_fn_t)dlsym(RTLD_NEXT, "recvfrom");
    return real_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}
