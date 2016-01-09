#ifndef __CONSOLE__READ_INPUT_H__
#define __CONSOLE__READ_INPUT_H__

#include <windows.h>

/** Test whether the current thread is calling read_input() at the moment.
 */
BOOL is_handling_input(void);

/** Tell the current read_input() loop to stop.
  
  \param do_abort Whether to return NULL from read_input().
  \param opt_replace_input An optional replacement of the input buffer before returning.
  
  \return Whether there is any a current input loop.
 */
BOOL stop_current_input(BOOL do_abort, const wchar_t *opt_replace_input);

/** Read a line of input
  
  \return The text entered by the user without trailing newline, or NULL on error. 
          The result must be freed with free_memory().
 */
wchar_t *read_input(BOOL multiline_mode, const wchar_t *default_input);

#endif // __CONSOLE__READ_INPUT_H__
