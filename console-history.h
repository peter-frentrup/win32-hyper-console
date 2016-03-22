#ifndef __CONSOLE_HISTORY_H__
#define __CONSOLE_HISTORY_H__


#include <wchar.h>


struct console_history_t;


/** Create a new console history buffer.

  \param options History option flags for future use. Must be 0.
 */
struct console_history_t *console_history_new(int options);


/** Free a console history buffer.

  \param hist A console history buffer or NULL.
 */
void console_history_free(struct console_history_t *hist);


/** Get the number of entries in the history.
  
  \param hist A console history buffer or NULL.
 */
int console_history_count(struct console_history_t *hist);


/** Get a history entry.
  
  \param hist A console history buffer.
  \param index The entries index. 0 is the oldest, console_history_count(hist)-1 the newest.
  \param length Optional output parameter where to store the text length.
  
  \return The history entry or NULL on error.
 */
const wchar_t *console_history_get(struct console_history_t *hist, int index, int *length);

/** Add an entry to the history.

  \param hist A console history buffer or NULL.
  \param text The new entry.
  \param text_length The length of the entry or -1 to indicate that \a text is a NUL-terminated string.

  This function copies the \a text buffer. The entry may be discarded depending on the history buffer options.
 */
void console_history_add(struct console_history_t *hist, const wchar_t *text, int text_length);


#endif // __CONSOLE_HISTORY_H__
