#include "config_data.h"
#include "config_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Canonical option table. The names, defaults, and groupings mirror
// defaults[] in src/engine/m_misc.c. `min`/`max` are only consulted for
// CFG_RANGE entries; the renderer also uses them as hints elsewhere.
static cfg_option_t options[] = {
    // ---- Display ---------------------------------------------------------
    {"screenblocks",     "Screen size",        "Display",
     "View size: 3 = smallest, 11 = full screen (no status bar)",
     CFG_RANGE, 3, 11, 9, NULL, 0, NULL},
    {"detaillevel",      "Detail level",       "Display",
     "0 = high detail, 1 = low detail (faster software renderer)",
     CFG_BOOL, 0, 1, 0, NULL, 0, NULL},
    {"usegamma",         "Gamma correction",   "Display",
     "Gamma level 0..4 (cycled by F11 in-game)",
     CFG_RANGE, 0, 4, 0, NULL, 0, NULL},

    // ---- Messages --------------------------------------------------------
    {"show_messages",    "Show messages",      "Messages",
     "Display in-game pickup / event messages",
     CFG_BOOL, 0, 1, 1, NULL, 0, NULL},

    // ---- Keyboard --------------------------------------------------------
    {"key_right",        "Turn right",         "Keyboard",
     "Key to turn right",
     CFG_KEY, 0, 0, DKEY_RIGHTARROW, NULL, 0, NULL},
    {"key_left",         "Turn left",          "Keyboard",
     "Key to turn left",
     CFG_KEY, 0, 0, DKEY_LEFTARROW,  NULL, 0, NULL},
    {"key_up",           "Move forward",       "Keyboard",
     "Key to move forward",
     CFG_KEY, 0, 0, 'w',              NULL, 0, NULL},
    {"key_down",         "Move backward",      "Keyboard",
     "Key to move backward",
     CFG_KEY, 0, 0, 's',              NULL, 0, NULL},
    {"key_strafeleft",   "Strafe left",        "Keyboard",
     "Key to strafe left",
     CFG_KEY, 0, 0, 'a',              NULL, 0, NULL},
    {"key_straferight",  "Strafe right",       "Keyboard",
     "Key to strafe right",
     CFG_KEY, 0, 0, 'd',              NULL, 0, NULL},
    {"key_fire",         "Fire",               "Keyboard",
     "Key to fire the current weapon",
     CFG_KEY, 0, 0, DKEY_RCTRL,       NULL, 0, NULL},
    {"key_use",          "Use / open",         "Keyboard",
     "Key to use switches / open doors",
     CFG_KEY, 0, 0, ' ',              NULL, 0, NULL},
    {"key_strafe",       "Strafe modifier",    "Keyboard",
     "Hold to convert turn keys into strafe",
     CFG_KEY, 0, 0, DKEY_RALT,        NULL, 0, NULL},
    {"key_speed",        "Run modifier",       "Keyboard",
     "Hold to run (always-run users: bind to an unused key)",
     CFG_KEY, 0, 0, DKEY_RSHIFT,      NULL, 0, NULL},

    // ---- Mouse -----------------------------------------------------------
    {"use_mouse",        "Enable mouse",       "Mouse",
     "Enable mouse input",
     CFG_BOOL, 0, 1, 1, NULL, 0, NULL},
    {"mouse_sensitivity","Mouse sensitivity",  "Mouse",
     "Mouse turn sensitivity (typical range 0..20)",
     CFG_RANGE, 0, 20, 5, NULL, 0, NULL},
    {"mousedev",         "Mouse device",       "Mouse",
     "Mouse device path (legacy serial mouse; unused on SDL clients)",
     CFG_STRING, 0, 0, 0, "/dev/ttyS0", 0, NULL},
    {"mousetype",        "Mouse type",         "Mouse",
     "Mouse protocol (legacy; unused on SDL clients)",
     CFG_STRING, 0, 0, 0, "microsoft", 0, NULL},
    {"mouseb_fire",      "Mouse button: fire", "Mouse",
     "Mouse button index that triggers fire (0=left, 1=middle, 2=right)",
     CFG_RANGE, 0, 7, 0, NULL, 0, NULL},
    {"mouseb_strafe",    "Mouse button: strafe","Mouse",
     "Mouse button index that triggers strafe",
     CFG_RANGE, 0, 7, 1, NULL, 0, NULL},
    {"mouseb_forward",   "Mouse button: forward","Mouse",
     "Mouse button index that triggers forward",
     CFG_RANGE, 0, 7, 2, NULL, 0, NULL},

    // ---- Joystick --------------------------------------------------------
    {"use_joystick",     "Enable joystick",    "Joystick",
     "Enable joystick input",
     CFG_BOOL, 0, 1, 0, NULL, 0, NULL},
    {"joyb_fire",        "Joy button: fire",   "Joystick",
     "Joystick button index that triggers fire",
     CFG_RANGE, 0, 15, 0, NULL, 0, NULL},
    {"joyb_strafe",      "Joy button: strafe", "Joystick",
     "Joystick button index that triggers strafe",
     CFG_RANGE, 0, 15, 1, NULL, 0, NULL},
    {"joyb_use",         "Joy button: use",    "Joystick",
     "Joystick button index that triggers use",
     CFG_RANGE, 0, 15, 3, NULL, 0, NULL},
    {"joyb_speed",       "Joy button: speed",  "Joystick",
     "Joystick button index that triggers speed/run",
     CFG_RANGE, 0, 15, 2, NULL, 0, NULL},
};

static const int n_options = (int)(sizeof(options) / sizeof(options[0]));

cfg_option_t *cfg_options(int *count)
{
    if (count) *count = n_options;
    return options;
}

void cfg_reset_defaults(void)
{
    int i;
    for (i = 0; i < n_options; i++) {
        cfg_option_t *o = &options[i];
        if (o->type == CFG_STRING) {
            free(o->value_str);
            o->value_str = o->default_str ? strdup(o->default_str) : NULL;
        } else {
            o->value_int = o->default_int;
        }
    }
}

static cfg_option_t *find_option(const char *name)
{
    int i;
    for (i = 0; i < n_options; i++)
        if (!strcmp(options[i].name, name)) return &options[i];
    return NULL;
}

int cfg_load(const char *path)
{
    FILE *f;
    char  def[80];
    char  strparm[256];

    cfg_reset_defaults();

    f = fopen(path, "r");
    if (!f) return 0;  // missing file is fine; defaults stand.

    // Same parse shape as M_LoadDefaults() in m_misc.c: tokenized as
    // `name<ws>value-to-eol`, where strings are double-quoted.
    while (!feof(f)) {
        if (fscanf(f, "%79s %255[^\n]\n", def, strparm) != 2) {
            // Skip malformed line; eat to end-of-line.
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }

        cfg_option_t *o = find_option(def);
        if (!o) continue;  // unknown key: drop, like the engine does on save.

        if (strparm[0] == '"') {
            // Strip surrounding quotes.
            size_t len = strlen(strparm);
            if (len >= 2 && strparm[len - 1] == '"') strparm[len - 1] = '\0';
            if (o->type == CFG_STRING) {
                free(o->value_str);
                o->value_str = strdup(strparm + 1);
            }
        } else {
            int parm;
            if (strparm[0] == '0' && strparm[1] == 'x')
                sscanf(strparm + 2, "%x", &parm);
            else
                sscanf(strparm, "%i", &parm);
            if (o->type != CFG_STRING) o->value_int = parm;
        }
    }

    fclose(f);
    return 0;
}

int cfg_save(const char *path)
{
    FILE *f = fopen(path, "w");
    int   i;

    if (!f) return -1;

    for (i = 0; i < n_options; i++) {
        const cfg_option_t *o = &options[i];
        if (o->type == CFG_STRING) {
            fprintf(f, "%s\t\t\"%s\"\n", o->name,
                    o->value_str ? o->value_str : "");
        } else {
            fprintf(f, "%s\t\t%i\n", o->name, o->value_int);
        }
    }

    fclose(f);
    return 0;
}

const char *cfg_default_path(void)
{
    static char buf[1024];
    const char *home = getenv("HOME");
    if (home && *home)
        snprintf(buf, sizeof buf, "%s/.doomrc", home);
    else
        snprintf(buf, sizeof buf, "./.doomrc");
    return buf;
}
