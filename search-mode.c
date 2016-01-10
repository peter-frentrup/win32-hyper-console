#include "search-mode.h"
#include "console-buffer-io.h"
#include "memory-util.h"

#include <assert.h>
#include <strsafe.h>


#ifndef MOUSE_HWHEELED
#  define MOUSE_HWHEELED 0x0008
#endif


struct match_t {
  struct match_t *prev;
  struct match_t *next;
  
  COORD position;
};

struct console_search_t {
  HANDLE input_handle;
  HANDLE output_handle;
  
  COORD console_size;
  wchar_t *screen;
  WORD *attributes;
  
  COORD     original_pos;
  wchar_t  *oritinal_title;
  
  WORD *highlight_attributes;
  int highlight_attributes_capacity;
  
  WORD *result_attributes;
  int result_attributes_capacity;
  
  wchar_t *filter_text;
  int filter_length;
  int filter_capacity;
  
  struct match_t *current_result;
  
  WORD highlight_attr;
  WORD current_attr;
  
  unsigned active: 1;
  unsigned stop: 1;
  unsigned dont_follow_cursor: 1;
};

static BOOL should_ignore_filter(struct console_search_t *cs);
static BOOL is_match_at_index(struct console_search_t *cs, int index);
static struct match_t *find_all_within(struct console_search_t *cs, int start, int end);
static void find_all_at(struct console_search_t *cs, COORD pos);
static void extended_filter(struct console_search_t *cs, int old_length);
static void truncated_filter(struct console_search_t *cs, int old_length);

static BOOL resize_filter_text(struct console_search_t *cs, int length);
static BOOL add_filter_char(struct console_search_t *cs, wchar_t ch);
static BOOL remove_filter_char(struct console_search_t *cs);

static void set_filter_title(struct console_search_t *cs);
static void goto_result(struct console_search_t *cs);
static void goto_next_result(struct console_search_t *cs, BOOL forward);

static BOOL start_search_mode(struct console_search_t *cs);

static BOOL search_mode_handle_key_event(struct console_search_t *cs, KEY_EVENT_RECORD *er);
static BOOL search_mode_handle_mouse_event(struct console_search_t *cs, MOUSE_EVENT_RECORD *er);

static BOOL run_search_mode(struct console_search_t *cs, INPUT_RECORD *event);
static void finish_search_mode(struct console_search_t *cs);


static BOOL should_ignore_filter(struct console_search_t *cs) {
  const wchar_t *s;
  const wchar_t *e;
  
  assert(cs != NULL);
  
  s = cs->filter_text;
  e = cs->filter_text + cs->filter_length;
  for(;s != e;++s) {
    if(*s != ' ')
      return FALSE;
  }
  
  return TRUE;
}

static BOOL is_match_at_index(struct console_search_t *cs, int index) {
  int cmp;
  
  assert(cs != NULL);
  assert(cs->filter_length >= 0);
  
  assert(index >= 0);
  assert(index <= cs->console_size.X * cs->console_size.Y);
  
  if(!cs->screen || cs->filter_length == 0)
    return FALSE;
  
  assert(cs->filter_text != NULL);
  
  if(index > cs->console_size.X * cs->console_size.Y - cs->filter_length)
    return FALSE;
  
  cmp = CompareStringW(
    LOCALE_USER_DEFAULT, 
    LINGUISTIC_IGNORECASE, 
    cs->screen + index, 
    cs->filter_length,
    cs->filter_text,
    cs->filter_length);
  
  return (cmp == CSTR_EQUAL);
}

static struct match_t *find_all_within(struct console_search_t *cs, int start, int end) {
  struct match_t *result = NULL;
  
  assert(cs != NULL);
  assert(0 <= start);
  assert(start <= end);
  assert(end <= cs->console_size.X * cs->console_size.Y);
  
  if(cs->filter_length == 0)
    return NULL;
  
  while(start < end) {
    if(is_match_at_index(cs, start)) {
      struct match_t *new_match = (struct match_t*)allocate_memory(sizeof(struct match_t));
      
      if(new_match) {
        if(result) {
          new_match->next = result;
          new_match->prev = result->prev;
          new_match->prev->next = new_match;
          result->prev = new_match;
        }
        else{
          result = new_match->prev = new_match->next = new_match;
        }
        
        new_match->position.Y = start / cs->console_size.X;
        new_match->position.X = start % cs->console_size.X;
      }
      
      start+= cs->filter_length;
    }
    else
      ++start;
  }
  
  return result;
}

static void find_all_at(struct console_search_t *cs, COORD pos) {
  int index;
  struct match_t *above;
  struct match_t *below;
  DWORD written;
  
  assert(cs != NULL);
  assert(pos.X >= 0);
  assert(pos.Y >= 0);
  
  if(!cs->active)
    return;
  
  assert(cs->current_result == NULL);
  
  if(cs->filter_length == 0 || should_ignore_filter(cs))
    return;
     
  index = pos.Y * cs->console_size.X + pos.X;
  above = find_all_within(cs, 0, index);
  below = find_all_within(cs, index, cs->console_size.X * cs->console_size.Y);
  
  if(above && below) {
    struct match_t *last_above = above->prev;
    
    below->prev->next = above;
    above->prev = below->prev;
    
    below->prev = last_above;
    last_above->next = below;
  }
  else if(above)
    below = above;
  
  if(below) {
    if(below->position.X == pos.X && below->position.Y == pos.Y)
      cs->current_result = below;
    else 
      cs->current_result = below->prev;
  }
  
  if(cs->current_result) {
    struct match_t *tmp = cs->current_result->next;
    
    for(;tmp != cs->current_result;tmp = tmp->next) {
      console_write_output_attribute(
        cs->output_handle, 
        cs->highlight_attributes, 
        cs->filter_length,
        tmp->position,
        &written);
    }
    
    console_write_output_attribute(
      cs->output_handle, 
      cs->result_attributes, 
      cs->filter_length,
      tmp->position,
      &written);
  }
}

static void extended_filter(struct console_search_t *cs, int old_length) {
  struct match_t *tmp;
  struct match_t *prev;
  DWORD written;
  int index;
  
  assert(cs != NULL);
  
  if(!cs->current_result)
    return;
  
  tmp = cs->current_result->prev;
  while(tmp != cs->current_result) {
    index = tmp->position.Y * cs->console_size.X + tmp->position.X;
    prev = tmp->prev;
    
    if(is_match_at_index(cs, index)) {
      console_write_output_attribute(
        cs->output_handle, 
        cs->highlight_attributes, 
        cs->filter_length,
        tmp->position,
        &written);
      
      tmp = prev;
    }
    else {
      console_write_output_attribute(
        cs->output_handle, 
        cs->attributes + index, 
        old_length,
        tmp->position,
        &written);
      
      tmp->next->prev = prev;
      prev->next = tmp->next;
      
      free_memory(tmp);
      
      tmp = prev;
    }
  }
  
  index = tmp->position.Y * cs->console_size.X + tmp->position.X;
  if(!is_match_at_index(cs, index)) {
    console_write_output_attribute(
      cs->output_handle, 
      cs->attributes + index, 
      old_length,
      tmp->position,
      &written);
    
    prev = tmp->prev;
    
    if(prev != tmp) {
      tmp->next->prev = prev;
      prev->next = tmp->next;
    }
    else 
      prev = NULL;
    
    free_memory(tmp);
      
    cs->current_result = prev;
  }
  
  if(cs->current_result) {
    console_write_output_attribute(
      cs->output_handle, 
      cs->result_attributes, 
      cs->filter_length,
      cs->current_result->position,
      &written);
  }
}

static void truncated_filter(struct console_search_t *cs, int old_length) {
  COORD pos;
  int new_length;
  
  assert(cs != NULL);
  
  if(!cs->active)
    return;
    
  new_length = cs->filter_length;
  assert(new_length < old_length);
  
  if(cs->current_result) {
    struct match_t *current;
    DWORD written;
    COORD console_size = cs->console_size;
    
    pos = cs->current_result->position;
    
    assert(cs->current_result->prev != NULL);
    assert(cs->current_result->prev->next == cs->current_result);
    assert(cs->current_result->next != NULL);
    assert(cs->current_result->next->prev == cs->current_result);
    
    current = cs->current_result;
    current->prev->next = NULL;
    while(current) {
      struct match_t *tmp = current;
      COORD pos;
      int index = new_length + current->position.Y * console_size.X + current->position.X;
      
      pos.Y = index / console_size.X;
      pos.X = index % console_size.X;
      
      console_write_output_attribute(
        cs->output_handle, 
        cs->attributes + index, 
        old_length - new_length,
        pos,
        &written);
      
      current = current->next;
      
      free_memory(tmp);
    }
    
    cs->current_result = NULL;
  }
  else
    pos = cs->original_pos;
  
  find_all_at(cs, pos);
}

static BOOL resize_filter_text(struct console_search_t *cs, int length) {
  int i;
  
  assert(cs != NULL);
    
  if(!resize_array(
        (void**)&cs->filter_text,
        &cs->filter_capacity,
        sizeof(cs->filter_text[0]),
        length + 1))
  {
    cs->filter_length = 0;
    return FALSE;
  }
  
  if(!resize_array(
        (void**)&cs->highlight_attributes,
        &cs->highlight_attributes_capacity,
        sizeof(cs->highlight_attributes[0]),
        length))
  {
    cs->filter_length = 0;
    return FALSE;
  }
  
  if(!resize_array(
        (void**)&cs->result_attributes,
        &cs->result_attributes_capacity,
        sizeof(cs->result_attributes[0]),
        length))
  {
    cs->filter_length = 0;
    return FALSE;
  }
  
  cs->filter_text[length] = L'\0';
  cs->filter_length = length;
  
  if(length > 0){
    for(i = 0;i < length;++i)
      cs->highlight_attributes[i] = cs->highlight_attr;
    
    cs->highlight_attributes[0]        |= COMMON_LVB_GRID_LVERTICAL;
    cs->highlight_attributes[length-1] |= COMMON_LVB_GRID_RVERTICAL;
    
    
    for(i = 0;i < length;++i)
      cs->result_attributes[i] = cs->current_attr;
    
    cs->result_attributes[0]        |= COMMON_LVB_GRID_LVERTICAL;
    cs->result_attributes[length-1] |= COMMON_LVB_GRID_RVERTICAL;
  }
  
  return TRUE;
}

static BOOL add_filter_char(struct console_search_t *cs, wchar_t ch) {
  assert(cs != NULL);
  
  if(!resize_filter_text(cs, cs->filter_length + 1)) {
    return FALSE;
  }
  
  cs->filter_text[cs->filter_length - 1] = ch;
  
  set_filter_title(cs);
  
  if(cs->current_result) {
    extended_filter(cs, cs->filter_length - 1);
  }
  else {
    find_all_at(cs, cs->original_pos);
  }
  
  goto_result(cs);
  
  return TRUE;
}

static BOOL remove_filter_char(struct console_search_t *cs) {
  assert(cs != NULL);
  
  if(cs->filter_length == 0)
    return FALSE;
  
  if(!resize_filter_text(cs, cs->filter_length - 1)) {
    return FALSE;
  }
  
  set_filter_title(cs);
  truncated_filter(cs, cs->filter_length + 1);
  goto_result(cs);
  
  return TRUE;
}

static void set_filter_title(struct console_search_t *cs) {
  wchar_t text[MAX_PATH];
  
  if(!cs->oritinal_title)
    return;
  
  if(cs->filter_text)
    StringCbPrintfW(text, sizeof(text), L"Find \x201C%s\x201D", cs->filter_text);
  else
    StringCbCopyW(text, sizeof(text), L"Find \x201C\x201D");
  
  SetConsoleTitleW(text);
}

static void goto_result(struct console_search_t *cs) {
  assert(cs != NULL);
  
  if(cs->current_result) {
    SetConsoleCursorPosition(cs->output_handle, cs->current_result->position);
  }
}

static void goto_next_result(struct console_search_t *cs, BOOL forward) {
  struct match_t *result;
  DWORD written;
  
  assert(cs != NULL);
  
  result = cs->current_result;
  if(result && result->prev != result) {
    console_write_output_attribute(
      cs->output_handle,
      cs->highlight_attributes,
      cs->filter_length,
      result->position,
      &written);
    
    result = forward ? result->next : result->prev;
    console_write_output_attribute(
      cs->output_handle,
      cs->result_attributes,
      cs->filter_length,
      result->position,
      &written);
    
    cs->current_result = result;
    goto_result(cs);
  }
  else {
    console_alert(cs->output_handle);
  }
}

static BOOL start_search_mode(struct console_search_t *cs) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(cs != NULL);
  
  if(GetConsoleScreenBufferInfo(cs->output_handle, &csbi)) {
    COORD pos;
    DWORD read;
    DWORD length;
    
    pos.X = pos.Y = 0;
    
    cs->console_size = csbi.dwSize;
    
    read = 0;
    length = csbi.dwSize.X * csbi.dwSize.Y;
    
    free_memory(cs->screen);
    cs->screen = allocate_memory(length * sizeof(wchar_t));
    if( !cs->screen || 
        !console_read_output_character(cs->output_handle, cs->screen, length, pos, &read) ||
        read != length) 
    {
      free_memory(cs->screen);
      cs->screen = NULL;
      return FALSE;
    }
    
    read = 0;
    free_memory(cs->attributes);
    cs->attributes = allocate_memory(length * sizeof(WORD));
    if( !cs->attributes || 
        !console_read_output_attribute(cs->output_handle, cs->attributes, length, pos, &read) ||
        read != length) 
    {
      free_memory(cs->screen);
      cs->screen = NULL;
      
      free_memory(cs->attributes);
      cs->attributes = NULL;
      return FALSE;
    }
    
    cs->active = TRUE;
  }
  
  if(cs->oritinal_title == NULL) {
    cs->oritinal_title = allocate_memory(sizeof(wchar_t) * 256);
    if(cs->oritinal_title) {
      GetConsoleTitleW(cs->oritinal_title, 256);
    }
  }
  
  set_filter_title(cs);
    
  return TRUE;
}

static BOOL search_mode_handle_key_event(struct console_search_t *cs, KEY_EVENT_RECORD *er) {
  assert(cs != NULL);
  assert(er != NULL);
  
  if(!er->bKeyDown) 
    return cs->active;
    
  switch(er->wVirtualKeyCode) {
    case VK_SHIFT:
    case VK_CONTROL:
    case VK_MENU:
      return cs->active;
  }
  
  if(cs->active) {
    switch(er->wVirtualKeyCode) {
      case VK_ESCAPE:
        cs->stop = TRUE;
        return TRUE;
      
      case VK_BACK:
        remove_filter_char(cs);
        return TRUE;
        
      case 'F': // Ctrl+F
        if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          goto_next_result(cs, er->dwControlKeyState & SHIFT_PRESSED);
          return TRUE;
        }
        break;
      
      case 'V': // Ctrl+V
        if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          console_paste_from_clipboard(cs->input_handle);
          return TRUE;
        }
        break;
        
      case VK_RETURN:
      case VK_F3:
        goto_next_result(cs, er->dwControlKeyState & SHIFT_PRESSED);
        return TRUE;
    }
    
    if((unsigned)er->uChar.UnicodeChar >= (unsigned)L' ') {
      add_filter_char(cs, er->uChar.UnicodeChar);
      return TRUE;
    }
  }
  else {
    switch(er->wVirtualKeyCode) {
      case 'F': // Ctrl+F
        if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          start_search_mode(cs);
          return TRUE;
        }
        break;
    }
  }
  
  if(cs->active)
    console_alert(cs->output_handle);
  
  //cs->stop = TRUE;
  //return FALSE;
  return cs->active;
}

static BOOL search_mode_handle_mouse_event(struct console_search_t *cs, MOUSE_EVENT_RECORD *er) {
  assert(cs != NULL);
  assert(er != NULL);
  
  switch(er->dwEventFlags) {
    case 0: /* mouse down/up */
      cs->stop = TRUE;
      cs->dont_follow_cursor = TRUE;
      return FALSE;
      
    case DOUBLE_CLICK:
      return cs->active;
      
    case MOUSE_MOVED:
      return cs->active;
      
    case MOUSE_HWHEELED:
    case MOUSE_WHEELED:
      if(cs->active) {
        console_scroll_wheel(cs->output_handle, er);
        return TRUE;
      }
      return FALSE;
  }
  
  return FALSE;
}

static BOOL run_search_mode(struct console_search_t *cs, INPUT_RECORD *event) {
  assert(cs != NULL);
  assert(event != NULL);
  
  for(;;) {
    DWORD num_read;
    
    switch(event->EventType) {
      case KEY_EVENT:
        if(!search_mode_handle_key_event(cs, &event->Event.KeyEvent))
          return FALSE;
        break;
        
      case MOUSE_EVENT:
        if(!search_mode_handle_mouse_event(cs, &event->Event.MouseEvent))
          return FALSE;
        break;
        
      default:
        if(!cs->active)
          return FALSE;
        break;
    }
    
    if(!cs->active || cs->stop)
      return TRUE;
      
    if(!ReadConsoleInputW(cs->input_handle, event, 1, &num_read) || num_read < 1)
      return TRUE;
  };
}

static void finish_search_mode(struct console_search_t *cs) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(cs != NULL);
  
  if(cs->oritinal_title) {
    SetConsoleTitleW(cs->oritinal_title);
    
    free_memory(cs->oritinal_title);
    cs->oritinal_title = NULL;
  }
  
  if(cs->attributes) {
    COORD pos;
    DWORD length = cs->console_size.X * cs->console_size.Y;
    DWORD written;
    pos.X = 0;
    pos.Y = 0;
    console_write_output_attribute(cs->output_handle, cs->attributes, length, pos, &written);
  }
  
  free_memory(cs->screen);
  free_memory(cs->attributes);
  
  free_memory(cs->highlight_attributes);
  free_memory(cs->result_attributes);
  free_memory(cs->filter_text);
  
  if(cs->current_result) {
    assert(cs->current_result->prev != NULL);
    assert(cs->current_result->prev->next == cs->current_result);
    assert(cs->current_result->next != NULL);
    assert(cs->current_result->next->prev == cs->current_result);
    
    cs->current_result->prev->next = NULL;
    while(cs->current_result) {
      struct match_t *tmp = cs->current_result;
      
      cs->current_result = tmp->next;
      
      free_memory(tmp);
    }
  }
  
  if(GetConsoleScreenBufferInfo(cs->output_handle, &csbi)) {
    COORD pos = cs->original_pos;
    
    if(csbi.dwCursorPosition.X != pos.X || csbi.dwCursorPosition.Y != pos.Y) {
      SetConsoleCursorPosition(cs->output_handle, pos);
      
      if(cs->dont_follow_cursor)
        SetConsoleWindowInfo(cs->output_handle, TRUE, &csbi.srWindow);
    }
  }
}

BOOL console_handle_search_mode(HANDLE hConsoleInput, HANDLE hConsoleOutput, INPUT_RECORD *event, const wchar_t *filter) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  struct console_search_t cs[1];
  BOOL result;
  
  assert(event != NULL);
  
  memset(cs, 0, sizeof(cs));
  cs->input_handle = hConsoleInput;
  cs->output_handle = hConsoleOutput;
  cs->highlight_attr = BACKGROUND_INTENSITY | BACKGROUND_RED | BACKGROUND_GREEN | COMMON_LVB_UNDERSCORE | COMMON_LVB_GRID_HORIZONTAL;
  cs->current_attr   = BACKGROUND_INTENSITY | BACKGROUND_RED | FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | COMMON_LVB_UNDERSCORE | COMMON_LVB_GRID_HORIZONTAL;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi))
    return FALSE;
    
  cs->console_size = csbi.dwSize;
  cs->original_pos = csbi.dwCursorPosition;
  
  if(filter) {
    int length = wcslen(filter);
    
    start_search_mode(cs);
    
    if(resize_filter_text(cs, length)) {
      memcpy(cs->filter_text, filter, length * sizeof(wchar_t));
    }
    
    set_filter_title(cs);
    find_all_at(cs, cs->original_pos);
  }
  
  result = run_search_mode(cs, event);
  finish_search_mode(cs);
  
  return result;
}
