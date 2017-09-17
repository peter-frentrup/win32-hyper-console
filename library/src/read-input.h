#ifndef __CONSOLE__READ_INPUT_H__
#define __CONSOLE__READ_INPUT_H__

#include <windows.h>

/** Tell the current read_input() loop to stop.
  
  \param do_abort Whether to return NULL from read_input().
  \param opt_replace_input An optional replacement of the input buffer before returning.
  
  \return Whether there is any a current input loop.
 */
BOOL stop_current_input(BOOL do_abort, const wchar_t *opt_replace_input);

#endif // __CONSOLE__READ_INPUT_H__
