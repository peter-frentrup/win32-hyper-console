#ifndef __CONSOLE__READ_INPUT_H__
#define __CONSOLE__READ_INPUT_H__

#include <windows.h>

struct console_history_t;

enum{
  READ_INPUT_FLAG_MULTILINE = 1
};

/** Options for read_input()
 */
struct read_input_settings_t {
  /** Size of the structure, i.e. sizeof(struct read_input_settings_t)
   */
  size_t size;
  
  /** Zero or more of the READ_INPUT_FLAG_XXX constants.
   */
  int flags; 
  
  /** The default input or NULL.
   */
  const wchar_t *default_input;
  
  /** A history buffer or NULL.
   */
  struct console_history_t *history;
};

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
  
  \param settings Optional customizations of the input procedure.
  \return The text entered by the user without trailing newline, or NULL on error. 
          The result must be freed with free_memory().
 */
wchar_t *read_input(struct read_input_settings_t *settings);

#endif // __CONSOLE__READ_INPUT_H__
