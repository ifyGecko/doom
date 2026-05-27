#include "config_ui.h"
#include "config_data.h"
#include "config_keys.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ncurses.h>

// ---- color pair ids -------------------------------------------------------
enum {
    PAIR_TITLE   = 1,
    PAIR_SECTION = 2,
    PAIR_SELECT  = 3,
    PAIR_HINT    = 4,
    PAIR_HELP    = 5,
    PAIR_STATUS  = 6,
    PAIR_DIALOG  = 7,
};

// One renderable row: either a section header (option == NULL) or an option.
typedef struct {
    const char   *section;  // non-NULL on header rows
    cfg_option_t *option;   // non-NULL on option rows
} row_t;

static row_t       *rows;
static int          n_rows;
static int          selected;        // current row index
static int          scroll_top;      // first visible row index (named to
                                     // avoid colliding with ncurses' scroll())
static int          dirty;           // pending unsaved changes
static const char  *filepath;
static char         statusmsg[256];

// ---- row layout -----------------------------------------------------------
//
// Build a row list that interleaves section headers with their options,
// preserving the order in cfg_options(). Sections are de-duplicated by
// pointer-then-strcmp on first appearance.
static void build_rows(void)
{
    int           n_opts;
    cfg_option_t *opts = cfg_options(&n_opts);
    const char   *last_section = NULL;
    int           i;

    free(rows);
    rows = calloc(n_opts * 2, sizeof(row_t));
    n_rows = 0;

    for (i = 0; i < n_opts; i++) {
        if (!last_section || strcmp(last_section, opts[i].section) != 0) {
            rows[n_rows].section = opts[i].section;
            rows[n_rows].option  = NULL;
            n_rows++;
            last_section = opts[i].section;
        }
        rows[n_rows].section = NULL;
        rows[n_rows].option  = &opts[i];
        n_rows++;
    }

    // Snap selection onto the first selectable (option) row.
    selected = 0;
    while (selected < n_rows && !rows[selected].option) selected++;
}

// ---- value rendering ------------------------------------------------------
static void render_value(const cfg_option_t *o, char *buf, size_t buflen)
{
    switch (o->type) {
      case CFG_BOOL:
        snprintf(buf, buflen, "%s (%d)", o->value_int ? "on" : "off",
                 o->value_int);
        break;
      case CFG_RANGE:
        snprintf(buf, buflen, "%d  (%d..%d)",
                 o->value_int, o->min, o->max);
        break;
      case CFG_INT:
        snprintf(buf, buflen, "%d", o->value_int);
        break;
      case CFG_KEY:
        snprintf(buf, buflen, "%s  [code %d]",
                 doom_key_label(o->value_int), o->value_int);
        break;
      case CFG_STRING:
        snprintf(buf, buflen, "\"%s\"",
                 o->value_str ? o->value_str : "");
        break;
    }
}

// ---- main screen draw -----------------------------------------------------
static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(statusmsg, sizeof statusmsg, fmt, ap);
    va_end(ap);
}

static void draw_screen(void)
{
    int  rows_avail, cols, i, y, x;
    int  top, bot, view;
    int  label_w;
    char buf[256];

    erase();
    getmaxyx(stdscr, rows_avail, cols);

    // Title bar.
    attron(COLOR_PAIR(PAIR_TITLE) | A_BOLD);
    for (x = 0; x < cols; x++) mvaddch(0, x, ' ');
    mvprintw(0, 1, " DOOM Configuration%s", dirty ? "  *" : "");
    {
        char right[256];
        int  rx;
        snprintf(right, sizeof right, "File: %s ", filepath);
        rx = cols - (int)strlen(right) - 1;
        if (rx < 20) rx = 20;
        mvaddnstr(0, rx, right, cols - rx - 1);
    }
    attroff(COLOR_PAIR(PAIR_TITLE) | A_BOLD);

    // Scroll bookkeeping: keep selected visible inside the option viewport.
    top  = 2;
    bot  = rows_avail - 4;   // leave room for help, status, hints
    view = bot - top + 1;
    if (view < 1) view = 1;
    if (selected < scroll_top) scroll_top = selected;
    if (selected >= scroll_top + view) scroll_top = selected - view + 1;
    if (scroll_top < 0) scroll_top = 0;
    if (scroll_top > n_rows - view)
        scroll_top = n_rows - view > 0 ? n_rows - view : 0;

    // Option / section rows.
    y = top;
    for (i = scroll_top; i < n_rows && y <= bot; i++, y++) {
        cfg_option_t *o;
        int           is_sel;

        if (rows[i].section) {
            attron(COLOR_PAIR(PAIR_SECTION) | A_BOLD);
            mvprintw(y, 2, "%s", rows[i].section);
            attroff(COLOR_PAIR(PAIR_SECTION) | A_BOLD);
            continue;
        }

        o = rows[i].option;
        is_sel = (i == selected);
        if (is_sel) attron(COLOR_PAIR(PAIR_SELECT));

        // pad full line so the highlight reaches the right margin
        for (x = 0; x < cols; x++) mvaddch(y, x, ' ');

        render_value(o, buf, sizeof buf);

        // Label column ~30, value column = rest. Trim if narrow.
        label_w = cols < 60 ? cols / 2 : 30;
        mvprintw(y, 4, "%-*.*s", label_w, label_w, o->label);
        mvprintw(y, 4 + label_w + 2, "%s", buf);

        if (is_sel) attroff(COLOR_PAIR(PAIR_SELECT));
    }

    // Help line (per-option).
    if (rows_avail >= 4) {
        cfg_option_t *o = selected < n_rows ? rows[selected].option : NULL;
        attron(COLOR_PAIR(PAIR_HELP));
        for (x = 0; x < cols; x++) mvaddch(rows_avail - 3, x, ' ');
        if (o && o->help)
            mvprintw(rows_avail - 3, 1, " %s", o->help);
        attroff(COLOR_PAIR(PAIR_HELP));
    }

    // Status line.
    if (rows_avail >= 3) {
        attron(COLOR_PAIR(PAIR_STATUS));
        for (x = 0; x < cols; x++) mvaddch(rows_avail - 2, x, ' ');
        if (statusmsg[0]) mvprintw(rows_avail - 2, 1, " %s", statusmsg);
        attroff(COLOR_PAIR(PAIR_STATUS));
    }

    // Key hints.
    if (rows_avail >= 2) {
        attron(COLOR_PAIR(PAIR_HINT) | A_BOLD);
        for (x = 0; x < cols; x++) mvaddch(rows_avail - 1, x, ' ');
        mvprintw(rows_avail - 1, 1,
                 " arrows: move   enter/space: edit   s: save   r: reset   q: quit ");
        attroff(COLOR_PAIR(PAIR_HINT) | A_BOLD);
    }

    refresh();
}

// ---- generic modal helpers ------------------------------------------------
//
// All edit overlays render into a small centered window. They return 1 if
// the value was changed, 0 if cancelled.
static WINDOW *open_dialog(int height, int width, const char *title)
{
    int sh, sw, y, x;
    WINDOW *w;

    getmaxyx(stdscr, sh, sw);
    if (height > sh - 2) height = sh - 2;
    if (width  > sw - 2) width  = sw - 2;
    y = (sh - height) / 2;
    x = (sw - width)  / 2;

    w = newwin(height, width, y, x);
    wbkgd(w, COLOR_PAIR(PAIR_DIALOG));
    box(w, 0, 0);
    if (title) {
        wattron(w, A_BOLD);
        mvwprintw(w, 0, 2, " %s ", title);
        wattroff(w, A_BOLD);
    }
    keypad(w, TRUE);
    return w;
}

static void close_dialog(WINDOW *w)
{
    delwin(w);
    touchwin(stdscr);
    refresh();
}

// ---- edit overlays --------------------------------------------------------
static int edit_bool(cfg_option_t *o)
{
    // Toggle inline -- no need for a modal. Simpler UX.
    o->value_int = !o->value_int;
    return 1;
}

static int edit_int_dialog(cfg_option_t *o)
{
    WINDOW *w = open_dialog(7, 50,
                            o->type == CFG_RANGE ? "Edit value" : "Edit number");
    char buf[64];
    int  pos = 0;

    snprintf(buf, sizeof buf, "%d", o->value_int);
    pos = (int)strlen(buf);

    if (o->type == CFG_RANGE)
        mvwprintw(w, 2, 2, "Range: %d .. %d", o->min, o->max);
    mvwprintw(w, 4, 2, "Value: ");
    wmove(w, 4, 9);
    curs_set(1);
    echo();

    int changed = 0;
    for (;;) {
        mvwprintw(w, 4, 9, "%-40s", buf);
        wmove(w, 4, 9 + pos);
        wrefresh(w);
        int c = wgetch(w);
        if (c == 27) break;               // Esc: cancel
        if (c == '\n' || c == KEY_ENTER) {
            int v;
            if (sscanf(buf, "%i", &v) == 1) {
                if (o->type == CFG_RANGE && (v < o->min || v > o->max)) {
                    set_status("Value %d outside %d..%d", v, o->min, o->max);
                    break;
                }
                if (v != o->value_int) {
                    o->value_int = v;
                    changed = 1;
                }
            }
            break;
        }
        if ((c == KEY_BACKSPACE || c == 127 || c == 8) && pos > 0) {
            buf[--pos] = '\0';
            continue;
        }
        if (pos < (int)sizeof(buf) - 1 &&
            (isdigit(c) || (pos == 0 && (c == '-' || c == '+')) ||
             c == 'x' || c == 'X' ||
             (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            buf[pos++] = (char)c;
            buf[pos] = '\0';
        }
    }
    noecho();
    curs_set(0);
    close_dialog(w);
    return changed;
}

static int edit_string(cfg_option_t *o)
{
    WINDOW *w = open_dialog(7, 60, "Edit string");
    char buf[256];
    int  pos;

    snprintf(buf, sizeof buf, "%s", o->value_str ? o->value_str : "");
    pos = (int)strlen(buf);

    mvwprintw(w, 2, 2, "%s:", o->label);
    curs_set(1);
    echo();

    int changed = 0;
    for (;;) {
        mvwprintw(w, 4, 2, "%-54s", buf);
        wmove(w, 4, 2 + pos);
        wrefresh(w);
        int c = wgetch(w);
        if (c == 27) break;
        if (c == '\n' || c == KEY_ENTER) {
            if (!o->value_str || strcmp(o->value_str, buf) != 0) {
                free(o->value_str);
                o->value_str = strdup(buf);
                changed = 1;
            }
            break;
        }
        if ((c == KEY_BACKSPACE || c == 127 || c == 8) && pos > 0) {
            buf[--pos] = '\0';
            continue;
        }
        if (pos < (int)sizeof(buf) - 1 && c >= ' ' && c < 127) {
            buf[pos++] = (char)c;
            buf[pos] = '\0';
        }
    }
    noecho();
    curs_set(0);
    close_dialog(w);
    return changed;
}

// Scrollable picker of named doom keys. Used as a Tab-fallback when the
// terminal cannot capture a key directly (e.g. plain modifier presses).
static int pick_key_from_list(int *out)
{
    int n;
    const doom_key_t *list = doom_keys_named(&n);
    int sel = 0, scr = 0;
    WINDOW *w = open_dialog(20, 40, "Pick key");
    int chosen = 0;

    for (;;) {
        int view = 16;
        int i, c;

        werase(w);
        wbkgd(w, COLOR_PAIR(PAIR_DIALOG));
        box(w, 0, 0);
        wattron(w, A_BOLD);
        mvwprintw(w, 0, 2, " Pick key ");
        wattroff(w, A_BOLD);
        mvwprintw(w, 18, 2, "enter: select   esc: cancel");

        if (sel < scr) scr = sel;
        if (sel >= scr + view) scr = sel - view + 1;

        for (i = 0; i < view && scr + i < n; i++) {
            int row_y = 1 + i;
            int is_sel = (scr + i == sel);
            if (is_sel) wattron(w, A_REVERSE);
            mvwprintw(w, row_y, 2, "%-36s", list[scr + i].name);
            if (is_sel) wattroff(w, A_REVERSE);
        }
        wrefresh(w);

        c = wgetch(w);
        if (c == 27) break;
        if (c == '\n' || c == KEY_ENTER) {
            *out = list[sel].code;
            chosen = 1;
            break;
        }
        if (c == KEY_UP   && sel > 0)     sel--;
        if (c == KEY_DOWN && sel < n - 1) sel++;
        if (c == KEY_NPAGE) { sel += 8; if (sel >= n) sel = n - 1; }
        if (c == KEY_PPAGE) { sel -= 8; if (sel < 0)  sel = 0;     }
    }

    close_dialog(w);
    return chosen;
}

static int edit_key(cfg_option_t *o)
{
    WINDOW *w = open_dialog(8, 56, "Bind key");
    int changed = 0;

    mvwprintw(w, 2, 2, "Current binding: %s  [code %d]",
              doom_key_label(o->value_int), o->value_int);
    mvwprintw(w, 4, 2, "Press the new key.");
    mvwprintw(w, 6, 2, "Tab: pick from list   Esc: cancel");
    wrefresh(w);

    for (;;) {
        int c = wgetch(w);
        if (c == 27) break;                   // Esc: cancel
        if (c == '\t') {
            close_dialog(w);
            int picked;
            if (pick_key_from_list(&picked)) {
                if (picked != o->value_int) {
                    o->value_int = picked;
                    changed = 1;
                }
            }
            return changed;
        }
        int dk = doom_key_from_ncurses(c);
        if (dk) {
            if (dk != o->value_int) {
                o->value_int = dk;
                changed = 1;
            }
            break;
        }
        // unmapped key: just wait for another
    }

    close_dialog(w);
    return changed;
}

static int edit_current(void)
{
    cfg_option_t *o = rows[selected].option;
    if (!o) return 0;
    switch (o->type) {
      case CFG_BOOL:                       return edit_bool(o);
      case CFG_INT:
      case CFG_RANGE:                      return edit_int_dialog(o);
      case CFG_KEY:                        return edit_key(o);
      case CFG_STRING:                     return edit_string(o);
    }
    return 0;
}

// ---- yes/no confirmation --------------------------------------------------
static int confirm(const char *prompt)
{
    WINDOW *w = open_dialog(5, 60, "Confirm");
    mvwprintw(w, 2, 2, "%s", prompt);
    mvwprintw(w, 3, 2, "y = yes, n / esc = no");
    wrefresh(w);
    int yes = 0;
    for (;;) {
        int c = wgetch(w);
        if (c == 'y' || c == 'Y') { yes = 1; break; }
        if (c == 'n' || c == 'N' || c == 27 || c == '\n' || c == KEY_ENTER)
            break;
    }
    close_dialog(w);
    return yes;
}

// ---- navigation -----------------------------------------------------------
static void move_selection(int delta)
{
    int s = selected;
    for (;;) {
        s += delta > 0 ? 1 : -1;
        if (s < 0 || s >= n_rows) return;       // hit edge, no change
        if (rows[s].option) { selected = s; return; }
    }
}

// ---- ncurses init / teardown ---------------------------------------------
static void init_colors(void)
{
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(PAIR_TITLE,   COLOR_WHITE,  COLOR_BLUE);
    init_pair(PAIR_SECTION, COLOR_YELLOW, -1);
    init_pair(PAIR_SELECT,  COLOR_BLACK,  COLOR_CYAN);
    init_pair(PAIR_HINT,    COLOR_WHITE,  COLOR_BLUE);
    init_pair(PAIR_HELP,    COLOR_CYAN,   -1);
    init_pair(PAIR_STATUS,  COLOR_GREEN,  -1);
    init_pair(PAIR_DIALOG,  COLOR_WHITE,  COLOR_BLACK);
}

int config_ui_run(const char *path)
{
    filepath = path;
    statusmsg[0] = '\0';

    cfg_load(path);
    build_rows();

    if (!initscr()) {
        fprintf(stderr, "doom-config: failed to initialize ncurses.\n");
        return 1;
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_colors();

    set_status("Loaded %s", path);

    int running = 1;
    while (running) {
        draw_screen();
        int c = getch();
        statusmsg[0] = '\0';
        switch (c) {
          case 'q': case 'Q':
            if (dirty) {
                if (confirm("Unsaved changes. Quit without saving?"))
                    running = 0;
            } else {
                running = 0;
            }
            break;
          case 's': case 'S':
            if (cfg_save(path) == 0) {
                dirty = 0;
                set_status("Saved to %s", path);
            } else {
                set_status("ERROR: could not write %s", path);
            }
            break;
          case 'r': case 'R':
            if (confirm("Reset all options to defaults?")) {
                cfg_reset_defaults();
                dirty = 1;
                set_status("Reset to defaults (not yet saved).");
            }
            break;
          case KEY_UP:    move_selection(-1); break;
          case KEY_DOWN:  move_selection(+1); break;
          case KEY_HOME:
            selected = 0;
            while (selected < n_rows && !rows[selected].option) selected++;
            break;
          case KEY_END:
            selected = n_rows - 1;
            while (selected > 0 && !rows[selected].option) selected--;
            break;
          case KEY_PPAGE: {
            int i;
            for (i = 0; i < 8; i++) move_selection(-1);
            break;
          }
          case KEY_NPAGE: {
            int i;
            for (i = 0; i < 8; i++) move_selection(+1);
            break;
          }
          case '\n':
          case KEY_ENTER:
          case ' ':
            if (edit_current()) dirty = 1;
            break;
          default:
            break;
        }
    }

    endwin();
    free(rows);
    return 0;
}
