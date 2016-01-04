#include "mark-mode.h"
#include "console-buffer-io.h"
#include "debug.h"
#include "memory-util.h"

#include <assert.h>
#include <stdlib.h>
#include <strsafe.h>


#ifndef MOUSE_HWHEELED
#  define MOUSE_HWHEELED 0x0008
#endif


#define MIN(A, B)  ((A) < (B) ? (A) : (B))
#define MAX(A, B)  ((A) > (B) ? (A) : (B))


struct console_mark_t {
  HANDLE input_handle;
  HANDLE output_handle;
  
  COORD console_size;
  
  COORD anchor;
  COORD pos;
  
  COORD                original_pos;
  wchar_t             *oritinal_title;
  CONSOLE_CURSOR_INFO  original_cursor;
  
  unsigned active: 1;
  unsigned stop: 1;
  unsigned continue_on_empty_click: 1;
  unsigned was_block_mode: 1;
  unsigned block_mode: 1;
};

static BOOL have_selected_output(struct console_mark_t *cm);
static void reselect_output(struct console_mark_t *cm, COORD pos, COORD anchor);

static void move_selection_left(struct console_mark_t *cm, BOOL fix_anchor, BOOL jump_word);
static void move_selection_right(struct console_mark_t *cm, BOOL fix_anchor, BOOL jump_word);
static void move_selection_up(struct console_mark_t *cm, BOOL fix_anchor);
static void move_selection_down(struct console_mark_t *cm, BOOL fix_anchor);

static wchar_t *cat_console_line(struct console_mark_t *cm, wchar_t *buffer, wchar_t *buffer_end, COORD start, int length);
static void copy_output_to_clipboard(struct console_mark_t *cm);

static void start_mark_mode(struct console_mark_t *cm);

static BOOL mark_mode_handle_key_event(struct console_mark_t *cm, KEY_EVENT_RECORD *er);
static BOOL mark_mode_handle_mouse_event(struct console_mark_t *cm, MOUSE_EVENT_RECORD *er);

static BOOL run_mark_mode(struct console_mark_t *cm, INPUT_RECORD *event);
static void finish_mark_mode(struct console_mark_t *cm);


static BOOL have_selected_output(struct console_mark_t *cm) {
  assert(cm != NULL);
  
  return cm->block_mode || cm->anchor.X != cm->pos.X || cm->anchor.Y != cm->pos.Y;
}

static void invert_colors_block(
  HANDLE hConsoleOutput,
  COORD start,
  COORD end
) {
  int y;
  
  for(y = MIN(start.Y, end.Y); y <= MAX(start.Y, end.Y); ++y) {
    COORD from;
    COORD to;
    
    from.X = MIN(start.X, end.X);
    from.Y = y;
    to.X = MAX(start.X, end.X) + 1;
    to.Y = y;
    
    console_reinvert_colors(hConsoleOutput, from, from, from, to);
  }
}

static void reselect_output(struct console_mark_t *cm, COORD pos, COORD anchor) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(cm != NULL);
  
  if(cm->block_mode) {
    if(cm->was_block_mode) {
      invert_colors_block(cm->output_handle, cm->anchor, cm->pos);
    }
    else {
      console_reinvert_colors(cm->output_handle, cm->anchor, cm->pos, anchor, anchor);
    }
    
    invert_colors_block(cm->output_handle, anchor, pos);
  }
  else {
    if(cm->was_block_mode) {
      invert_colors_block(cm->output_handle, cm->anchor, cm->pos);
      cm->pos = cm->anchor;
    }
    
    console_reinvert_colors(cm->output_handle, cm->anchor, cm->pos, anchor, pos);
  }
  
  cm->was_block_mode = cm->block_mode;
  cm->anchor = anchor;
  cm->pos = pos;
  
  
  if(pos.X == cm->console_size.X)
    --pos.X;
    
//  if(pos.Y == cm->console_size.Y) {
//    pos.Y--;
//    pos.X = cm->console_size.X - 1;
//  }

  if(GetConsoleScreenBufferInfo(cm->output_handle, &csbi)) {
    if(csbi.dwCursorPosition.X != pos.X || csbi.dwCursorPosition.Y != pos.Y)
      SetConsoleCursorPosition(cm->output_handle, pos);
  }
}


static void move_selection_left(struct console_mark_t *cm, BOOL fix_anchor, BOOL jump_word) {
  COORD new_pos;
  
  assert(cm != NULL);
  
  new_pos = cm->pos;
  if(new_pos.X > 0) {
    new_pos.X--;
  }
  else if(new_pos.Y > 0) {
    new_pos.Y--;
    new_pos.X = cm->console_size.X - 1;
  }
  
  if(jump_word) {
    COORD dummy;
    if(!console_get_screen_word_start_end(cm->output_handle, new_pos, &new_pos, &dummy))
      return;
  }
  
  reselect_output(cm, new_pos, fix_anchor ? cm->anchor : new_pos);
}

static void move_selection_right(struct console_mark_t *cm, BOOL fix_anchor, BOOL jump_word) {
  COORD new_pos;
  
  assert(cm != NULL);
  
  new_pos = cm->pos;
  
  if(jump_word) {
    COORD dummy;
    if(!console_get_screen_word_start_end(cm->output_handle, new_pos, &dummy, &new_pos))
      return;
  }
  else if(new_pos.X + 1 < cm->console_size.X) {
    new_pos.X++;
  }
  else if(new_pos.Y < cm->console_size.Y) {
    new_pos.Y++;
    new_pos.X = 0;
  }
  
  reselect_output(cm, new_pos, fix_anchor ? cm->anchor : new_pos);
}

static void move_selection_up(struct console_mark_t *cm, BOOL fix_anchor) {
  COORD new_pos;
  
  assert(cm != NULL);
  
  new_pos = cm->pos;
  if(new_pos.Y > 0)
    new_pos.Y--;
  else if(new_pos.X > 0 && !cm->block_mode)
    new_pos.X = 0;
    
  reselect_output(cm, new_pos, fix_anchor ? cm->anchor : new_pos);
}

static void move_selection_down(struct console_mark_t *cm, BOOL fix_anchor) {
  COORD new_pos;
  
  assert(cm != NULL);
  
  new_pos = cm->pos;
  if(new_pos.Y + 1 < cm->console_size.Y)
    new_pos.Y++;
  else if(new_pos.X < cm->console_size.X && !cm->block_mode)
    new_pos.X = cm->console_size.X;
    
  reselect_output(cm, new_pos, fix_anchor ? cm->anchor : new_pos);
}



static wchar_t *cat_console_line(
  struct console_mark_t *cm,
  wchar_t *buffer,
  wchar_t *buffer_end,
  COORD start,
  int length
) {
  DWORD dummy_read_count;
  wchar_t *s;
  
  assert(cm != NULL);
  
  assert(buffer != NULL);
  assert(buffer_end != NULL);
  assert(buffer < buffer_end);
  assert(length + 2 < buffer_end - buffer);
  assert(length >= 0);
  
  if(length == 0) {
    *buffer = L'\0';
    return buffer;
  }
  
  console_read_output_character(
    cm->output_handle,
    buffer,
    length,
    start,
    &dummy_read_count);
    
  s = buffer + length;
  if(s[-1] == L' ') {
    while(s > buffer && s[-1] == L' ')
      --s;
  }
  
  if(s == buffer || s - buffer + start.X + 1 < cm->console_size.X) {
    *s++ = L'\r';
    *s++ = L'\n';
  }
  
  *s = L'\0';
  return s;
}

/** Get the non-block-mode selected text.

  \return The unwrapped text lines. Should be freed with free_memory(). NULL on error.
 */
static wchar_t *get_selection_lines(struct console_mark_t *cm, int *total_length) {
  COORD start;
  COORD end;
  int length;
  int index_a;
  int index_b;
  wchar_t *str;
  int max_length;
  wchar_t *s;
  
  assert(cm != NULL);
  assert(total_length != NULL);
  
  index_a = cm->anchor.Y * cm->console_size.X + cm->anchor.X;
  index_b = cm->pos.Y    * cm->console_size.X + cm->pos.X;
  
  if(index_a < index_b) {
    start.X = cm->anchor.X;
    start.Y = cm->anchor.Y;
    end.X = cm->pos.X;
    end.Y = cm->pos.Y;
    length = index_b - index_a;
  }
  else {
    start.X = cm->pos.X;
    start.Y = cm->pos.Y;
    end.X = cm->anchor.X;
    end.Y = cm->anchor.Y;
    length = index_a - index_b;
  }
  
  max_length = length + 2 * (end.Y - start.Y + 1) + 1;
  str = allocate_memory(sizeof(wchar_t) * max_length);
  if(!str) {
    total_length = 0;
    return NULL;
  }
    
  s = str;
  while(start.Y < end.Y) {
    s = cat_console_line(cm, s, str + max_length, start, cm->console_size.X - start.X);
    
    start.X = 0;
    start.Y++;
  }
  
  s = cat_console_line(cm, s, str + max_length, start, end.X - start.X);
  
  while(str + 2 <= s && s[-2] == L'\r' && s[-1] == L'\n')
    s -= 2;
    
  *s = L'\0';
  
  *total_length = s - str;
  return str;
}

/** Get the block-mode selected text.

  \return The rectangle block of lines, trimmed ad line ends. 
  Should be freed with free_memory(). NULL on error.
 */
static wchar_t *get_selection_block_lines(struct console_mark_t *cm, int *total_length) {
  COORD start;
  COORD end;
  wchar_t *str;
  wchar_t *s;
  int length;
  int line_length;
  COORD pos;
  
  assert(cm != NULL);
  assert(total_length != NULL);
  
  start.Y = MIN(cm->anchor.Y, cm->pos.Y);
  start.X = MIN(cm->anchor.X, cm->pos.X);
  end.Y   = MAX(cm->anchor.Y, cm->pos.Y);
  end.X   = MAX(cm->anchor.X, cm->pos.X);
  
  line_length = end.X - start.X + 1;
  
  length = (end.Y - start.Y + 1) * (line_length + 2);
  str = allocate_memory(sizeof(wchar_t) * length);
  if(!str) {
    *total_length = 0;
    return NULL;
  }
  
  s = str;
  pos.X = start.X;
  for(pos.Y = start.Y;pos.Y <= end.Y;pos.Y++) {
    DWORD dummy_read_count;
    wchar_t *next;
    
    console_read_output_character(
      cm->output_handle,
      s,
      line_length,
      pos,
      &dummy_read_count);
    
    next = s + line_length;
    while(next != s && next[-1] == L' ')
      --next;
    
    s = next;
    *s++ = L'\r';
    *s++ = L'\n';
  }
  
  s-= 2;
  *s = L'\0';
  *total_length = s - str;
  return str;
}

static void copy_output_to_clipboard(struct console_mark_t *cm) {
  wchar_t *str;
  int length;
  
  if(cm->block_mode)
    str = get_selection_block_lines(cm, &length);
  else
    str = get_selection_lines(cm, &length);
    
  if(!str)
    return;
  
  assert(length >= 0);
  
  if(OpenClipboard(NULL)) {
    void *copy_data;
    HGLOBAL copy_handle = GlobalAlloc(GMEM_MOVEABLE, (length + 1) * sizeof(wchar_t));
    
    if(!copy_handle) {
      free_memory(str);
      CloseClipboard();
      return;
    }
    
    copy_data = GlobalLock(copy_handle);
    memcpy(copy_data, str, (length + 1) * sizeof(wchar_t));
    GlobalUnlock(copy_handle);
    
    SetClipboardData(CF_UNICODETEXT, copy_handle);
    
    CloseClipboard();
  }
  
  free_memory(str);
}


static void start_mark_mode(struct console_mark_t *cm) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  CONSOLE_CURSOR_INFO cci;
  
  assert(cm != NULL);
  
  if(GetConsoleScreenBufferInfo(cm->output_handle, &csbi)) {
    reselect_output(cm, csbi.dwCursorPosition, csbi.dwCursorPosition);
    cm->active = TRUE;
  }
  
  if(cm->oritinal_title == NULL) {
    cm->oritinal_title = allocate_memory(sizeof(wchar_t) * 256);
    if(cm->oritinal_title) {
      wchar_t buffer[256 + 20];
      
      GetConsoleTitleW(cm->oritinal_title, 256);
      
      StringCbPrintfW(buffer, sizeof(buffer), L"Mark Mode %s", cm->oritinal_title);
      SetConsoleTitleW(buffer);
    }
    else
      SetConsoleTitleW(L"Mark mode");
  }
  
  cci.bVisible = TRUE;
  cci.dwSize = 75;
  SetConsoleCursorInfo(cm->output_handle, &cci);
}


static BOOL mark_mode_handle_key_event(struct console_mark_t *cm, KEY_EVENT_RECORD *er) {
  assert(cm != NULL);
  assert(er != NULL);
  
  if(er->bKeyDown) {
    switch(er->wVirtualKeyCode) {
      case VK_SHIFT:
      case VK_CONTROL:
      case VK_MENU:
        return cm->active;
    }
    
    if(cm->active) {
      switch(er->wVirtualKeyCode) {
        case VK_ESCAPE:
          cm->stop = TRUE;
          return TRUE;
          
        case VK_LEFT:
          cm->block_mode = 0 != (er->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
          move_selection_left(
            cm,
            er->dwControlKeyState & SHIFT_PRESSED,
            er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
          return TRUE;
          
        case VK_RIGHT:
          cm->block_mode = 0 != (er->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
          move_selection_right(
            cm,
            er->dwControlKeyState & SHIFT_PRESSED,
            er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
          return TRUE;
          
        case VK_UP:
          cm->block_mode = 0 != (er->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
          move_selection_up(
            cm,
            er->dwControlKeyState & SHIFT_PRESSED);
          return TRUE;
          
        case VK_DOWN:
          cm->block_mode = 0 != (er->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
          move_selection_down(
            cm,
            er->dwControlKeyState & SHIFT_PRESSED);
          return TRUE;
          
        case 'A': // Ctrl+A
          if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
            COORD anchor;
            COORD pos;
            
            anchor.X = 0;
            anchor.Y = 0;
            pos.X = 0;
            pos.Y = cm->console_size.Y;
            
            reselect_output(cm, pos, anchor);
            return TRUE;
          }
          break;
          
        case 'C': // Ctrl+C
          if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
            if(have_selected_output(cm))
              copy_output_to_clipboard(cm);
            else
              console_alert(cm->output_handle);
            
            cm->stop = TRUE;
            return TRUE;
          }
          break;
          
        case 'X': // Ctrl+X
          if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
            if(have_selected_output(cm))
              copy_output_to_clipboard(cm);
            
            console_alert(cm->output_handle);
            cm->stop = TRUE;
            return TRUE;
          }
          break;
        
        case VK_BACK:
        case VK_DELETE:
          console_alert(cm->output_handle);
          cm->stop = TRUE;
          return TRUE;
      }
    }
    else {
      switch(er->wVirtualKeyCode) {
        case 'M': // Ctrl+M
          if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
            start_mark_mode(cm);
            cm->continue_on_empty_click = TRUE;
            return TRUE;
          }
          break;
      }
    }
    
    cm->stop = TRUE;
    return FALSE;
  }
  else {
    return cm->active;
  }
}

static BOOL mark_mode_handle_mouse_event(struct console_mark_t *cm, MOUSE_EVENT_RECORD *er) {
  assert(er != NULL);
  
  switch(er->dwEventFlags) {
    case 0: /* mouse down/up */
      debug_printf(L"mark_mode_handle_mouse_event press/release %x %x\n", er->dwButtonState, er->dwControlKeyState);
      if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
        start_mark_mode(cm);
        cm->block_mode = 0 != (er->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
        
        reselect_output(cm, er->dwMousePosition, er->dwMousePosition);
        return TRUE;
      }
      if(cm->active && er->dwButtonState == 0) {
        if(!have_selected_output(cm) && !cm->continue_on_empty_click) {
          cm->stop = TRUE;
          return TRUE;
        }
      }
      return cm->active;
      
    case DOUBLE_CLICK:
      debug_printf(L"mark_mode_handle_mouse_event DOUBLE_CLICK %x\n", er->dwButtonState);
      if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
        COORD start;
        COORD end;
        
        if(console_get_screen_word_start_end(cm->output_handle, er->dwMousePosition, &start, &end)) {
          start_mark_mode(cm);
          reselect_output(cm, end, start);
        }
        
        return TRUE;
      }
      return cm->active;
      
    case MOUSE_MOVED:
      if(cm->active) {
        if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
          reselect_output(cm, er->dwMousePosition, cm->anchor);
        }
      }
      return cm->active;
      
    case MOUSE_HWHEELED:
    case MOUSE_WHEELED:
      if(cm->active) {
        console_scroll_wheel(cm->output_handle, er);
        return TRUE;
      }
      return FALSE;
  }
  
  return FALSE;
}


static BOOL run_mark_mode(struct console_mark_t *cm, INPUT_RECORD *event) {
  assert(cm != NULL);
  assert(event != NULL);
  
  for(;;) {
    DWORD num_read;
    
    switch(event->EventType) {
      case KEY_EVENT:
        if(!mark_mode_handle_key_event(cm, &event->Event.KeyEvent))
          return FALSE;
        break;
        
      case MOUSE_EVENT:
        if(!mark_mode_handle_mouse_event(cm, &event->Event.MouseEvent))
          return FALSE;
        break;
        
      default:
        if(!cm->active)
          return FALSE;
        break;
    }
    
    if(!cm->active || cm->stop)
      return TRUE;
      
    if(!ReadConsoleInputW(cm->input_handle, event, 1, &num_read) || num_read < 1)
      return TRUE;
  };
  
  return FALSE;
}

static void finish_mark_mode(struct console_mark_t *cm) {
  assert(cm != NULL);
  
  cm->block_mode = FALSE;
  reselect_output(cm, cm->original_pos, cm->original_pos);
  
  if(cm->oritinal_title) {
    SetConsoleTitleW(cm->oritinal_title);
    
    free_memory(cm->oritinal_title);
    cm->oritinal_title = NULL;
  }
  
  SetConsoleCursorInfo(cm->output_handle, &cm->original_cursor);
}

BOOL console_handle_mark_mode(HANDLE hConsoleInput, HANDLE hConsoleOutput, INPUT_RECORD *event, BOOL force_mark_mode) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  struct console_mark_t cm;
  BOOL result;
  
  assert(event != NULL);
  
  memset(&cm, 0, sizeof(cm));
  cm.input_handle = hConsoleInput;
  cm.output_handle = hConsoleOutput;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi))
    return FALSE;
    
  if(!GetConsoleCursorInfo(hConsoleOutput, &cm.original_cursor))
    return FALSE;
    
  cm.console_size = csbi.dwSize;
  cm.original_pos = csbi.dwCursorPosition;
  
  if(force_mark_mode)
    start_mark_mode(&cm);
    
  result = run_mark_mode(&cm, event);
  finish_mark_mode(&cm);
  
  return result;
}

