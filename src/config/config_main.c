// doom-config: ncurses TUI for editing a doomrc.
//
// Usage:
//   doom-config                  # edits $HOME/.doomrc (creates if missing)
//   doom-config <path>           # edits <path>
//   doom-config -c <path>        # explicit -c form, matches engine's flag
//   doom-config -h | --help      # print usage
//
// The on-disk format produced here matches what M_SaveDefaults() in
// src/engine/m_misc.c writes, so the engine will load it back unmodified.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "config_ui.h"
#include "config_data.h"

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
        "Usage: %s [-c <path> | <path>]\n"
        "  Edit a doomrc configuration file.\n"
        "  Defaults to $HOME/.doomrc when no path is given.\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s requires a path argument\n",
                        argv[0], argv[i]);
                return 2;
            }
            path = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], argv[i]);
            usage(stderr, argv[0]);
            return 2;
        }
        if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "%s: extra argument '%s'\n", argv[0], argv[i]);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (!path) path = cfg_default_path();

    return config_ui_run(path);
}
