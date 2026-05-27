// ncurses front-end for the config tool.

#ifndef DOOM_CONFIG_UI_H
#define DOOM_CONFIG_UI_H

// Run the configuration UI against the given file path. Loads the file
// (if present), lets the user edit, optionally saves back. Returns 0 on
// a clean exit, non-zero if ncurses failed to initialize.
int config_ui_run(const char *path);

#endif
