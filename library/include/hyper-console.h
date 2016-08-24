#ifndef __HYPER_CONSOLE_H__
#define __HYPER_CONSOLE_H__

#include <windows.h>
#include <hyper-console-config.h>


struct hyper_console_history_t;

/** Create a new console history buffer.

  \param options History option flags for future use. Must be 0.
 */
HYPER_CONSOLE_API
struct hyper_console_history_t *hyper_console_history_new(int options);


/** Free a console history buffer.

  \param hist A console history buffer or NULL.
 */
HYPER_CONSOLE_API
void hyper_console_history_free(struct hyper_console_history_t *hist);


enum {
  /** Input can span multiple lines.
  
      By default, input is read until two consecutive line freeds \\n are read.
      But that can be changed by providing a `hyper_console_settings_t::need_more_input_predicate`
      callback.
   */
  HYPER_CONSOLE_FLAGS_MULTILINE = 1
};


/** Options for read_input()
 */
struct hyper_console_settings_t {
  /** Size of the structure, i.e. sizeof(struct read_input_settings_t)
   */
  size_t size;
  
  /** Zero or more of the HYPER_CONSOLE_FLAGS_XXX constants.
   */
  int flags; 
  
  /** The default input or NULL.
   */
  const wchar_t *default_input;
  
  /** A history buffer or NULL.
   */
  struct hyper_console_history_t *history;
  
  /** Optional context argument for callbacks.
   */
  void *callback_context;
  
  /** Optional end-of-input detection callback.
      This only really makes sense when \c HYPER_CONSOLE_FLAGS_MULTILINE is given in \c flags.
      \param context The value provided in \c callback_context.
      \param buffer  The whole current input buffer.
      \param len     The input buffer length.
      \param pos     The current cursor position.
      \return TRUE if more input lines should be read. FALSE if input is done and 
              hyper_console_readline() should return.
      
      This function is only called when the cursor is in the last line. When the cursor is in a 
      previous line, a line break will be inserted.
   */
  BOOL (*need_more_input_predicate)(void *context, const wchar_t *buffer, int len, int cursor_pos);
};

/** Read a line of input
  
  \param settings Optional customizations of the input procedure.
  \return The text entered by the user without trailing newline, or NULL on error. 
          The result must be freed with hyper_console_free_memory().
 */
HYPER_CONSOLE_API
wchar_t *hyper_console_readline(struct hyper_console_settings_t *settings);



/** Release a block of memory.
 */
HYPER_CONSOLE_API
void hyper_console_free_memory(void *data);



HYPER_CONSOLE_API
void hyper_console_init_hyperlink_system(void);

HYPER_CONSOLE_API
void hyper_console_done_hyperlink_system(void);


HYPER_CONSOLE_API
void hyper_console_start_link(const wchar_t *title);

HYPER_CONSOLE_API
void hyper_console_set_link_input_text(const wchar_t *text);

/** Set the current link's color attributes and returns the previous color attributes.
 */
HYPER_CONSOLE_API
WORD hyper_console_set_link_color(WORD attribute);

HYPER_CONSOLE_API
void hyper_console_end_link(void);


#endif
