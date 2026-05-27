// Doom key codes + ncurses translation.
//
// The numeric values here MUST match the KEY_* constants in
// src/engine/doomdef.h, since they are what the engine writes to / reads
// from the doomrc file. We redefine them locally (rather than pulling in
// doomdef.h) so the config tool stays decoupled from engine headers
// (which include unrelated game enums, and whose KEY_* names collide
// with ncurses' own KEY_* macros).

#ifndef DOOM_CONFIG_KEYS_H
#define DOOM_CONFIG_KEYS_H

// ---- doom key code constants (from src/engine/doomdef.h) -----------------
#define DKEY_RIGHTARROW 0xae
#define DKEY_LEFTARROW  0xac
#define DKEY_UPARROW    0xad
#define DKEY_DOWNARROW  0xaf
#define DKEY_ESCAPE     27
#define DKEY_ENTER      13
#define DKEY_TAB        9
#define DKEY_F1         (0x80+0x3b)
#define DKEY_F2         (0x80+0x3c)
#define DKEY_F3         (0x80+0x3d)
#define DKEY_F4         (0x80+0x3e)
#define DKEY_F5         (0x80+0x3f)
#define DKEY_F6         (0x80+0x40)
#define DKEY_F7         (0x80+0x41)
#define DKEY_F8         (0x80+0x42)
#define DKEY_F9         (0x80+0x43)
#define DKEY_F10        (0x80+0x44)
#define DKEY_F11        (0x80+0x57)
#define DKEY_F12        (0x80+0x58)
#define DKEY_BACKSPACE  127
#define DKEY_PAUSE      0xff
#define DKEY_EQUALS     0x3d
#define DKEY_MINUS      0x2d
#define DKEY_RSHIFT     (0x80+0x36)
#define DKEY_RCTRL      (0x80+0x1d)
#define DKEY_RALT       (0x80+0x38)

// Named entry in the human-readable key table.
typedef struct {
    const char *name;   // display label, e.g. "Right Arrow", "Space", "F1"
    int         code;   // doom key code
} doom_key_t;

// Iteration over the full named-key table (arrows, function keys, modifiers,
// space, enter, tab, escape, etc.). Letters/digits/punctuation are *not*
// included as named entries; they are handled via printable-char rendering.
const doom_key_t *doom_keys_named(int *count);

// Return a human-readable label for a doom key code. For printable ASCII
// (a-z, 0-9, punctuation) returns a static buffer with a quoted char.
// Never returns NULL.
const char *doom_key_label(int code);

// Translate an ncurses keypress (as returned by wgetch() with keypad mode
// enabled) into a doom key code. Returns 0 if the key is not mappable.
// Pass the raw int from wgetch(); this function knows ncurses' KEY_*
// macros internally.
int doom_key_from_ncurses(int ch);

#endif
