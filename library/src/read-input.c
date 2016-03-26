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


struct console_input_t {
  HANDLE input_handle;
  HANDLE output_handle;
  
  COORD console_size;
  WORD attr_default;
  WORD attr_fences;
  WORD attr_missing_fence;
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
  
  struct hyper_console_history_t *history;
  int history_index;
  
  CHAR_INFO *output_buffer; // [output_size]
  int output_capacity;
  int output_size;
  
  int *output_to_input_positions; // [output_size]
  int output_to_input_capacity;
  
  const char *error;
  int dirty_lines;
  
  unsigned use_position_dependent_coloring: 1;
  unsigned have_colored_fences: 1;
  unsigned multiline_mode: 1;
  unsigned stop: 1;
  unsigned ignore_input_when_stopped: 1;
  unsigned redo_in_mark_mode: 1;
  unsigned continue_with_search: 1;
};

static BOOL is_console(HANDLE handle);
static BOOL init_console(struct console_input_t *con);
static BOOL init_buffer(struct console_input_t *con);
static void init_colors(struct console_input_t *con);
static BOOL read_prompt(struct console_input_t *con, int length);
static void free_console(struct console_input_t *con);

static int get_output_position_from_input_position(struct console_input_t *con, int i);
static int get_input_position_from_output_position(struct console_input_t *con, int o);
static int get_input_position_from_screen_position(struct console_input_t *con, COORD pos, BOOL nearest);

static BOOL resize_output_buffer(struct console_input_t *con, int size);
static BOOL fill_output_buffer(struct console_input_t *con);
static BOOL expand_glyphs(struct console_input_t *con);
static BOOL colorize_matching_fences(struct console_input_t *con);
static void highlight_selection(struct console_input_t *con);
static BOOL extend_output_buffer_to_full_lines(struct console_input_t *con);
static BOOL scroll_screen_if_needed(struct console_input_t *con);
static BOOL write_output_buffer_lines(struct console_input_t *con);
static BOOL set_output_cursor_position(struct console_input_t *con);

static BOOL selection_equals(struct console_input_t *con, const wchar_t *str);
static BOOL can_auto_surround(struct console_input_t *con);

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

static void handle_key_down(struct console_input_t *con, const KEY_EVENT_RECORD *er);
static void handle_key_event(struct console_input_t *con, const KEY_EVENT_RECORD *er);

static void handle_lbutton_down(struct console_input_t *con, const MOUSE_EVENT_RECORD *er);
static void handle_lbutton_move(struct console_input_t *con, const MOUSE_EVENT_RECORD *er);
static void handle_lbutton_double_click(struct console_input_t *con, const MOUSE_EVENT_RECORD *er);
static void handle_mouse_event(struct console_input_t *con, const MOUSE_EVENT_RECORD *er);

static void handle_window_buffer_size_event(struct console_input_t *con, const WINDOW_BUFFER_SIZE_RECORD *er);
static void handle_focus_event(struct console_input_t *con, const FOCUS_EVENT_RECORD *er);
static void handle_menu_event(struct console_input_t *con, const MENU_EVENT_RECORD *er);
static void handle_unknown_event(struct console_input_t *con, const INPUT_RECORD *ir);

static BOOL is_mouse_event_inside_edit_region(struct console_input_t *con, INPUT_RECORD *ir);

static void finish_input(struct console_input_t *con);
static BOOL input_loop(struct console_input_t *con);

static struct console_input_t *get_current_input(void);

static BOOL is_console(HANDLE handle) {
  DWORD mode;
  return GetConsoleMode(handle, &mode);
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
  con->input_text = allocate_memory(sizeof(wchar_t) * con->input_capacity);
  if(!con->input_text) {
    con->input_capacity = 0;
    con->error = "allocate_memory";
    return FALSE;
  }
  
  con->input_text[0] = L'\0';
  con->use_position_dependent_coloring = TRUE;
  
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
    
  con->attr_fences = con->attr_default;
  con->attr_missing_fence = con->attr_default;
  
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
      con->prompt = allocate_memory(sizeof(con->prompt[0]) * con->prompt_size);
    }
  }
  
  if(length <= 0)
    return TRUE;
    
  if(!con->prompt) {
    con->prompt_size = 0;
    con->error = "allocate_memory";
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

static void free_console(struct console_input_t *con) {
  assert(con != NULL);
  
  hyper_console_free_memory(con->input_text);
  hyper_console_free_memory(con->prompt);
  hyper_console_free_memory(con->output_buffer);
  hyper_console_free_memory(con->input_to_output_positions);
  hyper_console_free_memory(con->output_to_input_positions);
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

static BOOL insert_glyph(struct console_input_t *con, int pos, CHAR_INFO glyph, int repeat) {
  int i;
  int input_pos;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  assert(pos >= 0);
  assert(pos <= con->output_size);
  
  assert(repeat >= 0);
  
  resize_output_buffer(con, con->output_size + repeat);
  if(con->error)
    return FALSE;
    
  memmove(
    con->output_buffer + pos + repeat,
    con->output_buffer + pos,
    (con->output_size - pos - repeat) * sizeof(CHAR_INFO));
  memmove(
    con->output_to_input_positions + pos + repeat,
    con->output_to_input_positions + pos,
    (con->output_size - pos - repeat) * sizeof(int));
    
  for(i = 0; i < repeat; ++i) {
    con->output_buffer[pos + i] = glyph;
  }
  
  if(pos > 0)
    input_pos = con->output_to_input_positions[pos - 1];
  else
    input_pos = 0;
    
  for(i = 0; i < repeat; ++i) {
    con->output_to_input_positions[pos + i] = input_pos;
  }
  
  for(++input_pos; input_pos <= con->input_length; ++input_pos) {
    con->input_to_output_positions[input_pos] += repeat;
  }
  
  return TRUE;
}

static BOOL expand_glyphs(struct console_input_t *con) {
  int bufpos;
  int console_width;
  int tab_width = 8;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  console_width = con->console_size.X;
  assert(console_width > 0);
  
  if(tab_width > console_width)
    tab_width = console_width / 2;
    
  if(tab_width < 1)
    tab_width = 1;
    
  for(bufpos = con->prompt_size; bufpos < con->output_size; ++bufpos) {
    if(con->multiline_mode && con->output_buffer[bufpos].Char.UnicodeChar == L'\n') {
      int column = bufpos % console_width;
      
      con->output_buffer[bufpos].Char.UnicodeChar = L' ';
      if(!insert_glyph(con, bufpos + 1, con->output_buffer[bufpos], console_width - column - 1))
        return FALSE;
        
      bufpos += console_width - column - 1;
      continue;
    }
    
    if(con->output_buffer[bufpos].Char.UnicodeChar == L'\t') {
      int column = bufpos % console_width;
      int tabstop = ((column / tab_width) + 1) * tab_width;
      
      if(tabstop > console_width)
        tabstop = console_width;
        
      con->output_buffer[bufpos].Char.UnicodeChar = L' ';
      if(!insert_glyph(con, bufpos + 1, con->output_buffer[bufpos], tabstop - column - 1))
        return FALSE;
        
      bufpos += tabstop - column - 1;
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
    CHAR_INFO *empty = allocate_memory((con->dirty_lines - lines) * console_width * sizeof(CHAR_INFO));
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
  fill_output_buffer(con);
  colorize_matching_fences(con);
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
    
  end+= (int)right_length;
  if(!insert_input_text(con, start, left, (int)left_length)) {
    delete_input_text(con, end, 1);
    return FALSE;
  }
  
  end+= (int)left_length;
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

static void navigate_history(struct console_input_t *con, int delta) {
  const wchar_t *hist_text;
  int hist_text_length;
  
  assert(con != NULL);
  
  hist_text = console_history_get(con->history, con->history_index + delta, &hist_text_length);
  if(!hist_text) {
    int count = console_history_count(con->history);
    if(con->history_index + delta >= count) {
      con->history_index = count;
      
      con->input_anchor = 0;
      con->input_pos = con->input_length;
      delete_selection_no_update(con);
      update_output(con);
    }
    
    return;
  }
  
  con->history_index+= delta;
  
  con->input_anchor = 0;
  con->input_pos = con->input_length;
  delete_selection_no_update(con);
  insert_input_text(con, 0, hist_text, hist_text_length);
  if(delta > 0) {
    con->input_anchor = 0;
    con->input_pos = 0;
  }
  update_output(con);
}

static void handle_key_down(struct console_input_t *con, const KEY_EVENT_RECORD *er) {
  assert(con != NULL);
  
  switch(er->wVirtualKeyCode) {
    case VK_RETURN:
      if(con->multiline_mode) {
        if(con->input_pos == con->input_length && con->input_pos > 0 && con->input_text[con->input_pos - 1] == L'\n') {
          con->stop = 1;
          con->input_length -= 1;
        }
        else {
          delete_selection_no_update(con);
          insert_input_char(con, con->input_pos, L'\n');
          update_output(con);
        }
      }
      else {
        con->stop = 1;
        con->input_pos = con->input_anchor = con->input_length;
        if(scroll_screen_if_needed(con))
          update_output(con);
        set_output_cursor_position(con);
      }
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
      if(!console_scroll_key(con->output_handle, er)) {
        navigate_history(con, -1);
      }
      return;
    
    case VK_DOWN:
      if(!console_scroll_key(con->output_handle, er)) {
        navigate_history(con, +1);
      }
      return;
    
    case VK_PRIOR:
    case VK_NEXT:
      console_scroll_key(con->output_handle, er);
      return;
      
    case VK_ESCAPE:
      con->input_anchor = 0;
      con->input_pos = con->input_length;
      con->history_index = console_history_count(con->history);
      delete_selection_no_update(con);
      update_output(con);
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
      delete_selection_no_update(con);
      insert_input_char(con, con->input_pos, L'\t');
      update_output(con);
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
  
  if(er->bKeyDown)
    handle_key_down(con, er);
}

static void handle_lbutton_down(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  int i;
  
  assert(con != NULL);
  assert(er != NULL);
  
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

static void handle_window_buffer_size_event(struct console_input_t *con, const WINDOW_BUFFER_SIZE_RECORD *er) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(con != NULL);
  assert(er != NULL);
  
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
    
  con->console_size = csbi.dwSize;
  
  if(csbi.dwCursorPosition.Y != con->last_cursor_pos.Y || con->last_cursor_pos.X != con->last_cursor_pos.X) {
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
  
  hyperlink_system_end_input();
  update_output(con);
  hyperlink_system_start_input(con->console_size.X, con->input_line_coord_y);
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
    
  con->input_text[con->input_length] = L'\0';
  con->input_pos = con->input_anchor = con->input_length;
  con->use_position_dependent_coloring = FALSE;
  update_output(con);
}

static BOOL input_loop(struct console_input_t *con) {
  DWORD old_mode;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  if(!GetConsoleMode(con->input_handle, &old_mode)) {
    con->error = "GetConsoleMode";
    return FALSE;
  }
  
  if(!SetConsoleMode(con->input_handle, ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS)) {
    con->error = "SetConsoleMode";
    return FALSE;
  }
  
  hyperlink_system_start_input(con->console_size.X, con->input_line_coord_y);
  
  // There might be some links in the prompt, which just changed their color.
  read_prompt(con, con->prompt_size);
  
  while(!con->stop && !con->error) {
    INPUT_RECORD event;
    DWORD num_read;
    
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
      
      filter = allocate_memory((length + 1) * sizeof(wchar_t));
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
        
      if(console_handle_mark_mode(con->input_handle, con->output_handle, &event, FALSE))
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
        
        if(console_handle_mark_mode(con->input_handle, con->output_handle, &event, TRUE))
          break;
      }
      else
        break;
    }
  }
  
  finish_input(con);
  
  hyperlink_system_end_input();
  
  if(!WriteConsoleA(con->output_handle, "\n", 1, NULL, NULL))
    con->error = "WriteConsoleA";
    
  SetConsoleMode(con->input_handle, old_mode);
  return !con->error;
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

static wchar_t *read_file(FILE *file, BOOL multiline_mode) {
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
      if(multiline_mode) {
        if(str_len > 1 && str[str_len - 2] == L'\n') {
          str[str_len - 2] = L'\0';
          return str;
        }
      }
      else {
        str[str_len - 1] = L'\0';
        return str;
      }
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
  
  if(!init_console(con)) {
    BOOL multiline_mode = FALSE;
  
    if(HAVE_SETTINGS(settings, flags)) {
      multiline_mode = (settings->flags & HYPER_CONSOLE_FLAGS_MULTILINE) != 0;
    }
    
    /* settings->default_input is ignored */
    fflush(stdout);
    return read_file(stdin, multiline_mode);
  }
  
  if(HAVE_SETTINGS(settings, flags)) {
    con->multiline_mode = (settings->flags & HYPER_CONSOLE_FLAGS_MULTILINE) != 0;
  }
  
  if(HAVE_SETTINGS(settings, history)) {
    con->history = settings->history;
    con->history_index = console_history_count(con->history);
  }
  
  init_buffer(con);
  
  if(HAVE_SETTINGS(settings, default_input) && settings->default_input) {
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
    console_history_add(con->history, con->input_text, con->input_length);
    
    con->input_text = NULL;
    con->input_capacity = 0;
    free_console(con);
    return result;
  }
  
  free_console(con);
  return NULL;
}

BOOL is_handling_input(void) {
  return get_current_input() != NULL;
}

BOOL stop_current_input(BOOL do_abort, const wchar_t *opt_replace_input) {
  struct console_input_t *con;
  
  con = get_current_input();
  if(con == NULL || con->error)
    return FALSE;
    
  if(opt_replace_input) {
    delete_input_text(con, 0, con->input_length);
    insert_input_text(con, 0, opt_replace_input, -1);
    //update_output(con);
  }
  
  con->stop = TRUE;
  con->ignore_input_when_stopped = do_abort;
  return TRUE;
}