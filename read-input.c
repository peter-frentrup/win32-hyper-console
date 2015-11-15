#include "read-input.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>


#ifndef MOUSE_HWHEELED
#  define MOUSE_HWHEELED 0x0008
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
  
  int *input_to_output_positions; // [input_length + 1]
  int input_to_output_capacity;
  
  CHAR_INFO *prompt;
  int prompt_size;
  
  CHAR_INFO *output_buffer; // [output_size]
  int output_capacity;
  int output_size;
  
  int *output_to_input_positions; // [output_size]
  int output_to_input_capacity;
  
  const char *error;
  int dirty_lines;
  unsigned use_position_dependent_coloring;
  unsigned have_colored_fences: 1;
  unsigned multiline_mode: 1;
  unsigned stop: 1;
};

static BOOL init_console(struct console_input_t *con);
static BOOL init_buffer(struct console_input_t *con);
static void init_colors(struct console_input_t *con);
static BOOL read_prompt(struct console_input_t *con, int length);
static void free_console(struct console_input_t *con);
static BOOL resize_array(void **arr, int *capacity, int item_size, int newsize);
static int get_output_position_from_input_position(struct console_input_t *con, int i);
static int get_input_position_from_output_position(struct console_input_t *con, int o);
static int get_input_position_from_screen_position(struct console_input_t *con, COORD pos, BOOL nearest);
static BOOL resize_output_buffer(struct console_input_t *con, int size);
static BOOL fill_output_buffer(struct console_input_t *con);
static BOOL expand_glyphs(struct console_input_t *con);
static wchar_t get_opposite_fence(wchar_t ch);
static int find_matching_fence(struct console_input_t *con, int pos);
static BOOL colorize_matching_fences(struct console_input_t *con);
static void highlight_selection(struct console_input_t *con);
static BOOL extend_output_buffer_to_full_lines(struct console_input_t *con);
static BOOL scroll_screen_if_needed(struct console_input_t *con);
static BOOL write_output_buffer_lines(struct console_input_t *con);
static BOOL set_output_cursor_position(struct console_input_t *con);
static BOOL resize_input_text(struct console_input_t *con, int length);
static BOOL insert_input_text(struct console_input_t *con, int pos, wchar_t *str, int length);
static BOOL insert_input_char(struct console_input_t *con, int pos, wchar_t ch);
static BOOL delete_input_text(struct console_input_t *con, int pos, int length);

void free_memory(void *data) {
  free(data);
}

static BOOL init_console(struct console_input_t *con) {
  assert(con != NULL);
  
  memset(con, 0, sizeof(struct console_input_t));
  
  con->input_handle = GetStdHandle(STD_INPUT_HANDLE);
  con->output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  
  if(con->input_handle == INVALID_HANDLE_VALUE) {
    con->error = "GetStdHandle(STD_INPUT_HANDLE)";
    return 0;
  }
  
  if(con->output_handle == INVALID_HANDLE_VALUE) {
    con->error = "GetStdHandle(STD_OUTPUT_HANDLE)";
    return 0;
  }
  
  con->input_capacity = 256;
  con->input_text = malloc(sizeof(wchar_t) * con->input_capacity);
  if(!con->input_text) {
    con->input_capacity = 0;
    con->error = "malloc";
    return 0;
  }
  
  con->input_text[0] = L'\0';
  con->use_position_dependent_coloring = TRUE;
  
  return 1;
}

static BOOL init_buffer(struct console_input_t *con) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(con != NULL);
  
  if(con->error)
    return 0;
    
  memset(&csbi, 0, sizeof(csbi));
  if(!GetConsoleScreenBufferInfo(con->output_handle, &csbi)) {
    con->error = "GetConsoleScreenBufferInfo";
    return 0;
  }
  
  con->console_size = csbi.dwSize;
  con->attr_default = csbi.wAttributes;
  init_colors(con);
  
  con->input_line_coord_y = csbi.dwCursorPosition.Y;
  
  if(con->console_size.X < 1 || con->console_size.Y < 1) {
    con->error = "GetConsoleScreenBufferInfo size";
    return 0;
  }
  
  return read_prompt(con, csbi.dwCursorPosition.X);
}

static void init_colors(struct console_input_t *con) {
  assert(con != NULL);
  if(con->error)
    return;
    
  con->attr_fences = con->attr_default;
  con->attr_missing_fence = con->attr_default;
  
  if((con->attr_default & 0xFF) == 0x07) // light gray on black
    con->attr_fences = 0x0F; // white on black;
  else
    con->attr_fences = 0x09; // light blue on black;
    
    
  con->attr_missing_fence = 0xCF; // white on light red
  
}

static BOOL read_prompt(struct console_input_t *con, int length) {
  COORD bufsize;
  COORD bufpos;
  SMALL_RECT region;
  
  assert(con != NULL);
  if(con->error)
    return 0;
    
  free_memory(con->prompt);
  con->prompt = NULL;
  con->prompt_size = 0;
  
  if(length <= 0)
    return 1;
    
  con->prompt_size = length;
  con->prompt = malloc(sizeof(con->prompt[0]) * con->prompt_size);
  if(!con->prompt) {
    con->prompt_size = 0;
    con->error = "malloc";
    return 0;
  }
  
  bufsize.X = con->prompt_size;
  bufsize.Y = 1;
  bufpos.X = 0;
  bufpos.Y = 0;
  region.Left = 0;
  region.Top = con->input_line_coord_y;
  region.Right = con->prompt_size;
  region.Bottom = region.Top;
  if(!ReadConsoleOutputW(con->output_handle, con->prompt, bufsize, bufpos, &region)) {
    free_memory(con->prompt);
    con->prompt = NULL;
    con->prompt_size = 0;
    con->error = "ReadConsoleOutputW";
    return 0;
  }
  
  return 1;
}

static void free_console(struct console_input_t *con) {
  assert(con != NULL);
  
  free_memory(con->input_text);
  free_memory(con->prompt);
  free_memory(con->output_buffer);
  free_memory(con->input_to_output_positions);
  free_memory(con->output_to_input_positions);
  memset(con, 0, sizeof(struct console_input_t));
}

static BOOL resize_array(void **arr, int *capacity, int item_size, int newsize) {
  int newcap;
  
  assert(arr != NULL);
  assert(capacity != NULL);
  assert(item_size > 0);
  
  if(newsize < 0 || newsize > (1 << 30) / item_size)
    return 0;
    
  if(newsize <= *capacity)
    return 1;
    
  newcap = 2 * (*capacity);
  if(newcap <= 0)
    newcap = 1;
  while(newcap < newsize)
    newcap *= 2;
    
  *arr = realloc(*arr, newcap * item_size);
  if(!*arr) {
    *capacity = 0;
    return 0;
  }
  
  *capacity = newcap;
  return 1;
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
  
  return get_input_position_from_output_position(con, index);
}

static BOOL resize_output_buffer(struct console_input_t *con, int size) {
  assert(con != NULL);
  
  if(con->error)
    return 0;
    
  if(!resize_array(
      (void**)&con->output_buffer,
      &con->output_capacity,
      sizeof(con->output_buffer[0]),
      size))
  {
    con->error = "resize_array";
    con->output_size = 0;
    return 0;
  }
  
  if(!resize_array(
      (void**)&con->output_to_input_positions,
      &con->output_to_input_capacity,
      sizeof(con->output_to_input_positions[0]),
      size))
  {
    con->error = "resize_array";
    con->output_size = 0;
    return 0;
  }
  
  con->output_size = size;
  return 1;
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
    return 0;
    
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
  
  return 1;
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

static wchar_t get_opposite_fence(wchar_t ch) {
  switch(ch) {
    case L'(':
      return L')';
      
    case L'[':
      return L']';
      
    case L'{':
      return L'}';
      
    case L')':
      return L'(';
      
    case L']':
      return L'[';
      
    case L'}':
      return L'{';
      
    default:
      return L'\0';
  }
}

static int find_matching_fence(struct console_input_t *con, int pos) {
  int direction;
  wchar_t fence;
  wchar_t other_fence;
  int depth;
  
  assert(con != NULL);
  if(con->error)
    return -1;
    
  assert(pos >= 0);
  assert(pos <= con->input_length);
  
  fence = con->input_text[pos];
  other_fence = get_opposite_fence(fence);
  if(!other_fence)
    return -1;
    
  switch(fence) {
    case L'(':
    case L'[':
    case L'{':
      direction = +1;
      break;
      
    case L')':
    case L']':
    case L'}':
      direction = -1;
      break;
      
    default:
      return -1;
  }
  
  depth = 0;
  for(; pos >= 0 && pos < con->input_length; pos += direction) {
    if(con->input_text[pos] == fence) {
      ++depth;
      continue;
    }
    
    if(con->input_text[pos] == other_fence) {
      if(--depth == 0)
        return pos;
        
      continue;
    }
  }
  
  return -1;
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
    
    other_pos = find_matching_fence(con, pos);
    if(other_pos >= 0) {
      other_buf_pos = get_output_position_from_input_position(con, other_pos);
      
      con->output_buffer[buf_pos].Attributes = con->attr_fences;
      con->output_buffer[other_buf_pos].Attributes = con->attr_fences;
      
      con->have_colored_fences = TRUE;
      return TRUE;
    }
    
    if(get_opposite_fence(con->input_text[pos])) {
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
    return 0;
    
  console_width = con->console_size.X;
  assert(console_width > 0);
  
  old_size = con->output_size;
  if(old_size % console_width == 0)
    return 1;
    
  resize_output_buffer(con, (1 + old_size / console_width) * console_width);
  if(con->error)
    return 0;
    
  space.Attributes = con->attr_default;
  space.Char.UnicodeChar = L' ';
  for(i = old_size; i < con->output_size; ++i) {
    con->output_buffer[i] = space;
  }
  for(i = old_size; i < con->output_size; ++i) {
    con->output_to_input_positions[i] = con->input_length;
  }
  
  return 1;
}

static BOOL scroll_screen_if_needed(struct console_input_t *con) {
  int last_pos;
  int cur_pos;
  int last_line;
  int cur_line;
  
  BOOL did_scroll = 0;
  
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
    scroll_rect.Top = scroll_lines;
    scroll_rect.Bottom = con->console_size.X - 1;
    
    dst.X = 0;
    dst.Y = 0;
    
    space.Attributes = con->attr_default;
    space.Char.UnicodeChar = L' ';
    
    if(!ScrollConsoleScreenBufferW(con->output_handle, &scroll_rect, NULL, dst, &space)) {
      con->error = "ScrollConsoleScreenBufferW";
      return did_scroll;
    }
    
    con->input_line_coord_y -= scroll_lines;
    did_scroll = 1;
  }
  
  cur_pos = get_output_position_from_input_position(con, con->input_pos);
  if(cur_pos > 0)
    cur_line = (cur_pos - 1) / con->console_size.X;
  else
    cur_line = 0;
    
  if(con->input_line_coord_y + cur_line < 0) {
    con->input_line_coord_y = - cur_line;
    con->dirty_lines = con->console_size.Y;
    did_scroll = 1;
  }
  
  if(con->input_line_coord_y < 0 && last_line <= con->console_size.Y) {
    con->input_line_coord_y = 0;
    did_scroll = 1;
  }
  
  return did_scroll;
}

// Only writes full lines. Does not scroll buffer. Does not update cursor.
static BOOL write_output_buffer_lines(struct console_input_t *con) {
  COORD bufsize;
  COORD bufpos;
  SMALL_RECT region;
  int console_width;
  int lines;
  
  assert(con != NULL);
  if(con->error)
    return 0;
    
  console_width = con->console_size.X;
  assert(console_width > 0);
  
  lines = con->output_size / console_width;
  if(lines < con->dirty_lines) {
    CHAR_INFO *empty = malloc((con->dirty_lines - lines) * console_width * sizeof(CHAR_INFO));
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
        free_memory(empty);
        return 0;
      }
      
      free_memory(empty);
    }
  }
  con->dirty_lines = lines;
  
  bufsize.X = console_width;
  bufsize.Y = lines;
  bufpos.X = 0;
  bufpos.Y = 0;
  region.Left = 0;
  region.Top = con->input_line_coord_y;
  region.Right = console_width - 1;
  region.Bottom = region.Top + lines - 1;
  
  if(bufsize.Y > 0) {
    if(!WriteConsoleOutputW(con->output_handle, con->output_buffer, bufsize, bufpos, &region)) {
      con->error = "WriteConsoleOutputW";
      return 0;
    }
  }
  
  return 1;
}

static BOOL set_output_cursor_position(struct console_input_t *con) {
  COORD pos;
  int console_width;
  int output_pos;
  
  assert(con != NULL);
  if(con->error)
    return 0;
    
  console_width = con->console_size.X;
  assert(console_width > 0);
  
  assert(con->input_pos >= 0);
  assert(con->input_pos <= con->input_length);
  
  output_pos = get_output_position_from_input_position(con, con->input_pos);
  
  pos.X = output_pos % console_width;
  pos.Y = output_pos / console_width + con->input_line_coord_y;
  
  if(!SetConsoleCursorPosition(con->output_handle, pos)) {
    //  con->error = "SetConsoleCursorPosition";
    //  return 0;
  }
  
  return 1;
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

static BOOL resize_input_text(struct console_input_t *con, int length) {
  assert(con != NULL);
  if(con->error)
    return 0;
    
  if(!resize_array(
      (void**)&con->input_text,
      &con->input_capacity,
      sizeof(con->input_text[0]),
      length + 1))
  {
    con->error = "resize_array";
    con->input_length = 0;
    return 0;
  }
  
  if(!resize_array(
      (void**)&con->input_to_output_positions,
      &con->input_to_output_capacity,
      sizeof(con->input_to_output_positions[0]),
      length + 1))
  {
    con->error = "resize_array";
    con->input_length = 0;
    return 0;
  }
  
  con->input_text[length] = L'\0';
  con->input_length = length;
  return 1;
}

static BOOL insert_input_text(struct console_input_t *con, int pos, wchar_t *str, int length) {
  assert(con != NULL);
  if(con->error)
    return 0;
    
  assert(str != NULL || length == 0);
  
  assert(pos >= 0);
  assert(pos <= con->input_length);
  
  if(length < 0)
    length = (int)wcslen(str);
    
  resize_input_text(con, con->input_length + length);
  if(con->error)
    return 0;
    
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
    
  return 1;
}

static BOOL insert_input_char(struct console_input_t *con, int pos, wchar_t ch) {
  return insert_input_text(con, pos, &ch, 1);
}

static BOOL delete_input_text(struct console_input_t *con, int pos, int length) {
  assert(con != NULL);
  if(con->error)
    return 0;
    
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
    return 0;
    
  if(pos + length <= con->input_pos)
    con->input_pos -= length;
  else if(pos < con->input_pos)
    con->input_pos = pos;
    
  if(pos + length <= con->input_anchor)
    con->input_anchor -= length;
  else if(pos < con->input_anchor)
    con->input_anchor = pos;
    
  return 1;
}

static void reselect(struct console_input_t *con, int new_pos, int new_anchor) {
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

static int get_word_start(struct console_input_t *con, int pos) {
  assert(con != NULL);
  if(con->error)
    return 0;
    
  assert(pos >= 0);
  assert(pos <= con->input_length);
  
  if(pos > 0) {
    if(iswalpha(con->input_text[pos])) {
      while(pos > 0 && iswalpha(con->input_text[pos - 1]))
        --pos;
    }
    else if(iswdigit(con->input_text[pos])) {
      while(pos > 0 && iswdigit(con->input_text[pos - 1]))
        --pos;
    }
    else if(iswspace(con->input_text[pos])) {
      while(pos > 0 && iswspace(con->input_text[pos - 1]))
        --pos;
    }
  }
  
  return pos;
}

static int get_word_end(struct console_input_t *con, int pos) {
  assert(con != NULL);
  if(con->error)
    return 0;
    
  assert(pos >= 0);
  assert(pos <= con->input_length);
  
  if(pos < con->input_length) {
    if(iswalpha(con->input_text[pos])) {
      while(pos < con->input_length && iswalpha(con->input_text[pos + 1]))
        ++pos;
    }
    else if(iswdigit(con->input_text[pos])) {
      while(pos < con->input_length && iswdigit(con->input_text[pos + 1]))
        ++pos;
    }
    else if(iswspace(con->input_text[pos])) {
      while(pos < con->input_length && iswspace(con->input_text[pos + 1]))
        ++pos;
    }
    
    return pos + 1;
  }
  
  return pos;
}

static void move_left(struct console_input_t *con, BOOL fix_anchor, BOOL jump_word) {
  int new_pos;
  
  assert(con != NULL);
  if(con->error)
    return;
    
  new_pos = con->input_pos - 1;
  if(jump_word)
    new_pos = get_word_start(con, new_pos);
    
  if(fix_anchor) {
    reselect(con, new_pos, con->input_anchor);
  }
  else {
    if(con->input_pos < con->input_anchor)
      reselect(con, con->input_pos, con->input_pos);
    else if(con->input_anchor < con->input_pos)
      reselect(con, con->input_anchor, con->input_anchor);
    else
      reselect(con, new_pos, new_pos);
  }
}

static void move_right(struct console_input_t *con, BOOL fix_anchor, BOOL jump_word) {
  int new_pos;
  
  assert(con != NULL);
  if(con->error)
    return;
    
  new_pos = con->input_pos;
  if(jump_word)
    new_pos = get_word_end(con, con->input_pos);
  else
    new_pos = con->input_pos + 1;
    
  if(fix_anchor) {
    reselect(con, new_pos, con->input_anchor);
  }
  else {
    if(con->input_pos > con->input_anchor)
      reselect(con, con->input_pos, con->input_pos);
    else if(con->input_anchor > con->input_pos)
      reselect(con, con->input_anchor, con->input_anchor);
    else
      reselect(con, new_pos, new_pos);
  }
}

static void move_home(struct console_input_t *con, BOOL fix_anchor) {
  if(fix_anchor)
    reselect(con, 0, con->input_anchor);
  else
    reselect(con, 0, 0);
}

static void move_end(struct console_input_t *con, BOOL fix_anchor) {
  if(fix_anchor)
    reselect(con, con->input_length, con->input_anchor);
  else
    reselect(con, con->input_length, con->input_length);
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
    return FALSE;
    
  if(con->input_anchor < con->input_pos) {
    start = con->input_anchor;
    end = con->input_pos;
  }
  else if(con->input_pos < con->input_anchor) {
    start = con->input_pos;
    end = con->input_anchor;
  }
  else
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

struct send_input_t {
  struct console_input_t *con;
  INPUT_RECORD input_records[128];
  DWORD counter;
};

static BOOL flush_input(struct send_input_t *context) {
  DWORD written;
  
  assert(context != NULL);
  assert(context->con != NULL);
  if(context->con->error)
    return FALSE;
    
  assert(context->counter >= 0);
  assert(context->counter <= sizeof(context->input_records) / sizeof(INPUT_RECORD));
  
  if(!WriteConsoleInputW(context->con->input_handle, context->input_records, context->counter, &written)) {
    context->con->error = "WriteConsoleInputW";
    context->counter = 0;
    return FALSE;
  }
  
  context->counter = 0;
  return TRUE;
}

static BOOL send_input(struct send_input_t *context, wchar_t ch) {
  SHORT vk_mode;
  
  assert(context != NULL);
  assert(context->counter >= 0);
  
  if(context->counter + 2 > sizeof(context->input_records) / sizeof(INPUT_RECORD)) {
    if(!flush_input(context))
      return FALSE;
  }
  
  //BOOL shift;
  //BOOL ctrl;
  //BOOL alt;
  
  //if(ch == L'\r' && copy_data[1] == L'\n')
  //  ++copy_data;
  //else if(ch == L'\n')
  //  ch = L'\r';
  //else if(ch == L'\t')
  //  ch = L' ';
  
  vk_mode = VkKeyScanW(ch);
  //shift = (vk_mode & 0x0100) != 0;
  //ctrl  = (vk_mode & 0x0200) != 0;
  //alt   = (vk_mode & 0x0400) != 0;
  
  context->input_records[context->counter].EventType = KEY_EVENT;
  context->input_records[context->counter].Event.KeyEvent.bKeyDown = TRUE;
  context->input_records[context->counter].Event.KeyEvent.dwControlKeyState = 0;// (shift ? SHIFT_PRESSED : 0) | (ctrl ? LEFT_CTRL_PRESSED : 0) | (alt ? LEFT_ALT_PRESSED : 0);
  context->input_records[context->counter].Event.KeyEvent.uChar.UnicodeChar = ch;
  context->input_records[context->counter].Event.KeyEvent.wRepeatCount = 1;
  context->input_records[context->counter].Event.KeyEvent.wVirtualKeyCode = LOBYTE(vk_mode);
  context->input_records[context->counter].Event.KeyEvent.wVirtualScanCode = MapVirtualKeyW(LOBYTE(vk_mode), MAPVK_VK_TO_VSC);
  context->counter++;
  
  context->input_records[context->counter] = context->input_records[context->counter - 1];
  context->input_records[context->counter].Event.KeyEvent.bKeyDown = FALSE;
  context->counter++;
  
  return TRUE;
}

static void paste_from_clipboard(struct console_input_t *con) {
  HGLOBAL copy_handle;
  wchar_t *copy_data;
  
  assert(con != NULL);
  if(con->error)
    return FALSE;
    
  if(!IsClipboardFormatAvailable(CF_UNICODETEXT))
    return;
    
  if(!OpenClipboard(NULL))
    return;
    
  copy_handle = GetClipboardData(CF_UNICODETEXT);
  if(copy_handle) {
    copy_data = GlobalLock(copy_handle);
    if(copy_data) {
      struct send_input_t context[1];
      
      context->con = con;
      context->counter = 0;
      
      for(; *copy_data && !con->error; ++copy_data) {
        wchar_t ch = *copy_data;
        
        if(ch == L'\r' && copy_data[1] == L'\n') {
          send_input(context, L'\r');
          ++copy_data;
          continue;
        }
        
        if(ch == L'\n') {
          send_input(context, L'\r');
          continue;
        }
        
        if(ch == L'\t') {
          send_input(context, L' ');
          send_input(context, L' ');
          send_input(context, L' ');
          send_input(context, L' ');
          continue;
        }
        
        send_input(context, ch);
      }
      
      flush_input(context);
      
      //int i = 0;
      //while(/*copy_data[i] != L'\n' && copy_data[i] != L'\r' &&*/ copy_data[i] != L'\0')
      //  ++i;
      //
      //delete_selection_no_update(con);
      //insert_input_text(con, con->input_pos, copy_data, i);
      //update_output(con);
    }
    GlobalUnlock(copy_handle);
  }
  
  CloseClipboard();
}

static void handle_key_down(struct console_input_t *con, const KEY_EVENT_RECORD *er) {
  assert(con != NULL);
  
  switch(er->wVirtualKeyCode) {
    case VK_RETURN:
      if(con->multiline_mode) {
        if(con->input_pos == con->input_length && con->input_pos > 0 && con->input_text[con->input_pos - 1] == L'\n') {
          con->stop = 1;
          con->input_length -= 1;
          return;
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
        return;
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
      
    case 'A': // Ctrl+A
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        reselect(con, con->input_length, 0);
        return;
      }
      break;
      
    case VK_INSERT:
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        copy_to_clipboard(con);
        return;
      }
      if(er->dwControlKeyState & SHIFT_PRESSED) {
        paste_from_clipboard(con);
        return;
      }
      break;
      
    case 'C': // Ctrl+C
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        if(con->input_anchor != con->input_pos) {
          copy_to_clipboard(con);
          reselect(con, con->input_pos, con->input_pos);
          return;
        }
        else { // empty selection -> abort edit
          con->stop = TRUE;
          return;
        }
      }
      break;
      
    case 'V': // Ctrl+V
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        paste_from_clipboard(con);
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
      
    case VK_TAB:
      delete_selection_no_update(con);
      insert_input_char(con, con->input_pos, L'\t');
      update_output(con);
      return;
  }
  
  if((unsigned)er->uChar.UnicodeChar >= (unsigned)L' ') {
    delete_selection_no_update(con);
    insert_input_char(con, con->input_pos, er->uChar.UnicodeChar);
    update_output(con);
  }
}

static void handle_key_event(struct console_input_t *con, const KEY_EVENT_RECORD *er) {
  assert(con != NULL);
  
  if(er->bKeyDown)
    handle_key_down(con, er);
}

static void handle_mouse_wheel(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  short scroll_delta = (short)HIWORD(er->dwButtonState);
  int scroll_lines = 3;
  
  SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &scroll_lines, 0);
  scroll_lines = (scroll_lines * scroll_delta) / WHEEL_DELTA;
  
  memset(&csbi, 0, sizeof(csbi));
  if(GetConsoleScreenBufferInfo(con->output_handle, &csbi)) {
    csbi.srWindow.Top -= scroll_lines;
    csbi.srWindow.Bottom -= scroll_lines;
    
    if(csbi.srWindow.Top < 0) {
      csbi.srWindow.Bottom += (0 - csbi.srWindow.Top);
      csbi.srWindow.Top = 0;
    }
    else if(csbi.srWindow.Bottom >= csbi.dwSize.Y) {
      csbi.srWindow.Top += (csbi.dwSize.Y - 1 - csbi.srWindow.Bottom);
      csbi.srWindow.Bottom = csbi.dwSize.Y - 1;
    }
    
    SetConsoleWindowInfo(con->output_handle, TRUE, &csbi.srWindow);
  }
}

static void handle_lbutton_down(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  int i = get_input_position_from_screen_position(con, er->dwMousePosition, FALSE);
  
  if(i >= 0) {
    BOOL need_redraw = (con->input_anchor != con->input_pos);
    con->input_pos = i;
    con->input_anchor = i;
    if(need_redraw || scroll_screen_if_needed(con))
      update_output(con);
    else
      set_output_cursor_position(con);
    return;
  }
}

static void handle_lbutton_move(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  int i = get_input_position_from_screen_position(con, er->dwMousePosition, TRUE);
  
  if(i >= 0 && i != con->input_pos) {
    con->input_pos = i;
    update_output(con);
    return;
  }
}

static void handle_lbutton_double_click(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  int i = get_input_position_from_screen_position(con, er->dwMousePosition, TRUE);
  
  if(i >= 0) {
    int s = get_word_start(con, i);
    int e = get_word_end(con, i);
    
    con->input_anchor = s;
    con->input_pos = e;
    
    update_output(con);
    return;
  }
}

static void handle_mouse_event(struct console_input_t *con, const MOUSE_EVENT_RECORD *er) {
  assert(con != NULL);
  
  switch(er->dwEventFlags) {
    case 0:
      // button press/release
      if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
        handle_lbutton_down(con, er);
        return;
      }
      break;
      
    case DOUBLE_CLICK:
      if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
        handle_lbutton_double_click(con, er);
        return;
      }
      break;
      
    case MOUSE_MOVED:
      if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
        handle_lbutton_move(con, er);
        return;
      }
      break;
      
    case MOUSE_HWHEELED:
      break;
      
    case MOUSE_WHEELED:
      handle_mouse_wheel(con, er);
      break;
  }
}

static void handle_window_buffer_size_event(struct console_input_t *con, const WINDOW_BUFFER_SIZE_RECORD *er) {

}

static void handle_focus_event(struct console_input_t *con, const FOCUS_EVENT_RECORD *er) {

}

static void handle_menu_event(struct console_input_t *con, const MENU_EVENT_RECORD *er) {

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
    return 0;
    
  if(!GetConsoleMode(con->input_handle, &old_mode)) {
    con->error = "GetConsoleMode";
    return 0;
  }
  
  if(!SetConsoleMode(con->input_handle, ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT)) {
    con->error = "SetConsoleMode";
    return 0;
  }
  
  while(!con->stop && !con->error) {
    INPUT_RECORD input_records[1];
    DWORD num_read;
    int i;
    
    if(!ReadConsoleInputW(
        con->input_handle,
        input_records,
        sizeof(input_records) / sizeof(input_records[0]),
        &num_read))
    {
      con->error = "ReadConsoleInputW";
      break;
    }
    
    for(i = 0; i < num_read; ++i) {
      switch(input_records[i].EventType) {
        case KEY_EVENT:
          handle_key_event(con, &input_records[i].Event.KeyEvent);
          break;
          
        case MOUSE_EVENT:
          handle_mouse_event(con, &input_records[i].Event.MouseEvent);
          break;
          
        case WINDOW_BUFFER_SIZE_EVENT: // scrn buf. resizing
          handle_window_buffer_size_event(con, &input_records[i].Event.WindowBufferSizeEvent);
          break;
          
        case FOCUS_EVENT:
          handle_focus_event(con, &input_records[i].Event.FocusEvent);
          break;
          
        case MENU_EVENT:   // disregard menu events
          handle_menu_event(con, &input_records[i].Event.MenuEvent);
          break;
      }
    }
  }
  
  finish_input(con);
  
  if(!WriteConsoleA(con->output_handle, "\n", 1, NULL, NULL))
    con->error = "WriteConsoleA";
    
  SetConsoleMode(con->input_handle, old_mode);
  return !con->error;
}

static void print_error() {
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

wchar_t *read_input(BOOL multiline_mode) {
  struct console_input_t con[1];
  
  if(!init_console(con))
    return NULL;
    
  con->multiline_mode = multiline_mode;
  init_buffer(con);
  
  insert_input_text(con, 0, L"default", -1);
  con->input_anchor = 0;
  update_output(con);
  
  input_loop(con);
  
  if(!con->error) {
    wchar_t *result = con->input_text;
    con->input_text = NULL;
    con->input_capacity = 0;
    free_console(con);
    return result;
  }
  
  fprintf(stderr, "input failed. ");
  fprintf(stderr, "%s ", con->error);
  print_error();
  fprintf(stderr, "\n");
  
  free_console(con);
  return NULL;
}

