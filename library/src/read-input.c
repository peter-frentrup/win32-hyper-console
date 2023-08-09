#define _WIN32_WINNT 0x0600

#include <hyper-console.h>

#include "read-input.h"
#include "console-history.h"
#include "memory-util.h"
#include "hyperlink-output.h"
#include "console-buffer-io.h"
#include "debug.h"
#include "mark-mode.h"
#include "search-mode.h"
#include "text-util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>


#ifndef MOUSE_HWHEELED
#  define MOUSE_HWHEELED 0x0008
#endif


#define MIN(A, B)  ((A) < (B) ? (A) : (B))
#define MAX(A, B)  ((A) > (B) ? (A) : (B))


#ifndef offsetof
#  define offsetof(type, member) __builtin_offsetof(type, member)
#endif


#ifndef ENABLE_LVB_GRID_WORLDWIDE
#  define ENABLE_LVB_GRID_WORLDWIDE  0x0010 /* since Win 10 Aniversary Update (?) */
#endif

struct console_input_t {
  HANDLE input_handle;
  HANDLE output_handle;
  
  COORD console_size;
  WORD attr_default;
  WORD attr_fences;
  WORD attr_missing_fence;
  WORD attr_completion;
  int input_line_coord_y;
  
  wchar_t *input_text; // [input_length + 1]
  int input_capacity;
  int input_length;
  int input_pos;
  int input_anchor;
  
  /* Used to detect reflowing/word-wrapping during console resize (Windows 10) */
  COORD last_cursor_pos;
  
  int *input_to_output_positions; // [input_length + 1]
  int input_to_output_capacity;
  
  CHAR_INFO *prompt;
  int prompt_size;
  
  CHAR_INFO *continuation_prompt;
  int continuation_prompt_size;
  
  struct hyper_console_history_t *history;
  
  CHAR_INFO *output_buffer; // [output_size]
  int output_capacity;
  int output_size;
  int preferred_column;
  
  int *output_to_input_positions; // [output_size]
  int output_to_input_capacity;
  
  void *callback_context;
  BOOL (*need_more_input_predicate)(void *context, const wchar_t *buffer, int len, int cursor_pos);
  wchar_t **(*auto_completion)(void *context, const wchar_t *buffer, int len, int cursor_pos, int *completion_start, int *completion_end);
  BOOL (*key_event_filter)(          void *context, const KEY_EVENT_RECORD *er);
  BOOL (*mark_mode_key_event_filter)(void *context, const KEY_EVENT_RECORD *er);
  
  int completion_pos;
  int completion_end;
  wchar_t **completions;
  int completions_count;
  int completion_index;
  
  const char *error;
  int dirty_lines;

  DWORD old_input_mode;
  DWORD old_output_mode;
  
  DWORD next_delayed_resize_time;
  
  int tab_width;
  int first_tab_column;
  
  unsigned use_position_dependent_coloring: 1;
  unsigned have_colored_fences: 1;
  unsigned multiline_mode: 1;
  unsigned stop: 1;
  unsigned ignore_input_when_stopped: 1;
  unsigned redo_in_mark_mode: 1;
  unsigned continue_with_search: 1;
  unsigned retain_completions: 1;
  unsigned navigating_history: 1;
  unsigned no_echo: 1;
};

static BOOL is_console(HANDLE handle);
static BOOL init_console(struct console_input_t *con);
static BOOL init_buffer(struct console_input_t *con);
static void init_colors(struct console_input_t *con);
static BOOL read_prompt(struct console_input_t *con, int length);
static void set_continuation_prompt(struct console_input_t *con, const wchar_t *str, int len);
static void forget_completions(struct console_input_t *con);
static void free_console(struct console_input_t *con);

static int get_output_position_from_input_position(struct console_input_t *con, int i);
static int get_input_position_from_output_position(struct console_input_t *con, int o);
static int get_input_position_from_screen_position(struct console_input_t *con, COORD pos, BOOL nearest);

static BOOL resize_output_buffer(struct console_input_t *con, int size);
static BOOL fill_output_buffer(struct console_input_t *con);
static BOOL insert_glyphs(struct console_input_t *con, int pos, const CHAR_INFO *glyphs, int glyphs_count, int repeat);
static BOOL expand_glyphs(struct console_input_t *con);
static BOOL colorize_matching_fences(struct console_input_t *con);
static void highlight_completion(struct console_input_t *con);
static void highlight_selection(struct console_input_t *con);
static BOOL extend_output_buffer_to_full_lines(struct console_input_t *con);
static BOOL scroll_screen_if_needed(struct console_input_t *con);
static BOOL write_output_buffer_lines(struct console_input_t *con);
static BOOL set_output_cursor_position(struct console_input_t *con);

static BOOL selection_equals(struct console_input_t *con, const wchar_t *str);
static BOOL can_auto_surround(struct console_input_t *con);
static BOOL has_hard_line_break(struct console_input_t *con);

static BOOL resize_input_text(struct console_input_t *con, int length);
static BOOL insert_input_text(struct console_input_t *con, int pos, const  wchar_t *str, int length);
static BOOL insert_input_char(struct console_input_t *con, int pos, wchar_t ch);
static BOOL surround_selection(struct console_input_t *con, const wchar_t *left, const wchar_t *right);
static BOOL delete_input_text(struct console_input_t *con, int pos, int length);

static void reselect_input(struct console_input_t *con, int new_pos, int new_anchor);
static void move_left(struct console_input_t *con, BOOL fix_anchor, BOOL jump_word);
static void move_right(struct console_input_t *con, BOOL fix_anchor, BOOL jump_word);
static void move_home(struct console_input_t *con, BOOL fix_anchor);
static void move_end(struct console_input_t *con, BOOL fix_anchor);

static BOOL delete_selection_no_update(struct console_input_t *con);
static void copy_to_clipboard(struct console_input_t *con);

static BOOL begin_navigate_history(struct console_input_t *con);
static void cancel_navigate_history(struct console_input_t *con);
static void navigate_history(struct console_input_t *con, int delta);
static BOOL handle_history_key_down(struct console_input_t *con, const KEY_EVENT_RECORD *er);

static void handle_key_return(struct console_input_t *con);
static BOOL try_indent(struct console_input_t *con, BOOL forward);
static void handle_completion(struct console_input_t *con, BOOL forward);

static void handle_key_down(struct console_input_t *con, const KEY_EVENT_RECORD *er);
static void handle_key_event(struct console_input_t *con, const KEY_EVENT_RECORD *er);

static void handle_lbutton_down(struct console_input_t *con, const MOUSE_EVENT_RECORD *er);
static void handle_lbutton_move(struct console_input_t *con, const MOUSE_EVENT_RECORD *er);
static void handle_lbutton_double_click(struct console_input_t *con, const MOUSE_EVENT_RECORD *er);
static void handle_mouse_event(struct console_input_t *con, const MOUSE_EVENT_RECORD *er);

static void handle_delayed_window_buffer_size_event(struct console_input_t *con);
static void handle_window_buffer_size_event(struct console_input_t *con, const WINDOW_BUFFER_SIZE_RECORD *er);
static void handle_focus_event(struct console_input_t *con, const FOCUS_EVENT_RECORD *er);
static void handle_menu_event(struct console_input_t *con, const MENU_EVENT_RECORD *er);
static void handle_unknown_event(struct console_input_t *con, const INPUT_RECORD *ir);

static BOOL is_mouse_event_inside_edit_region(struct console_input_t *con, INPUT_RECORD *ir);

static void finish_input(struct console_input_t *con);
static BOOL set_console_modes(struct console_input_t *con);
static BOOL input_loop(struct console_input_t *con);

static struct console_input_t *get_current_input(void);

static BOOL is_console(HANDLE handle) {
  DWORD mode;
  return GetConsoleMode(handle, &mode);
}

static BOOL default_need_more_input_predicate(void *context, const wchar_t *buffer, int len, int cursor_pos) {
  return FALSE;
}

static BOOL default_multiline_need_more_input_predicate(void *context, const wchar_t *buffer, int len, int cursor_pos) {
  if(len < 1)
    return TRUE;
    
  if(buffer[len - 1] == '\n')
    return FALSE;
    
  return TRUE;
}

static wchar_t **default_auto_completion(void *context, const wchar_t *buffer, int len, int cursor_pos, int *completion_start, int *completion_end) {
  *completion_start = 0;
  *completion_end = len;
  return NULL;
}

static BOOL default_key_event_filter(void *context, const KEY_EVENT_RECORD *er) {
  return FALSE;
}


static BOOL init_console(struct console_input_t *con) {
  assert(con != NULL);
  
  memset(con, 0, sizeof(struct console_input_t));
  
  con->input_handle = GetStdHandle(STD_INPUT_HANDLE);
  con->output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  
  if(con->input_handle == INVALID_HANDLE_VALUE) {
    con->error = "GetStdHandle(STD_INPUT_HANDLE)";
    return FALSE;
  }
  
  if(con->output_handle == INVALID_HANDLE_VALUE) {
    con->error = "GetStdHandle(STD_OUTPUT_HANDLE)";
    return FALSE;
  }
  
  if(!is_console(con->input_handle) || !is_console(con->output_handle)) {
    return FALSE;
  }
  
  con->input_capacity = 256;
  con->input_text = hyper_console_allocate_memory(sizeof(wchar_t) * con->input_capacity);
  if(!con->input_text) {
    con->input_capacity = 0;
    con->error = "hyper_console_allocate_memory";
    return FALSE;
  }
  
  con->input_text[0] = L'\0';
  con->use_position_dependent_coloring = TRUE;
  
  con->preferred_column = -1;
  
  con->need_more_input_predicate  = default_need_more_input_predicate;
  con->auto_completion            = default_auto_completion;
  con->key_event_filter           = default_key_event_filter;
  con->mark_mode_key_event_filter = default_key_event_filter;
  
  con->tab_width = 8;
  
  return TRUE;
}

static BOOL init_buffer(struct console_input_t *con) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(con != NULL);
  
  if(con->error)
    return FALSE;
    
  memset(&csbi, 0, sizeof(csbi));
  if(!GetConsoleScreenBufferInfo(con->output_handle, &csbi)) {
    con->error = "GetConsoleScreenBufferInfo";
    return FALSE;
  }
  
  con->console_size = csbi.dwSize;
  con->attr_default = csbi.wAttributes;
  init_colors(con);
  
  con->input_line_coord_y = csbi.dwCursorPosition.Y;
  
  if(con->console_size.X < 1 || con->console_size.Y < 1) {
    con->error = "GetConsoleScreenBufferInfo size";
    return FALSE;
  }
  
  return read_prompt(con, csbi.dwCursorPosition.X);
}

static void init_colors(struct console_input_t *con) {
  assert(con != NULL);
  if(con->error)
    return;
    
  con->attr_fences        = con->attr_default;
  con->attr_missing_fence = con->attr_default;
  con->attr_completion    = con->attr_default;
  
  // light gray on black
  if((con->attr_default & 0xFF) == (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)) {
    // white on black;
    con->attr_fences = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  }
  else {
    // light blue on black;
    con->attr_fences = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  }
  
  
  // white on light red
  con->attr_missing_fence = BACKGROUND_RED | BACKGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  
  // white on dark blue
  con->attr_completion = BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
}

static BOOL read_prompt(struct console_input_t *con, int length) {
  COORD bufsize;
  COORD bufpos;
  SMALL_RECT region;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  if(length != con->prompt_size) {
    hyper_console_free_memory(con->prompt);
    con->prompt = NULL;
    con->prompt_size = 0;
    
    if(length > 0) {
      con->prompt_size = length;
      con->prompt = hyper_console_allocate_memory(sizeof(con->prompt[0]) * con->prompt_size);
    }
  }
  
  if(length <= 0)
    return TRUE;
    
  if(!con->prompt) {
    con->prompt_size = 0;
    con->error = "hyper_console_allocate_memory";
    return FALSE;
  }
  
  bufsize.X = (SHORT)con->prompt_size;
  bufsize.Y = 1;
  bufpos.X = 0;
  bufpos.Y = 0;
  region.Left = 0;
  region.Top = (SHORT)con->input_line_coord_y;
  region.Right = (SHORT)con->prompt_size;
  region.Bottom = region.Top;
  if(!ReadConsoleOutputW(con->output_handle, con->prompt, bufsize, bufpos, &region)) {
    hyper_console_free_memory(con->prompt);
    con->prompt = NULL;
    con->prompt_size = 0;
    con->error = "ReadConsoleOutputW";
    return FALSE;
  }
  
  return TRUE;
}

static void set_continuation_prompt(struct console_input_t *con, const wchar_t *str, int len) {
  int i;
  assert(con);
  assert(str != NULL || len <= 0);
  
  hyper_console_free_memory(con->continuation_prompt);
  con->continuation_prompt = NULL;
  con->continuation_prompt_size = 0;
  
  if(len < 0) {
    if(str)
      len = (int)wcslen(str);
    else
      len = 0;
  }
  
  if(len == 0 || len > 1000)
    return;
    
  con->continuation_prompt = hyper_console_allocate_memory(len * sizeof(CHAR_INFO));
  if(!con->continuation_prompt)
    return;
    
  con->continuation_prompt_size = len;
  for(i = 0; i < len; ++i) {
    con->continuation_prompt[i].Char.UnicodeChar = str[i];
    con->continuation_prompt[i].Attributes       = con->attr_default;
  }
}

static void forget_completions(struct console_input_t *con) {
  wchar_t **compl;
  int i;
  
  assert(con != NULL);
  
  compl = con->completions;
  for(i = 0; i < con->completions_count; ++i)
    hyper_console_free_memory(compl[i]);
    
  hyper_console_free_memory(con->completions);
  
  con->completions = NULL;
  con->completion_pos = 0;
  con->completion_end = 0;
  con->completions_count = 0;
  con->completion_index = 0;
}

static void free_console(struct console_input_t *con) {
  assert(con != NULL);
  
  hyper_console_free_memory(con->input_text);
  hyper_console_free_memory(con->prompt);
  hyper_console_free_memory(con->continuation_prompt);
  hyper_console_free_memory(con->output_buffer);
  hyper_console_free_memory(con->input_to_output_positions);
  hyper_console_free_memory(con->output_to_input_positions);
  forget_completions(con);
  memset(con, 0, sizeof(struct console_input_t));
}

static int get_output_position_from_input_position(struct console_input_t *con, int i) {
  assert(con != NULL);
  if(con->error)
    return 0;
    
  assert(i >= 0);
  assert(i <= con->input_length);
  return con->input_to_output_positions[i];
}

static int get_input_position_from_output_position(struct console_input_t *con, int o) {
  assert(con != NULL);
  if(con->error)
    return 0;
    
  if(o < 0)
    return -1;
    
  if(o > con->output_size)
    return -1;
    
  if(o >= con->output_size)
    return con->input_length;
    
  return con->output_to_input_positions[o];
}

static int get_input_position_from_screen_position(struct console_input_t *con, COORD pos, BOOL nearest) {
  int index;
  
  assert(con != NULL);
  if(con->error)
    return -1;
    
  if(pos.Y < con->input_line_coord_y) {
    if(nearest)
      return get_input_position_from_output_position(con, pos.X);
      
    return -1;
  }
  
  index = (pos.Y - con->input_line_coord_y) * con->console_size.X + pos.X;
  if(nearest) {
    while(index > con->output_size)
      index -= con->console_size.X;
  }
  else if(index < con->prompt_size)
    return -1;
    
  return get_input_position_from_output_position(con, index);
}

static BOOL resize_output_buffer(struct console_input_t *con, int size) {
  assert(con != NULL);
  
  if(con->error)
    return FALSE;
    
  if(!resize_array(
        (void**)&con->output_buffer,
        &con->output_capacity,
        sizeof(con->output_buffer[0]),
        size))
  {
    con->error = "resize_array";
    con->output_size = 0;
    return FALSE;
  }
  
  if(!resize_array(
        (void**)&con->output_to_input_positions,
        &con->output_to_input_capacity,
        sizeof(con->output_to_input_positions[0]),
        size))
  {
    con->error = "resize_array";
    con->output_size = 0;
    return FALSE;
  }
  
  con->output_size = size;
  return TRUE;
}

static BOOL fill_output_buffer(struct console_input_t *con) {
  CHAR_INFO *p;
  wchar_t *s;
  int len;
  int i;
  int *i2o;
  int *o2i;
  
  assert(con != NULL);
  
  resize_output_buffer(con, con->prompt_size + con->input_length);
  if(con->error)
    return FALSE;
    
  memcpy(con->output_buffer, con->prompt, con->prompt_size * sizeof(con->prompt[0]));
  
  o2i = con->output_to_input_positions;
  memset(o2i, 0, con->prompt_size * sizeof(o2i[0]));
  
  o2i += con->prompt_size;
  i2o = con->input_to_output_positions;
  
  p = con->output_buffer + con->prompt_size;
  s = con->input_text;
  len = con->input_length;
  for(i = 0; i < len; ++i) {
  
    p->Char.UnicodeChar = *s;
    p->Attributes = con->attr_default;
    
    *o2i = i;
    *i2o = i + con->prompt_size;
    
    ++p;
    ++s;
    ++o2i;
    ++i2o;
  }
  
  *i2o = con->output_size;
  
  return TRUE;
}

static BOOL insert_glyphs(struct console_input_t *con, int pos, const CHAR_INFO *glyphs, int glyphs_count, int repeat) {
  int i;
  int input_pos;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  assert(pos >= 0);
  assert(pos <= con->output_size);
  
  assert(repeat >= 0);
  assert(glyphs_count >= 0);
  assert(glyphs != NULL || glyphs_count == 0);
  
  resize_output_buffer(con, con->output_size + glyphs_count * repeat);
  if(con->error)
    return FALSE;
    
  memmove(
    con->output_buffer + pos + glyphs_count * repeat,
    con->output_buffer + pos,
    (con->output_size - pos - glyphs_count * repeat) * sizeof(CHAR_INFO));
  memmove(
    con->output_to_input_positions + pos + glyphs_count * repeat,
    con->output_to_input_positions + pos,
    (con->output_size - pos - glyphs_count * repeat) * sizeof(int));
    
  for(i = 0; i < repeat; ++i) {
    int j;
    for(j = 0; j < glyphs_count; ++j)
      con->output_buffer[pos + i * glyphs_count + j] = glyphs[j];
  }
  
  if(pos > 0)
    input_pos = con->output_to_input_positions[pos - 1];
  else
    input_pos = 0;
    
  for(i = 0; i < repeat * glyphs_count; ++i) {
    con->output_to_input_positions[pos + i] = input_pos;
  }
  
  for(++input_pos; input_pos <= con->input_length; ++input_pos) {
    con->input_to_output_positions[input_pos] += repeat * glyphs_count;
  }
  
  return TRUE;
}

static BOOL expand_glyphs(struct console_input_t *con) {
  int bufpos;
  int console_width;
  int tab_width;
  int tab_start;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
  
  console_width = con->console_size.X;
  assert(console_width > 0);
  
  tab_width = con->tab_width;
  tab_start = con->first_tab_column;
  
  if(tab_width > console_width)
    tab_width = console_width / 2;
    
  if(tab_width < 1)
    tab_width = 1;
  
  if(tab_start < 0)
    tab_start = 0;
  else
    tab_start = tab_start % tab_width;
    
  for(bufpos = con->prompt_size; bufpos < con->output_size; ++bufpos) {
    if(con->multiline_mode && con->output_buffer[bufpos].Char.UnicodeChar == L'\n') {
      int column = bufpos % console_width;
      
      con->output_buffer[bufpos].Char.UnicodeChar = L' ';
      if(!insert_glyphs(con, bufpos + 1, &con->output_buffer[bufpos], 1, console_width - column - 1))
        return FALSE;
        
      bufpos += console_width - column;
      if(!insert_glyphs(con, bufpos, con->continuation_prompt, con->continuation_prompt_size, 1))
        return FALSE;
        
      bufpos += con->continuation_prompt_size;
      bufpos -= 1; // will be incremented immediately
      continue;
    }
    
    if(con->output_buffer[bufpos].Char.UnicodeChar == L'\t') {
      int column = bufpos % console_width;
      int tabstop;
      
      if(column < tab_start)
        tabstop = tab_start;
      else
        tabstop = tab_start + (((column - tab_start) / tab_width) + 1) * tab_width;
      
      if(tabstop > console_width)
        tabstop = console_width;
        
      con->output_buffer[bufpos].Char.UnicodeChar = L' ';
      if(!insert_glyphs(con, bufpos + 1, &con->output_buffer[bufpos], 1, tabstop - column - 1))
        return FALSE;
        
      bufpos += tabstop - column - 1;
      continue;
    }
    
    if(con->output_buffer[bufpos].Attributes & (COMMON_LVB_LEADING_BYTE |  COMMON_LVB_TRAILING_BYTE)) 
      continue;
    
    if(2 == console_get_cell_count_for_character(con->output_buffer[bufpos].Char.UnicodeChar)) {
      CHAR_INFO cell;
      cell.Char.UnicodeChar = con->output_buffer[bufpos].Char.UnicodeChar;
      cell.Attributes = con->output_buffer[bufpos].Attributes | COMMON_LVB_TRAILING_BYTE;
      
      if(!insert_glyphs(con, bufpos + 1, &cell, 1, 1))
        return FALSE;
        
      con->output_buffer[bufpos].Attributes |= COMMON_LVB_LEADING_BYTE;
      ++bufpos;
      continue;
    }
  }
  
  return 1;
}

static BOOL colorize_matching_fences(struct console_input_t *con) {
  int other_pos;
  int pos;
  int buf_pos;
  int other_buf_pos;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  con->have_colored_fences = FALSE;
  
  if(!con->use_position_dependent_coloring)
    return FALSE;
    
  if(con->input_anchor != con->input_pos)
    return FALSE;
    
  for(pos = con->input_pos; pos >= 0 && pos >= con->input_pos - 1; --pos) {
    buf_pos = get_output_position_from_input_position(con, pos);
    
    other_pos = console_find_opposite_fence(con->input_text, con->input_length, pos);
    if(other_pos >= 0) {
      other_buf_pos = get_output_position_from_input_position(con, other_pos);
      
      con->output_buffer[buf_pos].Attributes = con->attr_fences;
      con->output_buffer[other_buf_pos].Attributes = con->attr_fences;
      
      con->have_colored_fences = TRUE;
      return TRUE;
    }
    
    if(console_get_opposite_fence(con->input_text[pos])) {
      con->output_buffer[buf_pos].Attributes = con->attr_missing_fence;
      
      con->have_colored_fences = TRUE;
      return TRUE;
    }
  }
  
  return FALSE;
}

static void highlight_completion(struct console_input_t *con) {
  assert(con != NULL);
  if(con->error)
    return;
    
  if(con->completions_count > 0) {
    int s = get_output_position_from_input_position(con, con->completion_pos);
    int e = get_output_position_from_input_position(con, con->completion_end);
    
    for(; s < e; ++s) {
      WORD attr = con->output_buffer[s].Attributes;
      WORD rest = attr & 0xFF00;
      
      con->output_buffer[s].Attributes = rest | con->attr_completion;
    }
  }
}

static void highlight_selection(struct console_input_t *con) {
  int s;
  int e;
  
  assert(con != NULL);
  if(con->error)
    return;
    
  if(con->input_anchor < con->input_pos) {
    s = con->input_anchor;
    e = con->input_pos;
  }
  else if(con->input_pos < con->input_anchor) {
    s = con->input_pos;
    e = con->input_anchor;
  }
  else
    return;
    
  s = get_output_position_from_input_position(con, s);
  e = get_output_position_from_input_position(con, e);
  for(; s < e; ++s) {
    WORD attr = con->output_buffer[s].Attributes;
    WORD foreground = attr & 0x000F;
    WORD background = attr & 0x00F0;
    WORD rest = attr & 0xFF00;
    
    con->output_buffer[s].Attributes = rest | (foreground << 4) | (background >> 4);
  }
}

static BOOL extend_output_buffer_to_full_lines(struct console_input_t *con) {
  int console_width;
  int old_size;
  int i;
  CHAR_INFO space;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  console_width = con->console_size.X;
  assert(console_width > 0);
  
  old_size = con->output_size;
  if(old_size % console_width == 0)
    return TRUE;
    
  resize_output_buffer(con, (1 + old_size / console_width) * console_width);
  if(con->error)
    return FALSE;
    
  space.Attributes = con->attr_default;
  space.Char.UnicodeChar = L' ';
  for(i = old_size; i < con->output_size; ++i) {
    con->output_buffer[i] = space;
  }
  for(i = old_size; i < con->output_size; ++i) {
    con->output_to_input_positions[i] = con->input_length;
  }
  
  return TRUE;
}

static BOOL scroll_screen_if_needed(struct console_input_t *con) {
  int last_pos;
  int cur_pos;
  int last_line;
  int cur_line;
  
  BOOL did_scroll = FALSE;
  
  assert(con != NULL);
  if(con->error)
    return did_scroll;
    
  last_pos = get_output_position_from_input_position(con, con->input_length);
  last_line = 1 + last_pos / con->console_size.X;
  if(con->input_line_coord_y + last_line > con->console_size.Y) {
    int scroll_lines = con->input_line_coord_y + last_line - con->console_size.Y;
    SMALL_RECT scroll_rect;
    COORD dst;
    CHAR_INFO space;
    
    scroll_rect.Left = 0;
    scroll_rect.Right = con->console_size.X - 1;
    scroll_rect.Top = (SHORT)scroll_lines;
    scroll_rect.Bottom = con->console_size.Y - 1;
    
    dst.X = 0;
    dst.Y = 0;
    
    space.Attributes = con->attr_default;
    space.Char.UnicodeChar = L' ';
    
    hyperlink_system_end_input();
    
    if(!ScrollConsoleScreenBufferW(con->output_handle, &scroll_rect, NULL, dst, &space)) {
      con->error = "ScrollConsoleScreenBufferW";
      return did_scroll;
    }
    
    con->input_line_coord_y -= scroll_lines;
    did_scroll = TRUE;
    
    hyperlink_system_start_input(con->console_size.X, con->input_line_coord_y);
  }
  
  cur_pos = get_output_position_from_input_position(con, con->input_pos);
  if(cur_pos > 0)
    cur_line = (cur_pos - 1) / con->console_size.X;
  else
    cur_line = 0;
    
  if(con->input_line_coord_y + cur_line < 0) {
    con->input_line_coord_y = - cur_line;
    con->dirty_lines = con->console_size.Y;
    did_scroll = TRUE;
  }
  
  if(con->input_line_coord_y < 0 && last_line <= con->console_size.Y) {
    con->input_line_coord_y = 0;
    did_scroll = TRUE;
  }
  
  return did_scroll;
}

// Only writes full lines. Does not scroll buffer. Does not update cursor.
static BOOL write_output_buffer_lines(struct console_input_t *con) {
  COORD bufsize;
  COORD bufpos;
  SMALL_RECT region;
  SHORT console_width;
  int lines;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  console_width = con->console_size.X;
  assert(console_width > 0);
  
  lines = con->output_size / console_width;
  if(lines < con->dirty_lines) {
    CHAR_INFO *empty = hyper_console_allocate_memory((con->dirty_lines - lines) * console_width * sizeof(CHAR_INFO));
    if(empty) {
      int i;
      
      CHAR_INFO space;
      space.Attributes = con->attr_default;
      space.Char.UnicodeChar = L' ';
      for(i = 0; i < (con->dirty_lines - lines) * console_width; ++i) {
        empty[i] = space;
      }
      
      bufsize.X = console_width;
      bufsize.Y = con->dirty_lines - lines;
      bufpos.X = 0;
      bufpos.Y = 0;
      region.Left = 0;
      region.Top = con->input_line_coord_y + lines;
      region.Right = console_width - 1;
      region.Bottom = region.Top + con->dirty_lines - 1;
      if(!WriteConsoleOutputW(con->output_handle, empty, bufsize, bufpos, &region)) {
        con->error = "WriteConsoleOutputW empty";
        hyper_console_free_memory(empty);
        return FALSE;
      }
      
      hyper_console_free_memory(empty);
    }
  }
  con->dirty_lines = lines;
  
  bufsize.X = console_width;
  bufsize.Y = lines;
  bufpos.X = 0;
  bufpos.Y = 0;
  region.Left = 0;
  region.Top = (SHORT)con->input_line_coord_y;
  region.Right = console_width - 1;
  region.Bottom = region.Top + lines - 1;
  
  if(bufsize.Y > 0) {
    if(!WriteConsoleOutputW(con->output_handle, con->output_buffer, bufsize, bufpos, &region)) {
      con->error = "WriteConsoleOutputW";
      return FALSE;
    }
  }
  
  return TRUE;
}

static BOOL set_output_cursor_position(struct console_input_t *con) {
  COORD pos;
  int console_width;
  int output_pos;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  console_width = con->console_size.X;
  assert(console_width > 0);
  
  assert(con->input_pos >= 0);
  assert(con->input_pos <= con->input_length);
  
  output_pos = get_output_position_from_input_position(con, con->input_pos);
  
  pos.X = output_pos % console_width;
  pos.Y = output_pos / console_width + con->input_line_coord_y;
  
  con->last_cursor_pos = pos;
  if(!SetConsoleCursorPosition(con->output_handle, pos)) {
    //  con->error = "SetConsoleCursorPosition";
    //  return 0;
  }
  
  return TRUE;
}

static BOOL update_output(struct console_input_t *con) {
  if(con->no_echo)
    return !(con->error);
  
  fill_output_buffer(con);
  colorize_matching_fences(con);
  highlight_completion(con);
  highlight_selection(con);
  expand_glyphs(con);
  extend_output_buffer_to_full_lines(con);
  scroll_screen_if_needed(con);
  write_output_buffer_lines(con);
  set_output_cursor_position(con);
  
  return !(con->error);
}

static BOOL selection_equals(struct console_input_t *con, const wchar_t *str) {
  size_t length;
  size_t start;
  size_t end;
  
  assert(con != NULL);
  assert(str != NULL);
  
  length = wcslen(str);
  
  start = (size_t)MIN(con->input_anchor, con->input_pos);
  end   = (size_t)MAX(con->input_anchor, con->input_pos);
  
  if(end - start == length) {
    return 0 == memcmp(con->input_text + start, str, length * sizeof(wchar_t));
  }
  
  return FALSE;
}

static BOOL can_auto_surround(struct console_input_t *con) {
  assert(con != NULL);
  if(con->input_anchor == con->input_pos)
    return FALSE;
    
  if(selection_equals(con, L"("))
    return FALSE;
    
  if(selection_equals(con, L")"))
    return FALSE;
    
  if(selection_equals(con, L"["))
    return FALSE;
    
  if(selection_equals(con, L"]"))
    return FALSE;
    
  if(selection_equals(con, L"{"))
    return FALSE;
    
  if(selection_equals(con, L"}"))
    return FALSE;
    
  if(selection_equals(con, L"\'"))
    return FALSE;
    
  if(selection_equals(con, L"\""))
    return FALSE;
    
  if(selection_equals(con, L"`"))
    return FALSE;
    
  return TRUE;
}

static BOOL has_hard_line_break(struct console_input_t *con) {
  int i;
  assert(con != NULL);
  if(!con->multiline_mode)
    return FALSE;
    
  for(i = 0; i < con->input_length; ++i)
    if(con->input_text[i] == L'\n')
      return TRUE;
      
  return FALSE;
}

static BOOL resize_input_text(struct console_input_t *con, int length) {
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  if(!resize_array(
        (void**)&con->input_text,
        &con->input_capacity,
        sizeof(con->input_text[0]),
        length + 1))
  {
    con->error = "resize_array";
    con->input_length = 0;
    return FALSE;
  }
  
  if(!resize_array(
        (void**)&con->input_to_output_positions,
        &con->input_to_output_capacity,
        sizeof(con->input_to_output_positions[0]),
        length + 1))
  {
    con->error = "resize_array";
    con->input_length = 0;
    return FALSE;
  }
  
  con->input_text[length] = L'\0';
  con->input_length = length;
  return TRUE;
}

static BOOL insert_input_text(struct console_input_t *con, int pos, const wchar_t *str, int length) {
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  assert(str != NULL || length == 0);
  
  assert(pos >= 0);
  assert(pos <= con->input_length);
  
  con->preferred_column = -1;
  if(!con->retain_completions) {
    forget_completions(con);
  }
  
  if(length < 0) {
    length = (int)wcslen(str);
    
    if(length < 0)
      return FALSE;
  }
  
  resize_input_text(con, con->input_length + length);
  if(con->error)
    return FALSE;
    
  memmove(
    con->input_text + pos + length,
    con->input_text + pos,
    (con->input_length - pos - length) * sizeof(wchar_t));
    
  memmove(
    con->input_text + pos,
    str,
    length * sizeof(wchar_t));
    
  if(pos <= con->input_pos)
    con->input_pos += length;
    
  if(pos <= con->input_anchor)
    con->input_anchor += length;
    
  return TRUE;
}

static BOOL insert_input_char(struct console_input_t *con, int pos, wchar_t ch) {
  return insert_input_text(con, pos, &ch, 1);
}

static BOOL surround_selection(struct console_input_t *con, const wchar_t *left, const wchar_t *right) {
  int start;
  int end;
  size_t left_length;
  size_t right_length;
  
  assert(con != NULL);
  assert(left != NULL);
  assert(right != NULL);
  
  left_length  = wcslen(left);
  right_length = wcslen(right);
  
  if(left_length > INT_MAX / 2 || right_length > INT_MAX / 2 || left_length + right_length > INT_MAX / 2)
    return FALSE;
    
  start = MIN(con->input_anchor, con->input_pos);
  end = MAX(con->input_anchor, con->input_pos);
  
  if(!insert_input_text(con, end, right, (int)right_length))
    return FALSE;
    
  end += (int)right_length;
  if(!insert_input_text(con, start, left, (int)left_length)) {
    delete_input_text(con, end, 1);
    return FALSE;
  }
  
  end += (int)left_length;
  reselect_input(con, end, start);
  return TRUE;
}

static BOOL delete_input_text(struct console_input_t *con, int pos, int length) {
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  assert(pos >= 0);
  assert(pos <= con->input_length);
  assert(length >= 0);
  assert(length <= con->input_length - pos);
  
  con->preferred_column = -1;
  if(!con->retain_completions) {
    forget_completions(con);
  }
  
  memmove(
    con->input_text + pos,
    con->input_text + pos + length,
    (con->input_length - pos - length) * sizeof(wchar_t));
    
  resize_input_text(con, con->input_length - length);
  if(con->error)
    return FALSE;
    
  if(pos + length <= con->input_pos)
    con->input_pos -= length;
  else if(pos < con->input_pos)
    con->input_pos = pos;
    
  if(pos + length <= con->input_anchor)
    con->input_anchor -= length;
  else if(pos < con->input_anchor)
    con->input_anchor = pos;
    
  return TRUE;
}

static void reselect_input(struct console_input_t *con, int new_pos, int new_anchor) {
  BOOL need_redraw;
  
  assert(con != NULL);
  if(con->error)
    return;
    
  if(new_pos < 0)
    new_pos = 0;
    
  if(new_pos > con->input_length)
    new_pos = con->input_length;
    
  if(new_anchor < 0)
    new_anchor = 0;
    
  if(new_anchor > con->input_length)
    new_anchor = con->input_length;
    
  need_redraw = (con->input_pos != con->input_anchor) || (new_pos != new_anchor) || con->have_colored_fences;
  
  con->preferred_column = -1;
  if(!con->retain_completions) {
    if(con->completions)
      need_redraw = TRUE;
      
    forget_completions(con);
  }
  
  con->input_pos = new_pos;
  con->input_anchor = new_anchor;
  
  if(need_redraw || scroll_screen_if_needed(con) || colorize_matching_fences(con))
    update_output(con);
  else
    set_output_cursor_position(con);
}

static void move_left(struct console_input_t *con, BOOL fix_anchor, BOOL jump_word) {
  int new_pos;
  
  assert(con != NULL);
  if(con->error)
    return;
    
  new_pos = con->input_pos;
  if (new_pos > 0)
    new_pos--;
    
  if(jump_word)
    new_pos = console_get_word_start(con->input_text, con->input_length, new_pos);
    
  if(fix_anchor) {
    reselect_input(con, new_pos, con->input_anchor);
  }
  else {
    if(con->input_pos < con->input_anchor)
      reselect_input(con, con->input_pos, con->input_pos);
    else if(con->input_anchor < con->input_pos)
      reselect_input(con, con->input_anchor, con->input_anchor);
    else
      reselect_input(con, new_pos, new_pos);
  }
}

static void move_right(struct console_input_t *con, BOOL fix_anchor, BOOL jump_word) {
  int new_pos;
  
  assert(con != NULL);
  if(con->error)
    return;
    
  new_pos = con->input_pos;
  if(jump_word)
    new_pos = console_get_word_end(con->input_text, con->input_length, new_pos);
  else if(new_pos < con->input_length)
    new_pos++;
    
  if(fix_anchor) {
    reselect_input(con, new_pos, con->input_anchor);
  }
  else {
    if(con->input_pos > con->input_anchor)
      reselect_input(con, con->input_pos, con->input_pos);
    else if(con->input_anchor > con->input_pos)
      reselect_input(con, con->input_anchor, con->input_anchor);
    else
      reselect_input(con, new_pos, new_pos);
  }
}

static void move_home(struct console_input_t *con, BOOL fix_anchor) {
  if(fix_anchor)
    reselect_input(con, 0, con->input_anchor);
  else
    reselect_input(con, 0, 0);
}

static void move_end(struct console_input_t *con, BOOL fix_anchor) {
  if(fix_anchor)
    reselect_input(con, con->input_length, con->input_anchor);
  else
    reselect_input(con, con->input_length, con->input_length);
}

static BOOL move_up_down(struct console_input_t *con, int direction, BOOL fix_anchor) {
  int opos;
  int ipos;
  
  assert(direction == -1 || direction == +1);
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  if(!con->multiline_mode)
    return FALSE;
    
  if(con->preferred_column >= 0)
    opos = con->preferred_column;
  else
    opos = get_output_position_from_input_position(con, con->input_anchor);
  if(direction > 0)
    opos += con->console_size.X;
  else
    opos -= con->console_size.X;
    
  ipos = get_input_position_from_output_position(con, opos);
  if(ipos < 0) {
    if(direction < 0 && con->input_pos > 0)
      ipos = 0;
    else if(direction > 0 && con->input_pos < con->input_length)
      ipos = con->input_length;
    else
      return fix_anchor; // don't walk history when shift key was pressed
  }
  
  if(fix_anchor)
    reselect_input(con, ipos, con->input_anchor);
  else
    reselect_input(con, ipos, ipos);
    
  con->preferred_column = opos;
  return TRUE;
}

static BOOL delete_selection_no_update(struct console_input_t *con) {
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  if(con->input_pos < con->input_anchor) {
    delete_input_text(con, con->input_pos, con->input_anchor - con->input_pos);
    return TRUE;
  }
  
  if(con->input_anchor < con->input_pos) {
    delete_input_text(con, con->input_anchor, con->input_pos - con->input_anchor);
    return TRUE;
  }
  
  return FALSE;
}

static void copy_to_clipboard(struct console_input_t *con) {
  int start;
  int end;
  HGLOBAL copy_handle;
  wchar_t *copy_data;
  
  assert(con != NULL);
  if(con->error)
    return;
    
  start = MIN(con->input_anchor, con->input_pos);
  end   = MAX(con->input_anchor, con->input_pos);
  if(start == end)
    return;
    
  if(!OpenClipboard(NULL))
    return;
    
  copy_handle = GlobalAlloc(GMEM_MOVEABLE, (end - start + 1) * sizeof(wchar_t));
  if(!copy_handle) {
    CloseClipboard();
    return;
  }
  
  copy_data = GlobalLock(copy_handle);
  memcpy(copy_data, con->input_text + start, (end - start) * sizeof(wchar_t));
  copy_data[end - start] = L'\0';
  GlobalUnlock(copy_handle);
  
  SetClipboardData(CF_UNICODETEXT, copy_handle);
  
  CloseClipboard();
}

static BOOL begin_navigate_history(struct console_input_t *con) {
  assert(con != NULL);
  
  if(con->navigating_history)
    return TRUE;
    
  if(has_hard_line_break(con))
    return FALSE;
    
  console_history_set_future(con->history, con->input_text, con->input_length);
  con->navigating_history = TRUE;
  //if(con->input_anchor == con->input_length && con->input_pos == con->input_length)
    return TRUE;
    
  //reselect_input(con, con->input_length, con->input_length);
  //return FALSE;
}

static void cancel_navigate_history(struct console_input_t *con) {
  assert(con != NULL);
  
  console_history_set_future(con->history, NULL, 0);
  console_history_set_index(con->history, console_history_count(con->history));
  con->navigating_history = FALSE;
}

static void navigate_history(struct console_input_t *con, int delta) {
  const wchar_t *hist_text;
  int hist_text_length;
  int index;
  
  assert(con != NULL);
  
  index = console_history_get_index(con->history);
  hist_text = console_history_get(con->history, index + delta, &hist_text_length);
  if(!hist_text) {
    int count = console_history_count(con->history);
    if(index + delta >= count) {
      index = count; 
      delta = 0;
      
      hist_text = console_history_get_future(con->history, &hist_text_length);
    }
    else
      return;
  }
  
  index += delta;
  console_history_set_index(con->history, index);
  
  con->input_anchor = 0;
  con->input_pos = con->input_length;
  delete_selection_no_update(con);
  insert_input_text(con, 0, hist_text, hist_text_length);
  update_output(con);
}

static BOOL handle_history_key_down(struct console_input_t *con, const KEY_EVENT_RECORD *er) {
  assert(con != NULL);
  assert(er != NULL);
  
  if(!con->navigating_history)
    return FALSE;
    
  switch(er->wVirtualKeyCode) {
    case VK_SHIFT:
    case VK_CONTROL:
    case VK_MENU:
      return TRUE;
      
    case VK_UP:
      if(console_scroll_key(con->output_handle, er))
        return TRUE;
      navigate_history(con, -1);
      return TRUE;
      
    case VK_DOWN:
      if(console_scroll_key(con->output_handle, er))
        return TRUE;
      navigate_history(con, +1);
      return TRUE;
      
    case VK_RETURN:
      handle_key_return(con);
      if(!con->stop)
        cancel_navigate_history(con);
      return TRUE;
  }
  
  cancel_navigate_history(con);
  return FALSE;
}

static void handle_key_return(struct console_input_t *con) {
  int pos, next_nl;
  assert(con != NULL);
  
  next_nl = pos = MAX(con->input_pos, con->input_anchor);
  while(next_nl < con->input_length && con->input_text[next_nl] != L'\n') {
    ++next_nl;
  }
  
  if(next_nl == con->input_length) { /* cursor is at last line */
    if(!con->need_more_input_predicate(con->callback_context, con->input_text, con->input_length, pos)) {
      con->stop = 1;
      return;
    }
  }
  
  delete_selection_no_update(con);
  insert_input_char(con, con->input_pos, L'\n');
  update_output(con);
  
//  if(con->multiline_mode) {
//    if(con->input_pos == con->input_length && con->input_pos > 0 && con->input_text[con->input_pos - 1] == L'\n') {
//      con->stop = 1;
//      con->input_length -= 1;
//    }
//    else {
//      delete_selection_no_update(con);
//      insert_input_char(con, con->input_pos, L'\n');
//      update_output(con);
//    }
//  }
//  else {
//    con->stop = 1;
//    con->input_pos = con->input_anchor = con->input_length;
//    if(scroll_screen_if_needed(con))
//      update_output(con);
//    set_output_cursor_position(con);
//  }
}

static BOOL try_indent(struct console_input_t *con, BOOL forward) {
  assert(con != NULL);
  
  if(con->input_anchor == con->input_pos) {
    int line_start = con->input_pos;
    while(line_start > 0) {
      switch(con->input_text[line_start - 1]) {
        case L' ':
        case L'\t':
          --line_start;
          continue;
      }
      break;
    }
    
    if(line_start > 0 && con->input_text[line_start - 1] != L'\n')
      return FALSE;
      
    if(forward) {
      insert_input_char(con, con->input_pos, L'\t');
      update_output(con);
    }
    else if(line_start < con->input_pos) {
      reselect_input(con, con->input_pos - 1, con->input_anchor);
      delete_selection_no_update(con);
      update_output(con);
    }
  }
  else {
    int start = MIN(con->input_anchor, con->input_pos);
    int end   = MAX(con->input_anchor, con->input_pos);
    BOOL have_embedded_line_breaks = FALSE;
    int i;
    
    for(i = start; i < end; ++i) {
      if(con->input_text[i] == '\n') {
        have_embedded_line_breaks = TRUE;
        break;
      }
    }
    
    if(!have_embedded_line_breaks)
      return FALSE;
      
    while(start > 0 && con->input_text[start - 1] != L'\n')
      --start;
      
    if(forward) {
      for(i = end; i > start; --i) {
        if(con->input_text[i - 1] == L'\n')
          insert_input_char(con, i, L'\t');
      }
      
      insert_input_char(con, start, L'\t');
    }
    else {
      for(i = end; i >= start; --i) {
        if(i == start || con->input_text[i - 1] == L'\n') {
          if(i < end && con->input_text[i] == L'\t') {
            delete_input_text(con, i, 1);
          }
          // TODO: unindent spaces
        }
      }
    }
    
    update_output(con);
  }
  return TRUE;
}

static void handle_completion(struct console_input_t *con, BOOL forward) {
  assert(con != NULL);
  
  con->retain_completions = TRUE;
  if(con->completions_count == 0) {
    int start = 0;
    int end = con->input_length;
    wchar_t **results;
    
    assert(con->completions == NULL);
    
    results = con->auto_completion(
                con->callback_context,
                con->input_text,
                con->input_length,
                MIN(con->input_pos, con->input_anchor),
                &start,
                &end);
                
    if(results) {
      con->completions = results;
      con->completions_count = 0;
      
      if(start < 0 || end > con->input_length || end < start) {
        forget_completions(con);
        con->retain_completions = FALSE;
        return;
      }
      
      con->completion_pos = start;
      con->completion_end = end;
      while(*results) {
        con->completions_count++;
        ++results;
      }
      
      wchar_t *orig = hyper_console_allocate_memory((end - start + 1) * sizeof(wchar_t));
      if(!orig) {
        forget_completions(con);
        con->retain_completions = FALSE;
        return;
      }
      
      memcpy(orig, con->input_text + start, (end - start) * sizeof(wchar_t));
      orig[end - start] = L'\0';
      *results = orig;
      con->completions_count++;
      
      if(forward)
        con->completion_index = -1;
      else
        con->completion_index = con->completions_count - 1;
    }
  }
  
  if(con->completions_count > 0) {
    if(forward)
      con->completion_index++;
    else
      con->completion_index--;
      
    if(con->completion_index < 0)
      con->completion_index = con->completions_count - 1;
    else if(con->completion_index >= con->completions_count)
      con->completion_index = 0;
      
    reselect_input(con, con->completion_pos, con->completion_end);
    delete_selection_no_update(con);
    insert_input_text(con, con->completion_pos, con->completions[con->completion_index], -1);
    con->completion_end = con->input_pos;
    update_output(con);
  }
  
  con->retain_completions = FALSE;
}

static void handle_key_down(struct console_input_t *con, const KEY_EVENT_RECORD *er) {
  assert(con != NULL);
  assert(er != NULL);
  
  if(handle_history_key_down(con, er))
    return;
    
  switch(er->wVirtualKeyCode) {
    case VK_RETURN:
      handle_key_return(con);
      return;
      
    case VK_BACK:
      if(con->input_anchor == con->input_pos) {
        move_left(
          con,
          TRUE,
          er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
      }
      delete_selection_no_update(con);
      update_output(con);
      return;
      
    case VK_DELETE:
      if(con->input_anchor == con->input_pos) {
        move_right(
          con,
          TRUE,
          er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
      }
      delete_selection_no_update(con);
      update_output(con);
      return;
      
    case VK_LEFT:
      move_left(
        con,
        er->dwControlKeyState & SHIFT_PRESSED,
        er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
      return;
      
    case VK_RIGHT:
      move_right(
        con,
        er->dwControlKeyState & SHIFT_PRESSED,
        er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
      return;
      
    case VK_HOME:
      move_home(con, er->dwControlKeyState & SHIFT_PRESSED);
      return;
      
    case VK_END:
      move_end(con, er->dwControlKeyState & SHIFT_PRESSED);
      return;
      
    case VK_UP:
      if(console_scroll_key(con->output_handle, er))
        return;
      if(move_up_down(con, -1, er->dwControlKeyState & SHIFT_PRESSED))
        return;
      if(begin_navigate_history(con))
        navigate_history(con, -1);
      return;
      
    case VK_DOWN:
      if(console_scroll_key(con->output_handle, er))
        return;
      if(move_up_down(con, +1, er->dwControlKeyState & SHIFT_PRESSED))
        return;
      if(begin_navigate_history(con))
        navigate_history(con, +1);
      return;
      
    case VK_PRIOR:
    case VK_NEXT:
      console_scroll_key(con->output_handle, er);
      return;
      
    case VK_ESCAPE:
      if(con->completions_count > 0) {
        con->completion_index = 0;
        handle_completion(con, FALSE);
        forget_completions(con);
        update_output(con);
      }
      else {
        con->input_anchor = 0;
        con->input_pos = con->input_length;
        console_history_set_index(con->history, console_history_count(con->history));
        delete_selection_no_update(con);
        update_output(con);
      }
      return;
      
    case 'A': // Ctrl+A
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        if(abs(con->input_anchor - con->input_pos) == con->input_length) {
          con->redo_in_mark_mode = TRUE;
        }
        else {
          reselect_input(con, con->input_length, 0);
        }
        return;
      }
      break;
      
    case VK_INSERT: // Ctrl+Ins = copy, Shift+Ins = paste
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        copy_to_clipboard(con);
        return;
      }
      if(er->dwControlKeyState & SHIFT_PRESSED) {
        console_paste_from_clipboard(con->input_handle);
        return;
      }
      break;
      
    case 'C': // Ctrl+C
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        if(con->input_anchor != con->input_pos) {
          copy_to_clipboard(con);
          reselect_input(con, con->input_pos, con->input_pos);
          return;
        }
        else { // empty selection -> abort edit
          con->stop = TRUE;
          con->ignore_input_when_stopped = TRUE;
          return;
        }
      }
      break;
      
    case 'V': // Ctrl+V
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        console_paste_from_clipboard(con->input_handle);
        return;
      }
      break;
      
    case 'X': // Ctrl+X
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        copy_to_clipboard(con);
        delete_selection_no_update(con);
        update_output(con);
        
        return;
      }
      break;
      
    case 'F': // Ctrl+F
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        con->continue_with_search = TRUE;
        return;
      }
      break;
      
    case VK_TAB:
      if(er->dwControlKeyState & LITERAL_KEY_STATE) {
        delete_selection_no_update(con);
        insert_input_char(con, con->input_pos, er->uChar.UnicodeChar);
        update_output(con);
      }
      else if(!try_indent(con, !(er->dwControlKeyState & (SHIFT_PRESSED))))
        handle_completion(con, !(er->dwControlKeyState & (SHIFT_PRESSED)));
      return;
  }
  
  if(can_auto_surround(con)) {
    switch(er->uChar.UnicodeChar) {
      case L'(':
      case L')':
        surround_selection(con, L"(", L")");
        return;
        
      case L'[':
      case L']':
        surround_selection(con, L"[", L"]");
        return;
        
      case L'{':
      case L'}':
        surround_selection(con, L"{", L"}");
        return;
        
      case L'\"':
        surround_selection(con, L"\"", L"\"");
        return;
        
      case L'\'':
        surround_selection(con, L"\'", L"\'");
        return;
        
      case L'`':
        surround_selection(con, L"`", L"`");
        return;
    }
  }
  
  if((unsigned)er->uChar.UnicodeChar >= (unsigned)L' ') {
    delete_selection_no_update(con);
    insert_input_char(con, con->input_pos, er->uChar.UnicodeChar);
    update_output(con);
  }
}

static void handle_key_event(struct console_input_t *con, const KEY_EVENT_RECORD *er) {
  assert(con != NULL);
  assert(er != NULL);
  
  if(con->key_event_filter(con->callback_context, er))
    return;
  
  if(er->bKeyDown)
    handle_key_down(con, er);
}

static void handle_lbutton_down(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  int i;
  
  assert(con != NULL);
  assert(er != NULL);
  
  cancel_navigate_history(con);
  i = get_input_position_from_screen_position(con, er->dwMousePosition, TRUE);
  
  if(i >= 0) {
    BOOL need_redraw = (con->input_anchor != con->input_pos);
    
    con->input_pos = i;
    con->input_anchor = i;
    
    if(need_redraw || scroll_screen_if_needed(con))
      update_output(con);
    else
      set_output_cursor_position(con);
  }
}

static void handle_lbutton_move(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  int i;
  
  assert(con != NULL);
  assert(er != NULL);
  
  i = get_input_position_from_screen_position(con, er->dwMousePosition, TRUE);
  if(i >= 0 && i != con->input_pos) {
    con->input_pos = i;
    update_output(con);
  }
}

static void handle_lbutton_double_click(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  int i;
  
  assert(con != NULL);
  assert(er != NULL);
  if(con->error)
    return;
    
  i = get_input_position_from_screen_position(con, er->dwMousePosition, FALSE);
  
  if(i >= 0) {
    int s = console_get_word_start(con->input_text, con->input_length, i);
    int e = console_get_word_end(con->input_text, con->input_length, i);
    
    con->input_anchor = s;
    con->input_pos = e;
    
    update_output(con);
  }
}

static void handle_mouse_event(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  assert(er != NULL);
  
  switch(er->dwEventFlags) {
    case 0:
      // button press/release
      debug_printf(L"handle_mouse_event press/release %x\n", er->dwButtonState);
      
      if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
        handle_lbutton_down(con, er);
        return;
      }
      break;
      
    case DOUBLE_CLICK:
      debug_printf(L"handle_mouse_event DOUBLE_CLICK %x\n", er->dwButtonState);
      
      if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
        handle_lbutton_double_click(con, er);
        return;
      }
      break;
      
    case MOUSE_MOVED:
      if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
        handle_lbutton_move(con, er);
      }
      break;
      
    case MOUSE_HWHEELED:
    case MOUSE_WHEELED:
      console_scroll_wheel(con->output_handle, er);
      break;
  }
}

static void handle_delayed_window_buffer_size_event(struct console_input_t *con) {CONSOLE_SCREEN_BUFFER_INFO csbi;
  assert(con != NULL);
  
  con->next_delayed_resize_time = 0;
  
  if(!GetConsoleScreenBufferInfo(con->output_handle, &csbi)) {
    con->error = "GetConsoleScreenBufferInfo";
    return;
  }
  
  debug_printf(
    L"buffer_size (%dx%d -> %dx%d) %d:%d -> %d:%d; linear = %d\n",
    (int)con->console_size.Y,
    (int)con->console_size.X,
    (int)csbi.dwSize.Y,
    (int)csbi.dwSize.X,
    (int)con->last_cursor_pos.Y,
    (int)con->last_cursor_pos.X,
    (int)csbi.dwCursorPosition.Y,
    (int)csbi.dwCursorPosition.X,
    con->prompt_size + con->input_pos);
  
  if( csbi.dwSize.X == con->console_size.X && 
      csbi.dwSize.Y == con->console_size.Y)
  {
    // Windows 10 sends spurious WINDOW_BUFFER_SIZE_EVENT when scolling via mouse wheel.
    return;
  }
    
  con->console_size = csbi.dwSize;
  
  if(csbi.dwCursorPosition.Y != con->last_cursor_pos.Y || csbi.dwCursorPosition.X != con->last_cursor_pos.X) {
    int linear_cursor_pos = con->prompt_size + con->input_pos;
    
    int lines = linear_cursor_pos / csbi.dwSize.X;
    int rest = linear_cursor_pos % csbi.dwSize.X;
    
    if(rest != csbi.dwCursorPosition.X) {
      /* Strange! If the cursor moved due to line-unwrapping, then its column position
         should be exactly rest.
       */
      debug_printf(
        L"incomplete unwrap: cursor.X = %d != %d\n",
        (int)csbi.dwCursorPosition.X,
        rest);
    }
    
    if(con->input_line_coord_y != csbi.dwCursorPosition.Y - lines) {
      debug_printf(
        L"move input_line_coord_y from %d to %d\n",
        con->input_line_coord_y,
        csbi.dwCursorPosition.Y - lines);
    }
    
    con->input_line_coord_y = csbi.dwCursorPosition.Y - lines;
  }
  
  console_clean_lines(con->output_handle, con->input_line_coord_y);
  
  hyperlink_system_update_scollback(con->input_line_coord_y);
  hyperlink_system_end_input();
  update_output(con);
  hyperlink_system_start_input(con->console_size.X, con->input_line_coord_y);
}

static void handle_window_buffer_size_event(struct console_input_t *con, const WINDOW_BUFFER_SIZE_RECORD *er) {
  assert(con != NULL);
  assert(er != NULL);
  
  con->next_delayed_resize_time = GetTickCount() + 300;
}

static void handle_focus_event(struct console_input_t *con, const FOCUS_EVENT_RECORD *er) {
  assert(con != NULL);
  assert(er != NULL);
  
  debug_printf(L"handle_focus_event %d\n", (int)er->bSetFocus);
}

static void handle_menu_event(struct console_input_t *con, const MENU_EVENT_RECORD *er) {
  assert(con != NULL);
  assert(er != NULL);
  
  debug_printf(L"handle_menu_event %d\n", (int)er->dwCommandId);
}

static void handle_unknown_event(struct console_input_t *con, const INPUT_RECORD *ir) {
  wchar_t buffer[100];
  
  assert(con != NULL);
  assert(ir != NULL);
  
  StringCbPrintfW(buffer, sizeof(buffer), L"Unknown INPUT_RECORD %d\n", (int)ir->EventType);
  
  OutputDebugStringW(buffer);
}


static BOOL is_mouse_event_inside_edit_region(struct console_input_t *con, INPUT_RECORD *ir) {
  int i;
  
  assert(con != NULL);
  assert(ir != NULL);
  
  if(ir->EventType != MOUSE_EVENT)
    return FALSE;
    
  i = get_input_position_from_screen_position(con, ir->Event.MouseEvent.dwMousePosition, FALSE);
  return i >= 0;
}

static void finish_input(struct console_input_t *con) {
  assert(con != NULL);
  
  if(con->error)
    return;
    
  forget_completions(con);
  
  con->input_text[con->input_length] = L'\0';
  con->input_pos = con->input_anchor = con->input_length;
  con->use_position_dependent_coloring = FALSE;
  update_output(con);
}

static BOOL set_console_modes(struct console_input_t *con) {
  assert(con != NULL);

  if(!SetConsoleMode(con->input_handle, ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS)) {
    con->error = "SetConsoleMode on input_handle";
    return FALSE;
  }
  
  if(!SetConsoleMode(con->output_handle, ENABLE_LVB_GRID_WORLDWIDE)) {
    /* Trying to set ENABLE_LVB_GRID_WORLDWIDE will fail on pre-Win10 (v10.0.14393) systems
       with "Invalid Parameter".
       But this setting is not essential for correct functioning, so we ignore any error.
     */
    //con->error = "SetConsoleMode on output_handle";
    //return FALSE;
  }
  
  return TRUE;
}

static BOOL input_loop(struct console_input_t *con) {
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  if(!GetConsoleMode(con->input_handle, &con->old_input_mode)) {
    con->error = "GetConsoleMode on input_handle";
    return FALSE;
  }
  
  if(!GetConsoleMode(con->output_handle, &con->old_output_mode)) {
    con->error = "GetConsoleMode on input_handle";
    return FALSE;
  }
  
  if(!set_console_modes(con))
    return FALSE;

  hyperlink_system_start_input(con->console_size.X, con->input_line_coord_y);
  
  // There might be some links in the prompt, which just changed their color.
  read_prompt(con, con->prompt_size);
  
  while(!con->stop && !con->error) {
    INPUT_RECORD event;
    DWORD num_read;
    DWORD timeout = INFINITE;
    
    if(con->next_delayed_resize_time) {
      timeout = con->next_delayed_resize_time - GetTickCount();
    }
    
    num_read = WaitForSingleObject(con->input_handle, timeout);
    if(con->next_delayed_resize_time) {
      if(con->next_delayed_resize_time <= GetTickCount()) {
        con->next_delayed_resize_time = 0;
        handle_delayed_window_buffer_size_event(con);
      }
    }
    
    switch(num_read) {
      case WAIT_TIMEOUT: continue;
      case WAIT_OBJECT_0: break;
      case WAIT_ABANDONED: 
        debug_printf(L"WaitForSingleObject: abandoned\n");
        break;
      case WAIT_FAILED: 
        debug_printf(L"WaitForSingleObject failed\n");
        break;
    }
    
    num_read = 0;
    if(!ReadConsoleInputW(con->input_handle, &event, 1, &num_read) || num_read < 1) {
      con->error = "ReadConsoleInputW";
      break;
    }
    
    if(con->continue_with_search) {
      wchar_t *filter;
      BOOL event_eaten;
      int start = MIN(con->input_anchor, con->input_pos);
      int end   = MAX(con->input_anchor, con->input_pos);
      int length = end - start;
      
      con->continue_with_search = FALSE;
      
      filter = hyper_console_allocate_memory((length + 1) * sizeof(wchar_t));
      if(filter) {
        memcpy(filter, con->input_text + start, length * sizeof(wchar_t));
        filter[length] = L'\0';
        
        event_eaten = console_handle_search_mode(con->input_handle, con->output_handle, &event, filter);
        
        hyper_console_free_memory(filter);
        if(event_eaten)
          continue;
      }
    }
    
    if(!is_mouse_event_inside_edit_region(con, &event)) {
      if(hyperlink_system_handle_events(&event))
        continue;
        
//      if(console_handle_search_mode(con->input_handle, con->output_handle, &event, NULL))
//        continue;
      
      struct mark_mode_settings_t mm = { 
        .input_handle     = con->input_handle, 
        .output_handle    = con->output_handle,
        .callback_context = con->callback_context,
        .key_event_filter = con->mark_mode_key_event_filter };
      if(console_handle_mark_mode(&mm, &event, FALSE))
        continue;
    }
    
    for(;;) {
      switch(event.EventType) {
        case KEY_EVENT:
          handle_key_event(con, &event.Event.KeyEvent);
          break;
          
        case MOUSE_EVENT:
          handle_mouse_event(con, &event.Event.MouseEvent);
          break;
          
        case WINDOW_BUFFER_SIZE_EVENT: // scrn buf. resizing
          handle_window_buffer_size_event(con, &event.Event.WindowBufferSizeEvent);
          break;
          
        case FOCUS_EVENT:
          handle_focus_event(con, &event.Event.FocusEvent);
          break;
          
        case MENU_EVENT:   // disregard menu events
          handle_menu_event(con, &event.Event.MenuEvent);
          break;
          
        default:
          handle_unknown_event(con, &event);
          break;
      }
      
      if(con->redo_in_mark_mode) {
        con->redo_in_mark_mode = FALSE;
        
        struct mark_mode_settings_t mm = { 
          .input_handle     = con->input_handle, 
          .output_handle    = con->output_handle,
          .callback_context = con->callback_context,
          .key_event_filter = con->mark_mode_key_event_filter };
        if(console_handle_mark_mode(&mm, &event, TRUE))
          break;
      }
      else
        break;
    }
  }
  
  finish_input(con);
  
  hyperlink_system_end_input();
  
  SetConsoleMode(con->input_handle, con->old_input_mode);
  SetConsoleMode(con->output_handle, con->old_output_mode);
  
  if(!WriteConsoleA(con->output_handle, "\n", 1, NULL, NULL))
    con->error = "WriteConsoleA";
    
  return !con->error;
}

HYPER_CONSOLE_API
void hyper_console_interrupt(void (*callback)(void*), void *callback_arg) {
  struct console_input_t *con;
  
  assert(callback != NULL);
  
  con = get_current_input();
  if(con) {
    COORD pos;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    struct dangling_hyperlinks_t *prompt_links;
    int global_line_before;
    int global_line_after;
    int dummy_column;
    
    con->output_size = 0;
    write_output_buffer_lines(con);
    pos.X = 0;
    pos.Y = con->input_line_coord_y;
    if(!SetConsoleCursorPosition(con->output_handle, pos)) {
      debug_printf(L"hyper_console_interrupt: SetConsoleCursorPosition failed.");
    }
    
    if(!hyperlink_system_local_to_global(pos, &global_line_before, &dummy_column)) {
      debug_printf(L"hyper_console_interrupt: cannot get global_line_before.");
    }
    
    SetConsoleMode(con->input_handle, con->old_input_mode);
    SetConsoleMode(con->output_handle, con->old_output_mode);
    
    hyperlink_system_end_input();
    prompt_links = hyperlink_system_cut_links_after_cursor();
    
    callback(callback_arg);
    
    set_console_modes(con);
    if(!GetConsoleScreenBufferInfo(con->output_handle, &csbi)) {
      con->error = "GetConsoleScreenBufferInfo in hyper_console_interrupt";
      con->stop = TRUE;
      return;
    }
    
    con->console_size = csbi.dwSize;
    con->input_line_coord_y = csbi.dwCursorPosition.Y;
    if(csbi.dwCursorPosition.X > 0) {
      // might be in last line: scrolling and adjusting input_line_coord_y happens in update_output()
      con->input_line_coord_y++; 
    }
    
    update_output(con);
    
    hyperlink_system_start_input(con->console_size.X, con->input_line_coord_y);
    
    pos.Y = con->input_line_coord_y;
    if(!hyperlink_system_local_to_global(pos, &global_line_after, &dummy_column)) {
      debug_printf(L"hyper_console_interrupt: cannot get global_line_after.");
    }
    
    hyperlink_system_move_links(prompt_links, global_line_after - global_line_before);
    hyperlink_system_paste_and_activate_links(prompt_links);
  }
  else {
    callback(callback_arg);
  }
}

static void print_error(void) {
  wchar_t *msgbuf;
  DWORD dw = GetLastError();
  
  FormatMessageW(
    FORMAT_MESSAGE_ALLOCATE_BUFFER |
    FORMAT_MESSAGE_FROM_SYSTEM |
    FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    dw,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    (wchar_t*)&msgbuf,
    0, NULL );
    
  fwprintf(stderr, L"win32 error %x: %s", (unsigned)dw, msgbuf);
  
  LocalFree(msgbuf);
}

#ifdef __GNUC__
static __thread
#else
static __declspec( thread )
#endif
struct console_input_t *current_input_console = NULL;

static struct console_input_t *get_current_input(void) {
  return current_input_console;
}

static wchar_t *read_file(FILE *file, void *callback_context, BOOL (*need_more_input_predicate)(void*, const wchar_t*, int, int)) {
  wchar_t buffer[256];
  
  wchar_t *str = NULL;
  int str_capacity = 0;
  int str_len = 0;
  
  while(fgetws(buffer, ARRAYSIZE(buffer), file)) {
    int len = (int)wcslen(buffer);
    
    if(len <= 0)
      return str;
      
    if(!resize_array((void**)&str, &str_capacity, sizeof(wchar_t), str_len + len))
      break;
      
    memcpy(str + str_len, buffer, len * sizeof(wchar_t));
    str_len += len;
    
    if(str[str_len - 1] == L'\n') {
      str[str_len - 1] = L'\0';
      if(!need_more_input_predicate(callback_context, str, str_len - 1, str_len - 1))
        return str;
        
      str[str_len - 1] = L'\n';
//      if(multiline_mode) {
//        if(str_len > 1 && str[str_len - 2] == L'\n') {
//          str[str_len - 2] = L'\0';
//          return str;
//        }
//      }
//      else {
//        str[str_len - 1] = L'\0';
//        return str;
//      }
    }
  }
  
  hyper_console_free_memory(str);
  return NULL;
}

#define HAVE_SETTINGS(settings, name) ((settings) && (settings)->size >= offsetof(struct hyper_console_settings_t, name))

HYPER_CONSOLE_API
wchar_t *hyper_console_readline(struct hyper_console_settings_t *settings) {
  struct console_input_t con[1];
  struct console_input_t *old_con;
  BOOL ignore_default_input = FALSE;
  
  if(!init_console(con)) {
    void *callback_context = NULL;
    BOOL (*need_more_input_predicate)(void*, const wchar_t*, int, int) = default_need_more_input_predicate;
    
    if(HAVE_SETTINGS(settings, flags)) {
      if((settings->flags & HYPER_CONSOLE_FLAGS_MULTILINE) != 0)
        need_more_input_predicate = default_multiline_need_more_input_predicate;
    }
    
    if(HAVE_SETTINGS(settings, callback_context)) {
      callback_context = settings->callback_context;
    }
    
    if(HAVE_SETTINGS(settings, need_more_input_predicate) && settings->need_more_input_predicate) {
      need_more_input_predicate = settings->need_more_input_predicate;
    }
    
    /* settings->default_input is ignored */
    fflush(stdout);
    return read_file(stdin, callback_context, need_more_input_predicate);
  }
  
  if(HAVE_SETTINGS(settings, flags)) {
    if((settings->flags & HYPER_CONSOLE_FLAGS_MULTILINE) != 0) {
      con->multiline_mode = TRUE;
      con->need_more_input_predicate = default_multiline_need_more_input_predicate;
    }
    
    if((settings->flags & HYPER_CONSOLE_FLAGS_NO_ECHO) != 0) {
      con->no_echo = TRUE;
    }
  }
  
  if(HAVE_SETTINGS(settings, history)) {
    const wchar_t *text = NULL;
    int text_length = 0;
    
    con->history = settings->history;
    
    text = console_history_get_future(con->history, &text_length);
    if(text) {
      ignore_default_input = TRUE;
      insert_input_text(con, 0, text, text_length);
      con->input_anchor = 0;
    }
    console_history_set_future(con->history, NULL, 0);
  }
  
  if(HAVE_SETTINGS(settings, callback_context)) {
    con->callback_context = settings->callback_context;
  }
  
  if(HAVE_SETTINGS(settings, need_more_input_predicate) && settings->need_more_input_predicate) {
    con->need_more_input_predicate = settings->need_more_input_predicate;
  }
  
  if(HAVE_SETTINGS(settings, auto_completion) && settings->auto_completion) {
    con->auto_completion = settings->auto_completion;
  }
  
  if(HAVE_SETTINGS(settings, key_event_filter) && settings->key_event_filter) {
    con->key_event_filter = settings->key_event_filter;
  }
  
  if(HAVE_SETTINGS(settings, mark_mode_key_event_filter) && settings->mark_mode_key_event_filter) {
    con->mark_mode_key_event_filter = settings->mark_mode_key_event_filter;
  }
  
  if(HAVE_SETTINGS(settings, tab_width) && settings->tab_width) {
    con->tab_width = settings->tab_width;
  }
  
  if(HAVE_SETTINGS(settings, first_tab_column) && settings->first_tab_column) {
    con->first_tab_column = settings->first_tab_column;
  }
  
  init_buffer(con);
  
  if(HAVE_SETTINGS(settings, line_continuation_prompt)) {
    set_continuation_prompt(con, settings->line_continuation_prompt, -1);
  }
  
  if(!ignore_default_input && HAVE_SETTINGS(settings, default_input) && settings->default_input) {
    insert_input_text(con, 0, settings->default_input, -1);
    con->input_anchor = 0;
  }
  update_output(con);
  
  old_con = current_input_console;
  current_input_console = con;
  
  input_loop(con);
  
  current_input_console = old_con;
  
  if(con->error) {
    fprintf(stderr, "input failed. ");
    fprintf(stderr, "%s ", con->error);
    print_error();
    fprintf(stderr, "\n");
    
    free_console(con);
    return NULL;
  }
  
  if(!con->ignore_input_when_stopped) {
    wchar_t *result = con->input_text;
    int index = console_history_get_index(con->history);
    
    console_history_add(con->history, con->input_text, con->input_length);
    
    if(con->navigating_history)
      console_history_set_index(con->history, index);
    
    if( con->need_more_input_predicate == default_multiline_need_more_input_predicate &&
        con->input_length > 0 &&
        result[con->input_length - 1] == L'\n')
    {
      /* when HYPER_CONSOLE_FLAGS_MULTILINE was given, trim last line break */
      result[con->input_length - 1] = L'\0';
    }
    
    con->input_text = NULL;
    con->input_capacity = 0;
    free_console(con);
    return result;
  }
  
  free_console(con);
  return NULL;
}

HYPER_CONSOLE_API
const wchar_t *hyper_console_get_current_input(int *length) {
  struct console_input_t *con = get_current_input();
  
  assert(length != NULL);
  *length = 0;
  
  if(!con)
    return NULL;
  
  *length = con->input_length;
  con->input_text[con->input_length] = L'\0';
  return con->input_text;
}

HYPER_CONSOLE_API
void hyper_console_get_current_selection(int *position, int *anchor) {
  struct console_input_t *con = get_current_input();
  
  assert(position != NULL);
  assert(anchor != NULL);
  
  if(con) {
    *position = con->input_pos;
    *anchor = con->input_anchor;
  }
  else {
    *position = 0;
    *anchor = 0;
  }
}

HYPER_CONSOLE_API
void hyper_console_set_current_selection(int position, int anchor) {
  struct console_input_t *con = get_current_input();
  
  if(!con)
    return;
  
  reselect_input(con, position, anchor);
}

BOOL stop_current_input(BOOL do_abort, const wchar_t *opt_replace_input) {
  struct console_input_t *con;
  
  con = get_current_input();
  if(con == NULL || con->error)
    return FALSE;
    
  if(opt_replace_input) {
    if(!do_abort) 
      console_history_set_future(con->history, con->input_text, con->input_length);
    
    delete_input_text(con, 0, con->input_length);
    insert_input_text(con, 0, opt_replace_input, -1);
    //update_output(con);
  }
  
  con->stop = TRUE;
  con->ignore_input_when_stopped = do_abort;
  return TRUE;
}
