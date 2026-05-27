// -----------------------------------------------------------------------------
// DOOM engine session supervisor.
//
// Owns the listening socket for the lifetime of the engine process. Each
// accepted client connection is handled in a freshly fork()ed child running
// a complete single-player DOOM (D_DoomMain). The supervisor itself never
// touches game state, so:
//
//   - Two clients can connect simultaneously and play with different WADs /
//     configurations / CLI args; each lives in its own address space with
//     its own copy of every engine global.
//
//   - A client disconnect (clean BYE, TCP reset, kill -9) tears down only
//     that child. The supervisor reaps it and stays listening.
//
//   - An engine I_Error() / SIGSEGV / SIGABRT inside one session kills only
//     that child. Other sessions are unaffected and new connections still
//     succeed.
//
// Per-session temp directories (/tmp/doom-net-<pid>) are normally cleaned by
// the child's atexit() handler, but that does not run on signal-induced
// death. Each child therefore reports its temp dir path back to the
// supervisor over a one-shot pipe; on SIGCHLD the supervisor rm -rf's that
// path (idempotent if the child already removed it).
//
// Shutdown (SIGINT / SIGTERM on the supervisor): stop accepting, SIGTERM all
// children, give them 200 ms to drain, SIGKILL stragglers, reap, exit.
// -----------------------------------------------------------------------------

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "wire.h"

// Forwards into i_video_net.c / d_main.c.
extern void I_NetSetSessionFds(int client_fd, int report_pipe_wfd);
extern void I_NetBootstrap(void);
extern void D_DoomMain(void);

// Engine's own argv mirror, also defined in m_argv.c.
extern int    myargc;
extern char **myargv;


// ---- per-session table ----------------------------------------------------

typedef struct {
    pid_t   pid;          // 0 = slot free
    int     report_fd;    // read end of child->parent pipe; -1 = none/closed
    char    tempdir[256]; // empty until child reports it
} session_t;

static session_t *sessions       = NULL;
static int        sessions_cap   = 0;
static int        sessions_count = 0;

static int sessions_grow(void)
{
    int       new_cap = sessions_cap ? sessions_cap * 2 : 16;
    session_t *p      = (session_t *)realloc(sessions, sizeof(*sessions) * new_cap);
    if (!p) return -1;
    {
        int i;
        for (i = sessions_cap; i < new_cap; i++) {
            p[i].pid       = 0;
            p[i].report_fd = -1;
            p[i].tempdir[0] = 0;
        }
    }
    sessions     = p;
    sessions_cap = new_cap;
    return 0;
}

static session_t *session_alloc(void)
{
    int i;
    for (i = 0; i < sessions_cap; i++) {
        if (sessions[i].pid == 0) return &sessions[i];
    }
    if (sessions_grow() < 0) return NULL;
    return &sessions[sessions_cap / 2];   // first newly-zeroed slot
}

static session_t *session_lookup(pid_t pid)
{
    int i;
    for (i = 0; i < sessions_cap; i++) {
        if (sessions[i].pid == pid) return &sessions[i];
    }
    return NULL;
}

static void session_free(session_t *s)
{
    if (s->report_fd >= 0) { close(s->report_fd); s->report_fd = -1; }
    s->pid          = 0;
    s->tempdir[0]   = 0;
}


// ---- recursive directory removal ------------------------------------------

// Single-level expected (one wad + one default.cfg in a flat dir), but recurse
// defensively so a future engine that drops extra files doesn't leak.
static void rmtree(const char *path)
{
    DIR           *d;
    struct dirent *e;

    d = opendir(path);
    if (!d) return;  // ENOENT is fine; child may have cleaned up already.

    while ((e = readdir(d)) != NULL) {
        char        child[512];
        struct stat st;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(child, sizeof child, "%s/%s", path, e->d_name)
            >= (int)sizeof child)
            continue;
        if (lstat(child, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) rmtree(child);
        else                     unlink(child);
    }
    closedir(d);
    rmdir(path);
}


// ---- pipe drain (capture tempdir path before/after child exits) -----------

// Read whatever the child wrote into the report pipe and stash it as the
// session's tempdir. Idempotent and safe to call multiple times (subsequent
// calls just see EOF). Returns 1 if EOF was reached, 0 otherwise.
static int drain_report(session_t *s)
{
    int closed_pipe = 0;

    if (s->report_fd < 0) return 1;

    for (;;) {
        char    buf[sizeof s->tempdir];
        ssize_t n;
        size_t  cur = strlen(s->tempdir);

        n = read(s->report_fd, buf, sizeof buf);
        if (n > 0) {
            size_t room = (sizeof s->tempdir) - 1 - cur;
            size_t take = (size_t)n < room ? (size_t)n : room;
            if (take > 0) {
                memcpy(s->tempdir + cur, buf, take);
                s->tempdir[cur + take] = 0;
            }
            continue;
        }
        if (n == 0) { closed_pipe = 1; break; }                // EOF
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;    // no more for now
        closed_pipe = 1; break;                                // hard error
    }
    if (closed_pipe) { close(s->report_fd); s->report_fd = -1; }
    return closed_pipe;
}


// ---- child reaper ---------------------------------------------------------

static void reap_children(void)
{
    for (;;) {
        int   status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        session_t *s;

        if (pid <= 0) return;

        s = session_lookup(pid);

        if (WIFEXITED(status)) {
            fprintf(stderr, "[supervisor] session pid=%d exited code=%d\n",
                    (int)pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[supervisor] session pid=%d killed by signal=%d%s\n",
                    (int)pid, WTERMSIG(status),
                    WCOREDUMP(status) ? " (core dumped)" : "");
        }

        if (s) {
            // Race window: SIGCHLD may fire before the main poll loop has
            // read the tempdir bytes the child wrote. Drain now so we don't
            // miss them.
            drain_report(s);
            if (s->tempdir[0]) rmtree(s->tempdir);
            sessions_count--;
            session_free(s);
        }
    }
}


// ---- supervisor signal disposition ----------------------------------------

static int supervisor_signalfd(void)
{
    sigset_t mask;
    int      sfd;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    // Block the signals so they are delivered via signalfd and never
    // interrupt syscalls or run in async-signal context.
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        fprintf(stderr, "[supervisor] sigprocmask: %s\n", strerror(errno));
        return -1;
    }

    sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        fprintf(stderr, "[supervisor] signalfd: %s\n", strerror(errno));
        return -1;
    }
    return sfd;
}


// ---- shutdown -------------------------------------------------------------

static void shutdown_sessions(void)
{
    int i;
    int alive;
    struct timespec ts;
    int wait_ms = 0;

    fprintf(stderr, "[supervisor] shutting down: terminating %d session(s)\n",
            sessions_count);

    for (i = 0; i < sessions_cap; i++) {
        if (sessions[i].pid > 0) kill(sessions[i].pid, SIGTERM);
    }

    // Give them up to ~200 ms to exit cleanly, reaping as they die.
    for (wait_ms = 0; wait_ms < 200; wait_ms += 20) {
        reap_children();
        alive = 0;
        for (i = 0; i < sessions_cap; i++)
            if (sessions[i].pid > 0) alive++;
        if (alive == 0) break;
        ts.tv_sec  = 0;
        ts.tv_nsec = 20 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }

    // SIGKILL anything still alive.
    for (i = 0; i < sessions_cap; i++) {
        if (sessions[i].pid > 0) {
            fprintf(stderr, "[supervisor] SIGKILL stubborn pid=%d\n",
                    (int)sessions[i].pid);
            kill(sessions[i].pid, SIGKILL);
        }
    }
    // Final reap pass.
    for (wait_ms = 0; wait_ms < 200; wait_ms += 20) {
        reap_children();
        alive = 0;
        for (i = 0; i < sessions_cap; i++)
            if (sessions[i].pid > 0) alive++;
        if (alive == 0) break;
        ts.tv_sec  = 0;
        ts.tv_nsec = 20 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }
}


// ---- per-child entry ------------------------------------------------------

// Runs in the child after fork(). Closes the listening fd and any fds the
// parent owns for other sessions, then hands the connected client fd off
// to the existing engine bootstrap and runs D_DoomMain. Never returns.
static void __attribute__((noreturn))
engine_run_session(int client_fd, int report_pipe_wfd, int listen_fd)
{
    int i;

    close(listen_fd);
    for (i = 0; i < sessions_cap; i++) {
        if (sessions[i].report_fd >= 0) close(sessions[i].report_fd);
    }

    // Stop sharing the supervisor's process group so SIGINT to the foreground
    // pgrp (e.g. Ctrl-C in the terminal that launched the supervisor) reaches
    // the supervisor only, which then decides what to do with children.
    setpgid(0, 0);

    // Restore default signal mask so the engine's existing signal(SIGINT,
    // I_Quit) wiring works as before.
    {
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
    }

    I_NetSetSessionFds(client_fd, report_pipe_wfd);
    I_NetBootstrap();
    D_DoomMain();          // never returns
    _exit(0);              // belt + suspenders
}


// ---- listener accept handling ---------------------------------------------

static void accept_one(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t          alen = sizeof addr;
    int                cfd;
    int                report_pipe[2];
    pid_t              pid;
    session_t         *s;

    cfd = accept(listen_fd, (struct sockaddr *)&addr, &alen);
    if (cfd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        fprintf(stderr, "[supervisor] accept: %s\n", strerror(errno));
        return;
    }

    if (pipe(report_pipe) < 0) {
        fprintf(stderr, "[supervisor] pipe: %s\n", strerror(errno));
        close(cfd);
        return;
    }
    // Parent's read end is non-blocking so drain_report never stalls the poll
    // loop.
    fcntl(report_pipe[0], F_SETFL, O_NONBLOCK);

    s = session_alloc();
    if (!s) {
        fprintf(stderr, "[supervisor] out of memory; refusing connection\n");
        close(report_pipe[0]); close(report_pipe[1]); close(cfd);
        return;
    }
    // Reserve the slot before fork so a fast SIGCHLD finds it.
    s->pid       = -1;
    s->report_fd = report_pipe[0];
    s->tempdir[0] = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[supervisor] fork: %s\n", strerror(errno));
        close(report_pipe[0]); close(report_pipe[1]); close(cfd);
        session_free(s);
        return;
    }
    if (pid == 0) {
        // Child.
        close(report_pipe[0]);
        engine_run_session(cfd, report_pipe[1], listen_fd);
        /* unreachable */
    }

    // Parent.
    s->pid = pid;
    close(report_pipe[1]);
    close(cfd);
    sessions_count++;

    fprintf(stderr, "[supervisor] accepted %s:%d pid=%d (active=%d)\n",
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port),
            (int)pid, sessions_count);
}


// ---- entry point ----------------------------------------------------------

static int parse_int_opt(int argc, char **argv, const char *flag, int fallback)
{
    int i;
    for (i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], flag)) return atoi(argv[i + 1]);
    return fallback;
}

static const char *parse_str_opt(int argc, char **argv,
                                 const char *flag, const char *fallback)
{
    int i;
    for (i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], flag)) return argv[i + 1];
    return fallback;
}

int I_Supervise(int argc, char **argv)
{
    int                 listen_fd;
    int                 sfd;
    int                 port;
    const char         *bind_addr;
    int                 one = 1;
    struct sockaddr_in  addr;
    int                 stopping = 0;

    port      = parse_int_opt(argc, argv, "-port", DOOMNET_DEFAULT_PORT);
    bind_addr = parse_str_opt(argc, argv, "-bind", "0.0.0.0");

    signal(SIGPIPE, SIG_IGN);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "[supervisor] socket: %s\n", strerror(errno));
        return 1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fcntl(listen_fd, F_SETFD, FD_CLOEXEC);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "[supervisor] bad -bind '%s'\n", bind_addr);
        return 1;
    }
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "[supervisor] bind %s:%d: %s\n",
                bind_addr, port, strerror(errno));
        return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        fprintf(stderr, "[supervisor] listen: %s\n", strerror(errno));
        return 1;
    }

    sfd = supervisor_signalfd();
    if (sfd < 0) return 1;

    fprintf(stderr, "[supervisor] listening on %s:%d (pid=%d)\n",
            bind_addr, port, (int)getpid());

    // Main loop: poll listen_fd + signalfd + every session's report pipe.
    for (;;) {
        struct pollfd  pfds[4 + 64];     // grown on demand if needed
        struct pollfd *pfd_dyn = NULL;
        struct pollfd *pfd     = pfds;
        nfds_t         nfds    = 0;
        int            i, r;
        int            need;

        need = 2 + sessions_cap;
        if (need > (int)(sizeof pfds / sizeof pfds[0])) {
            pfd_dyn = (struct pollfd *)malloc(sizeof(struct pollfd) * need);
            if (!pfd_dyn) {
                fprintf(stderr, "[supervisor] OOM building pollfd set\n");
                break;
            }
            pfd = pfd_dyn;
        }

        // listen_fd (skipped while stopping so we drain children only)
        if (!stopping) {
            pfd[nfds].fd     = listen_fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }

        // signalfd
        pfd[nfds].fd     = sfd;
        pfd[nfds].events = POLLIN;
        nfds++;

        // session report pipes
        for (i = 0; i < sessions_cap; i++) {
            if (sessions[i].report_fd >= 0) {
                pfd[nfds].fd     = sessions[i].report_fd;
                pfd[nfds].events = POLLIN;
                nfds++;
            }
        }

        r = poll(pfd, nfds, stopping ? 100 : -1);
        if (r < 0) {
            if (errno == EINTR) { if (pfd_dyn) free(pfd_dyn); continue; }
            fprintf(stderr, "[supervisor] poll: %s\n", strerror(errno));
            if (pfd_dyn) free(pfd_dyn);
            break;
        }

        // Dispatch.
        for (i = 0; i < (int)nfds; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;

            if (pfd[i].fd == listen_fd) {
                accept_one(listen_fd);
                continue;
            }
            if (pfd[i].fd == sfd) {
                struct signalfd_siginfo si;
                while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                    if (si.ssi_signo == SIGCHLD) {
                        reap_children();
                    } else if (si.ssi_signo == SIGINT
                            || si.ssi_signo == SIGTERM) {
                        fprintf(stderr,
                                "[supervisor] signal %u; closing listener\n",
                                si.ssi_signo);
                        stopping = 1;
                    }
                }
                continue;
            }
            // Otherwise it's a session report pipe.
            {
                int j;
                for (j = 0; j < sessions_cap; j++) {
                    if (sessions[j].report_fd == pfd[i].fd) {
                        drain_report(&sessions[j]);
                        break;
                    }
                }
            }
        }

        if (pfd_dyn) free(pfd_dyn);

        if (stopping) {
            // Send SIGTERM to all sessions on first stop cycle.
            shutdown_sessions();
            break;
        }
    }

    close(listen_fd);
    close(sfd);
    free(sessions);
    return 0;
}
