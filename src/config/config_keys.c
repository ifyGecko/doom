#include "config_keys.h"

#include <stdio.h>
#include <ctype.h>

#include <ncurses.h>

// Named keys, in the order they appear in the picker. Printable ASCII keys
// (letters, digits, most punctuation) are not listed here -- they are
// represented by their literal character in doom_key_label() and entered
// directly via the live-capture overlay.
static const doom_key_t named[] = {
    {"Left Arrow",     DKEY_LEFTARROW},
    {"Right Arrow",    DKEY_RIGHTARROW},
    {"Up Arrow",       DKEY_UPARROW},
    {"Down Arrow",     DKEY_DOWNARROW},
    {"Space",          ' '},
    {"Enter",          DKEY_ENTER},
    {"Tab",            DKEY_TAB},
    {"Escape",         DKEY_ESCAPE},
    {"Backspace",      DKEY_BACKSPACE},
    {"Pause",          DKEY_PAUSE},
    {"Right Shift",    DKEY_RSHIFT},
    {"Right Ctrl",     DKEY_RCTRL},
    {"Right Alt",      DKEY_RALT},
    {"Equals",         DKEY_EQUALS},
    {"Minus",          DKEY_MINUS},
    {"F1",             DKEY_F1},
    {"F2",             DKEY_F2},
    {"F3",             DKEY_F3},
    {"F4",             DKEY_F4},
    {"F5",             DKEY_F5},
    {"F6",             DKEY_F6},
    {"F7",             DKEY_F7},
    {"F8",             DKEY_F8},
    {"F9",             DKEY_F9},
    {"F10",            DKEY_F10},
    {"F11",            DKEY_F11},
    {"F12",            DKEY_F12},
};

const doom_key_t *doom_keys_named(int *count)
{
    if (count) *count = (int)(sizeof(named) / sizeof(named[0]));
    return named;
}

const char *doom_key_label(int code)
{
    static char buf[16];
    int i;

    for (i = 0; i < (int)(sizeof(named) / sizeof(named[0])); i++)
        if (named[i].code == code) return named[i].name;

    // Printable ASCII falls through to a quoted-char rendering.
    if (code >= 33 && code < 127) {
        snprintf(buf, sizeof buf, "'%c'", code);
        return buf;
    }

    snprintf(buf, sizeof buf, "0x%02x", code & 0xff);
    return buf;
}

int doom_key_from_ncurses(int ch)
{
    switch (ch) {
      case KEY_LEFT:      return DKEY_LEFTARROW;
      case KEY_RIGHT:     return DKEY_RIGHTARROW;
      case KEY_UP:        return DKEY_UPARROW;
      case KEY_DOWN:      return DKEY_DOWNARROW;
      case KEY_BACKSPACE:
      case 127:
      case 8:             return DKEY_BACKSPACE;
      case KEY_ENTER:
      case '\r':
      case '\n':          return DKEY_ENTER;
      case '\t':          return DKEY_TAB;
      case KEY_F(1):      return DKEY_F1;
      case KEY_F(2):      return DKEY_F2;
      case KEY_F(3):      return DKEY_F3;
      case KEY_F(4):      return DKEY_F4;
      case KEY_F(5):      return DKEY_F5;
      case KEY_F(6):      return DKEY_F6;
      case KEY_F(7):      return DKEY_F7;
      case KEY_F(8):      return DKEY_F8;
      case KEY_F(9):      return DKEY_F9;
      case KEY_F(10):     return DKEY_F10;
      case KEY_F(11):     return DKEY_F11;
      case KEY_F(12):     return DKEY_F12;
      default:
        // Printable ASCII (space..~) maps to itself; doom treats these as
        // lowercase ASCII codes (see the 'w','s','a','d' defaults).
        if (ch >= ' ' && ch < 127) {
            if (ch >= 'A' && ch <= 'Z') ch = tolower(ch);
            return ch;
        }
        return 0;
    }
}
