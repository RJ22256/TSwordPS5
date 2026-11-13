// (TSword PS5) The settings rows of the console menu (ps5/ut/ui/option_rows.cpp): WSword PS5's rows in the console's
// look (WSword runtime/ui/option_rows.mm) - black and greys, the rows easing in one after another when a page opens, the
// selected row's highlight fading in and out, switches and values moving to their new state. The selected row's name
// and description are read with opt_console_help() (the menu's foot). Cross / a tap changes a row (toggle / next
// value), left / right step it.
//
//   opt_begin("##video", scale);
//   opt_section("Display");
//   if (opt_choice("Frame rate", &fps, names, 2, 0, "...")) save();
//   opt_end();
#pragma once

struct ImFont;

void opt_begin(const char *id, float scale);
void opt_end(void);
void opt_section(const char *title);
bool opt_toggle(const char *label, bool *v, bool def, const char *desc);
// wrap: past the last value back to the first (and the other way); false: stops at the ends
bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap = true);
// whole numbers lo..hi in steps of `step`; fmt formats it (e.g. "%d%%"), lo_name (if given) is shown for lo instead
bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name = 0);
// an action: true when activated
bool opt_button(const char *label, const char *desc);
// an action / item row with a value on the right (an outlined pill); true when activated
bool opt_action(const char *label, const char *value, const char *desc);
// the next row is shown greyed out and can't be changed
void opt_disable_next(void);
// the next page's first row takes the controller's cursor
void opt_focus_first(bool on);
// the fonts of the rows (the selected one in medium)
void opt_console_look(ImFont *regular, ImFont *medium);
void opt_console_help(const char **name, const char **desc);
void opt_page_replay(void);  // the next opt_begin plays the rows' entrance again (its category opened again)
