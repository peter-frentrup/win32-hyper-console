#ifndef __CONSOLE__SCROLL_COUNTER_H__
#define __CONSOLE__SCROLL_COUNTER_H__

#include <windows.h>

struct console_scrollback_t *console_scrollback_new(void);
void console_scrollback_free(struct console_scrollback_t *cs);

/** The visible lines have changed, possibly scolled. Update the global-scoll-position information accordingly.
 */
void console_scrollback_update(struct console_scrollback_t *cs, int known_visible_lines);

BOOL console_scollback_local_to_global(struct console_scrollback_t *cs, COORD local, int *line, int *column);
BOOL console_scollback_global_to_local(struct console_scrollback_t *cs, int line, int column, COORD *local);

#endif // __CONSOLE__SCROLL_COUNTER_H__
