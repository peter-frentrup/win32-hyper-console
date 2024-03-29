#ifndef __HYPER_CONSOLE_H__
#define __HYPER_CONSOLE_H__

#include <windows.h>
#include <hyper-console-config.h>


#define HYPER_CONSOLE_VERSION  0x00010001


/** An opaque structure that holds the current command history.
 */
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
  HYPER_CONSOLE_FLAGS_MULTILINE = 1,
  
  /** Do not echo the input text.
      
      This may be used for password input.
   */
  HYPER_CONSOLE_FLAGS_NO_ECHO = 2,
};


/** Options for read_input()
 */
struct hyper_console_settings_t {
  /** Size of the structure, i.e. sizeof(struct hyper_console_settings_t)
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
      \param context     The value provided in \c callback_context.
      \param buffer      The whole current input buffer.
      \param len         The input buffer length.
      \param cursor_pos  The current cursor position.
      \return TRUE if more input lines should be read. FALSE if input is done and 
              hyper_console_readline() should return.
      
      This function is only called when the cursor is in the last line. When the cursor is in a 
      previous line, a line break will be inserted.
   */
  BOOL (*need_more_input_predicate)(void *context, const wchar_t *buffer, int len, int cursor_pos);
  
  /** Optional auto-completion callback.
      This is called when the user presses TAB to get all possible completions
      \param context           The value provided in \c callback_context.
      \param buffer            The whole current input buffer.
      \param len               The input buffer length.
      \param cursor_pos        The current cursor position.
      \param completion_start  To be set to the start position of the text to be replaced/completed.
      \param completion_end    To be set to the end position of the text to be replaced/completed.
      \return A NULL-terminated array of NUL-terminated strings. 
      
      The returned array and each string inside has to be allocated with hyper_console_allocate_memory()
      and will be freed by the caller.
      
      The function may also return NULL for an empty array.
   */
  wchar_t **(*auto_completion)(void *context, const wchar_t *buffer, int len, int cursor_pos, int *completion_start, int *completion_end);
  
  /** Optional prompt for second/third/... line in multiline mode.
   */
  const wchar_t *line_continuation_prompt;
  
  /** Optional pre-processor for any key events.
      \param context  The value provided in \c callback_context.
      \param er       The key event record.
      \return Whether the event was handled and automatic handling should be suppressed.
      
      During mark mode, `mark_mode_key_event_filter` is used instead.
   */
  BOOL (*key_event_filter)(void *context, const KEY_EVENT_RECORD *er);
  
  /** Optional tab width.
      
      Defaults to 8. The value 0 is interpreted as the default (8).
   */
  int tab_width;
  
  /** Optional first tab position.
      
      Defaults to 0.
   */
  int first_tab_column;
  
  /** Optional pre-processor for any key events druing mark mode.
      \param context  The value provided in \c callback_context.
      \param er       The key event record.
      \return Whether the event was handled and automatic handling should be suppressed.
   */
  BOOL (*mark_mode_key_event_filter)(void *context, const KEY_EVENT_RECORD *er);
};

/** Read a line of input
  
  \param settings Optional customizations of the input procedure.
  \return The text entered by the user without trailing newline, or NULL on error. 
          The result must be freed with hyper_console_free_memory().
 */
HYPER_CONSOLE_API
wchar_t *hyper_console_readline(struct hyper_console_settings_t *settings);


/** Interrupt the current `hyper_console_readline()`.
   
  \param callback     A function to execute while the current input is hidden. 
                      This may print to console.
  \param callback_arg The argument to pass to \a callback.
  
  The current input line and prompt will be hidden before calling \a callback and restored 
  after that function returns (at the next empty line after the then current cursor position).
  
  If there is no current hyper_console_readline, \a callback will be called without pre- 
  or post-processing.
 */
HYPER_CONSOLE_API
void hyper_console_interrupt(void (*callback)(void*), void *callback_arg);

/** Get the currently edited input text.
  
  \param length Pointer to an integer receiving the input buffer length. Must not be NULL.
  \return The current input line if hyper_console_readline() is running in the current thread,
          or NULL otherwise.
  
  The buffer returned by this function is owned by hyper-console. You must not modify it.
 */
HYPER_CONSOLE_API
const wchar_t *hyper_console_get_current_input(int *length);

/** Get the current input text selection range.
  
  \param position Pointer to an integer receiving the cursor position. Must not be NULL.
  \param anchor   Pointer to an integer receiving the selection anchor. Must not be NULL.
  
  The selected text is between \a position and \a anchor.
 */
HYPER_CONSOLE_API
void hyper_console_get_current_selection(int *position, int *anchor);

/** Get the current input text selection range.
  
  \param position The new cursor position.
  \param anchor   The new selection anchor.
  
  Both, \a position and \a anchor will be clamped to 0..length_of_input.
 */
HYPER_CONSOLE_API
void hyper_console_set_current_selection(int position, int anchor);

/**Get the selected text in mark-mode.

  \param total_length Optional. Receives the string length of the selection.
  \return The unwrapped text lines. Must be freed with hyper_console_free_memory(). NULL on error (e.g. when mark mode is not active).
 */
HYPER_CONSOLE_API
wchar_t *hyper_console_get_mark_mode_selection(int *total_length);

/**Get the line that contains the mark mode cursor.

  \param line_length  Optional. Receives the length of the line.
  \param pos_in_line  Optional. Receives the relative position of the mark mode cursor inside the line
  \return The visual line. Must be freed with hyper_console_free_memory(). NULL on error (e.g. when mark mode is not active).

  Note that mark mode selection may span multiple lines above or below the returned line.
 */
HYPER_CONSOLE_API
wchar_t *hyper_console_get_mark_mode_line(int *line_length, int *pos_in_line);

/** Allocate a block of memory.
 */
HYPER_CONSOLE_API
void *hyper_console_allocate_memory(size_t size);

/** Release a block of memory.
 */
HYPER_CONSOLE_API
void hyper_console_free_memory(void *data);



/** Initialize the hyperlink system.
  
  This must be called before any other hyperling related functions.
  To clean-up, call hyper_console_done_hyperlink_system().
  
  Note that calls to hyper_console_init_hyperlink_system() ... hyper_console_done_hyperlink_system()
  may not be nested.
 */
HYPER_CONSOLE_API
void hyper_console_init_hyperlink_system(void);

/** Clean-up the hyperlink system.
 */
HYPER_CONSOLE_API
void hyper_console_done_hyperlink_system(void);


/** Start writing a hyperlink.

  \a title An optional title to show (in the console's title bar) when hovering the link.
           If set to NULL, the links' destination defined by hyper_console_set_link_input_text() 
           will be used.
  
  Any text written to the console until the call to hyper_console_end_link() will be part 
  of the hyperlink. Nested links are technically possible but not useful. 
  
  In HTML parlance, this function corresponds to an opting `<a title="...">` anchor.
  
  In DML (Debugger Markup Language) this corresponds to an opening `<link >` anchor.
 */
HYPER_CONSOLE_API
void hyper_console_start_link(const wchar_t *title);

/** Set the command/destination of the currently opened link.
  
  \a text An optional text that will be returned by hyper_console_readline() when the 
          link is clicked.
 */
HYPER_CONSOLE_API
void hyper_console_set_link_input_text(const wchar_t *text);

/** Set the current link's color attributes and returns the previous color attributes.
 */
HYPER_CONSOLE_API
WORD hyper_console_set_link_color(WORD attribute);

/** Close the currently opened link.
  
  In HTML (resp. DML) parlance, this puts the closing `</a>` (resp. `</link>`) tag.
 */
HYPER_CONSOLE_API
void hyper_console_end_link(void);


#endif
