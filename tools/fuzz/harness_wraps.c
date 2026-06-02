// -----------------------------------------------------------------------------
// AFL++ fuzz harness: linker --wrap stubs
// -----------------------------------------------------------------------------
//
// These functions intercept three engine symbols at link time so each fuzz
// iteration finishes after the engine's init phase rather than running the
// game loop forever. The interception is purely link-level (-Wl,--wrap=NAME);
// the engine sources themselves are untouched. The original definitions are
// still in the binary as __real_NAME and could be reached if needed.
//
// One iteration of a fuzz run looks like:
//
//   main -> __wrap_I_Supervise (below) -> I_NetSetSessionFds(0, -1) ->
//     I_NetBootstrap (parses HELLO/ARGS/CONFIG/WAD from fd 0) ->
//     D_DoomMain (parses argv, loads WAD lumps, R_Init, P_Init, ...) ->
//     D_DoomLoop -> first I_StartTic sees EOF on fd 0 -> I_Quit ->
//     __wrap_exit -> __real_exit -> cleanup_temp -> process exits
//
// -----------------------------------------------------------------------------

#include <stdlib.h>
#include <unistd.h>

// Forward decls for the engine entry points we splice into; the engine
// sources define them and we just re-call them in a different order.
extern void I_NetSetSessionFds(int client_fd, int report_pipe_wfd);
extern void I_NetBootstrap(void);
extern void D_DoomMain(void);

// Replace the supervisor entirely. The real I_Supervise (i_supervisor.c)
// listens on TCP, poll()s, accept()s, fork()s, and runs the engine in
// the child. That works under normal use but is a bad fit for AFL: we
// want each iteration to read the test case from the harness's input
// pipe, not from a TCP socket. Skip straight to what engine_run_session
// would have done in the child, using fd 0 (AFL's input pipe) as the
// "client socket".
int __wrap_I_Supervise(int argc, char **argv)
{
    (void)argc; (void)argv;
    I_NetSetSessionFds(0, -1);
    I_NetBootstrap();
    D_DoomMain();
    _exit(0);
}

// Drop the game loop. AFL has already gathered coverage from all the
// init-phase parsers by the time we get here, which is the whole point
// of this harness: fuzz the ingest, not the simulation.
void __wrap_D_DoomLoop(void)
{
    _exit(0);
}

// We let atexit handlers run (cleanup_temp in i_video_net.c removes the
// per-session /tmp -> /dev/shm directory). Skipping them leaks one tmpfs
// dir per fuzz iteration, which fills /dev/shm on long runs. The cost
// of running them is one unlink + rmdir on tmpfs - negligible vs. the
// engine's W_InitMultipleFiles + R_Init time.
extern void __real_exit(int) __attribute__((noreturn));
void __wrap_exit(int code)
{
    __real_exit(code);
}

// D_DoomMain prints a "press enter to continue" banner via getchar() when
// modifiedgame is set (i.e. when -file is on the command line). A fuzzed
// argv stream can trip this; never let it block stdin (stdin is the fuzz
// input pipe, not a TTY).
int __wrap_getchar(void)
{
    return '\n';
}
