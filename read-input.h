#ifndef __CONSOLE__READ_INPUT_H__
#define __CONSOLE__READ_INPUT_H__

#include <windows.h>

/** Test whether the current thread is calling read_input() at the moment.
 */
BOOL is_handling_input(void);

BOOL stop_current_input(BOOL do_abort, const wchar_t *opt_replace_input);

/** Read a line of input
  
  \return The text entered by the user without trailing newline, or NULL on error. 
          The result must be freed with free_memory().
 */
wchar_t *read_input(BOOL multiline_mode);

/** Release a block of memory returned by read_input().
 */
void free_memory(void *data);

#endif // __CONSOLE__READ_INPUT_H__
