// Option table + doomrc load/save.
//
// Mirrors the defaults[] table in src/engine/m_misc.c. The on-disk format
// is line-oriented `name<whitespace>value`, where string values are
// double-quoted. Numeric values are decimal or 0x-prefixed hex; the engine
// writes decimal so this tool does too.

#ifndef DOOM_CONFIG_DATA_H
#define DOOM_CONFIG_DATA_H

#include <stddef.h>

typedef enum {
    CFG_INT,      // arbitrary integer
    CFG_RANGE,    // integer constrained to [min, max]
    CFG_BOOL,     // 0 or 1
    CFG_KEY,      // doom key code
    CFG_STRING,   // free-form string
} cfg_type_t;

typedef struct {
    const char *name;        // doomrc key, e.g. "mouse_sensitivity"
    const char *label;       // UI label, e.g. "Mouse sensitivity"
    const char *section;     // UI grouping, e.g. "Display"
    const char *help;        // one-line description shown in status bar
    cfg_type_t  type;
    int         min, max;    // for CFG_RANGE; ignored otherwise
    int         default_int; // default for non-string types
    const char *default_str; // default for CFG_STRING (else NULL)

    // Current values. Exactly one is meaningful per option, per type.
    int         value_int;
    char       *value_str;   // owned heap string for CFG_STRING
} cfg_option_t;

// Iterate the option table.
cfg_option_t *cfg_options(int *count);

// Reset all options to their compiled-in defaults.
void cfg_reset_defaults(void);

// Load options from `path`. Missing file is not an error: the table is
// simply left at its defaults. Returns 0 on success, -1 if the file
// exists but could not be opened.
int cfg_load(const char *path);

// Persist all options to `path`. Format matches M_SaveDefaults() so the
// engine will read it back identically. Returns 0 on success, -1 on
// I/O failure.
int cfg_save(const char *path);

// Resolve the default doomrc path ($HOME/.doomrc). Returns a static
// buffer; never NULL (falls back to "./.doomrc" if HOME is unset).
const char *cfg_default_path(void);

#endif
