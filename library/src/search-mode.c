#include <hyper-console.h>

#include "search-mode.h"
#include "console-buffer-io.h"
#include "memory-util.h"
#include "text-util.h"

#include <assert.h>
#include <strsafe.h>


#ifndef MOUSE_HWHEELED
#  define MOUSE_HWHEELED 0x0008
#endif


#define MIN(A, B)  ((A) < (B) ? (A) : (B))
#define MAX(A, B)  ((A) > (B) ? (A) : (B))


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
  
  COORD                last_result_pos;
  COORD                original_pos;
  wchar_t             *oritinal_title;
  CONSOLE_CURSOR_INFO  original_cursor;
  
  WORD *highlight_attributes;
  int highlight_attributes_capacity;
  
  WORD *result_attributes;
  int result_attributes_capacity;
  
  wchar_t *filter_text;
  int filter_length;
  int filter_capacity;
  
  int filter_anchor;
  int filter_pos;
  
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
static void reset_filter(struct console_search_t *cs, int old_length);

static BOOL resize_filter_text(struct console_search_t *cs, int length);
static BOOL raw_edit_filter_selection(struct console_search_t *cs, const wchar_t *ins, int ins_length);
static BOOL add_filter_char(struct console_search_t *cs, wchar_t ch);
static BOOL remove_filter_selection(struct console_search_t *cs);

static void move_filter_selection_left(struct console_search_t *cs, BOOL fix_anchor, BOOL jump_word);
static void move_filter_selection_right(struct console_search_t *cs, BOOL fix_anchor, BOOL jump_word);

static void update_filter_selection(struct console_search_t *cs);
static void set_filter_title(struct console_search_t *cs);
static void goto_result(struct console_search_t *cs);
static void goto_next_result(struct console_search_t *cs, BOOL forward);

static void copy_filter_selection_to_clipboard(struct console_search_t *cs);

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
        
        new_match->position.Y = (SHORT)(start / cs->console_size.X);
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
    
    cs->last_result_pos = tmp->position;
    
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
      
      hyper_console_free_memory(tmp);
      
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
    
    hyper_console_free_memory(tmp);
      
    cs->current_result = prev;
  }
  
  if(cs->current_result) {
    cs->last_result_pos = cs->current_result->position;
    console_write_output_attribute(
      cs->output_handle, 
      cs->result_attributes, 
      cs->filter_length,
      cs->last_result_pos,
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
      COORD tmp_pos;
      int index = new_length + current->position.Y * console_size.X + current->position.X;
      
      tmp_pos.Y = (SHORT)(index / console_size.X);
      tmp_pos.X = index % console_size.X;
      
      console_write_output_attribute(
        cs->output_handle, 
        cs->attributes + index, 
        old_length - new_length,
        tmp_pos,
        &written);
      
      current = current->next;
      
      hyper_console_free_memory(tmp);
    }
    
    cs->current_result = NULL;
  }
  else
    pos = cs->last_result_pos;
  
  find_all_at(cs, pos);
}

static void reset_filter(struct console_search_t *cs, int old_length) {
  COORD pos;
  
  assert(cs != NULL);
  
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
      int index = current->position.Y * console_size.X + current->position.X;
      
      console_write_output_attribute(
        cs->output_handle, 
        cs->attributes + index, 
        old_length,
        current->position,
        &written);
      
      current = current->next;
      
      hyper_console_free_memory(tmp);
    }
    
    cs->current_result = NULL;
  }
  else
    pos = cs->last_result_pos;
  
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

static BOOL raw_edit_filter_selection(struct console_search_t *cs, const wchar_t *ins, int ins_length) {
  int old_length;
  int new_length;
  int start;
  int end;
  
  assert(cs != NULL);
  
  old_length = cs->filter_length;
  assert(cs->filter_pos <= old_length);
  assert(cs->filter_anchor <= old_length);
  
  if(ins)
    assert(ins_length >= 0);
  else
    assert(ins_length == 0);
  
  start = MIN(cs->filter_anchor, cs->filter_pos);
  end   = MAX(cs->filter_anchor, cs->filter_pos);
  new_length = old_length - (end - start) + ins_length;
  
  if(new_length > old_length) {
    if(!resize_filter_text(cs, new_length)) 
      return FALSE;
  }
  else
    cs->filter_length = new_length;
  
  memmove(
    cs->filter_text + start + ins_length,
    cs->filter_text + end,
    old_length - end);
  
  if(ins) {
    memmove(
      cs->filter_text + start,
      ins,
      ins_length);
  }
  
  cs->filter_anchor = start;
  cs->filter_pos = start + ins_length;
  return TRUE;
}

static BOOL add_filter_char(struct console_search_t *cs, wchar_t ch) {
  int old_length;
  BOOL append_only;
  
  assert(cs != NULL);
  
  old_length = cs->filter_length;
  assert(cs->filter_pos <= old_length);
  
  append_only = cs->filter_pos == old_length && cs->filter_anchor == old_length;
  if(!raw_edit_filter_selection(cs, &ch, 1))
    return FALSE;
  
  cs->filter_anchor = cs->filter_pos;
  
  if(cs->current_result && append_only) {
    extended_filter(cs, old_length);
  }
  else {
    reset_filter(cs, old_length);
  }
  
  update_filter_selection(cs);
  
  return TRUE;
}

static BOOL remove_filter_selection(struct console_search_t *cs) {
  int old_length;
  
  assert(cs != NULL);
  
  old_length = cs->filter_length;
  assert(cs->filter_pos >= 0);
  assert(cs->filter_pos <= old_length);
  assert(cs->filter_anchor >= 0);
  assert(cs->filter_anchor <= old_length);
  
  if(cs->filter_pos == cs->filter_anchor)
    return FALSE;
  
  if(!raw_edit_filter_selection(cs, NULL, 0))
    return FALSE;
  
  if(cs->filter_pos == cs->filter_length) {
    truncated_filter(cs, old_length);
  }
  else {
    reset_filter(cs, old_length);
  }
  
  update_filter_selection(cs);
  
  return TRUE;
}


static void move_filter_selection_left(struct console_search_t *cs, BOOL fix_anchor, BOOL jump_word) {
  int new_pos;

  assert(cs != NULL);
  if(!cs->filter_text)
    return;
  
  new_pos = cs->filter_pos;
  if (new_pos > 0)
    new_pos--;
    
  if(jump_word)
    new_pos = console_get_word_start(cs->filter_text, cs->filter_length, new_pos);
    
  if(fix_anchor) {
    cs->filter_pos = new_pos;
  }
  else {
    if(cs->filter_pos < cs->filter_anchor) {
      cs->filter_anchor = cs->filter_pos;
    }
    else if(cs->filter_anchor < cs->filter_pos) {
      cs->filter_pos = cs->filter_anchor;
    }
    else {
      cs->filter_pos = cs->filter_anchor = new_pos;
    }
  }
}

static void move_filter_selection_right(struct console_search_t *cs, BOOL fix_anchor, BOOL jump_word) {
  int new_pos;
  
  assert(cs != NULL);
    
  new_pos = cs->filter_pos;
  if(jump_word)
    new_pos = console_get_word_end(cs->filter_text, cs->filter_length, new_pos);
  else if(new_pos < cs->filter_length)
    new_pos++;
    
  if(fix_anchor) {
    cs->filter_pos = new_pos;
  }
  else {
    if(cs->filter_pos > cs->filter_anchor) {
      cs->filter_anchor = cs->filter_pos;
    }
    else if(cs->filter_anchor > cs->filter_pos) {
      cs->filter_pos = cs->filter_anchor;
    }
    else {
      cs->filter_pos = cs->filter_anchor = new_pos;
    }
  }
}


static void update_filter_selection(struct console_search_t *cs) {
  
  assert(cs != NULL);
  
  set_filter_title(cs);
  goto_result(cs);
}

static void set_filter_title(struct console_search_t *cs) {
  wchar_t text[MAX_PATH];
  wchar_t *s = text;
  wchar_t *end = text + ARRAYSIZE(text) - 1;
  
  assert(cs != NULL);
  
  if(!cs->oritinal_title)
    return;
  
  s = append_text(s, end, L"Find ", NULL);
  if(cs->filter_text) {
    int sel_start = MIN(cs->filter_pos, cs->filter_anchor);
    int sel_end   = MAX(cs->filter_pos, cs->filter_anchor);
    
    s = append_text(s, end, cs->filter_text, cs->filter_text + sel_start);
    if(sel_start == sel_end) {
      s = append_text(s, end, L"|", NULL);
    }
    else {
      s = append_text(s, end, L"[", NULL);
      
      s = append_text(s, end, cs->filter_text + sel_start, cs->filter_text + sel_end);
      
      s = append_text(s, end, L"]", NULL);
    }
    
    s = append_text(s, end, cs->filter_text + sel_end, cs->filter_text + cs->filter_length);
  }
  *s = L'\0';
  
  SetConsoleTitleW(text);
}

static void goto_result(struct console_search_t *cs) {
  COORD pos;
  
  assert(cs != NULL);
  
  pos = cs->last_result_pos;
  if(cs->current_result) {
    int index = MIN(cs->filter_pos, cs->filter_anchor);
    if(index > 0) {
      index += pos.Y * cs->console_size.X + pos.X;
      pos.Y = (SHORT)(index / cs->console_size.X);
      pos.X = index % cs->console_size.X;
    }
  }
  
  SetConsoleCursorPosition(cs->output_handle, pos);
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
    cs->last_result_pos = result->position;
    goto_result(cs);
  }
  else {
    console_alert(cs->output_handle);
  }
}


static void copy_filter_selection_to_clipboard(struct console_search_t *cs) {
  int start;
  int end;
  HGLOBAL copy_handle;
  wchar_t *copy_data;
  
  assert(cs != NULL);
    
  start = MIN(cs->filter_anchor, cs->filter_pos);
  end   = MAX(cs->filter_anchor, cs->filter_pos);
  
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
  memcpy(copy_data, cs->filter_text + start, (end - start) * sizeof(wchar_t));
  copy_data[end - start] = L'\0';
  GlobalUnlock(copy_handle);
  
  SetClipboardData(CF_UNICODETEXT, copy_handle);
  
  CloseClipboard();
}


static BOOL start_search_mode(struct console_search_t *cs) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  CONSOLE_CURSOR_INFO cci;
  
  assert(cs != NULL);
  
  if(GetConsoleScreenBufferInfo(cs->output_handle, &csbi)) {
    COORD pos;
    DWORD read;
    DWORD length;
    
    pos.X = pos.Y = 0;
    
    cs->console_size = csbi.dwSize;
    
    read = 0;
    length = csbi.dwSize.X * csbi.dwSize.Y;
    
    hyper_console_free_memory(cs->screen);
    cs->screen = allocate_memory(length * sizeof(wchar_t));
    if( !cs->screen || 
        !console_read_output_character(cs->output_handle, cs->screen, length, pos, &read) ||
        read != length) 
    {
      hyper_console_free_memory(cs->screen);
      cs->screen = NULL;
      return FALSE;
    }
    
    read = 0;
    hyper_console_free_memory(cs->attributes);
    cs->attributes = allocate_memory(length * sizeof(WORD));
    if( !cs->attributes || 
        !console_read_output_attribute(cs->output_handle, cs->attributes, length, pos, &read) ||
        read != length) 
    {
      hyper_console_free_memory(cs->screen);
      cs->screen = NULL;
      
      hyper_console_free_memory(cs->attributes);
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
  
  cci.bVisible = TRUE;
  cci.dwSize = 100;
  SetConsoleCursorInfo(cs->output_handle, &cci);
    
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
        if(cs->filter_anchor == cs->filter_pos) {
          move_filter_selection_left(
            cs,
            TRUE,
            er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
        }
        if(!remove_filter_selection(cs)) {
          console_alert(cs->output_handle);
        }
        return TRUE;
      
      case VK_DELETE:
        if(cs->filter_anchor == cs->filter_pos) {
          move_filter_selection_right(
            cs,
            TRUE,
            er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
        }
        if(!remove_filter_selection(cs)) {
          console_alert(cs->output_handle);
        }
        return TRUE;
      
      case VK_LEFT:
        move_filter_selection_left(
          cs,
          er->dwControlKeyState & SHIFT_PRESSED,
          er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
        update_filter_selection(cs);
        return TRUE;
      
      case VK_RIGHT:
        move_filter_selection_right(
          cs,
          er->dwControlKeyState & SHIFT_PRESSED,
          er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
        update_filter_selection(cs);
        return TRUE;
      
      case 'A': // Ctrl+A
        if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          if(cs->filter_text) {
            cs->filter_anchor = 0;
            cs->filter_pos = cs->filter_length;
            update_filter_selection(cs);
            return TRUE;
          }
        }
        break;
        
      case 'F': // Ctrl+F
        if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          if(cs->filter_text) {
            int old_anchor = cs->filter_anchor;
            int old_pos = cs->filter_pos;
            
            cs->filter_anchor = 0;
            cs->filter_pos = cs->filter_length;
            update_filter_selection(cs);
            
            if(old_pos != cs->filter_pos || old_anchor != cs->filter_anchor)
              return TRUE;
          }
          goto_next_result(cs, er->dwControlKeyState & SHIFT_PRESSED);
          return TRUE;
        }
        break;
      
      case VK_INSERT: // Ctrl+Ins = copy, Shift+Ins = paste
        if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          if(cs->filter_anchor != cs->filter_pos) {
            copy_filter_selection_to_clipboard(cs);
            return TRUE;
          }
          else { // empty selection -> beep
            console_alert(cs->output_handle);
            return TRUE;
          }
        }
        if(er->dwControlKeyState & SHIFT_PRESSED) {
          console_paste_from_clipboard(cs->input_handle);
          return TRUE;
        }
        break;
        
      case 'C': // Ctrl+C
        if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          if(cs->filter_anchor != cs->filter_pos) {
            copy_filter_selection_to_clipboard(cs);
            cs->filter_anchor = cs->filter_pos;
            update_filter_selection(cs);
            return TRUE;
          }
          else { // empty selection -> beep
            console_alert(cs->output_handle);
            return TRUE;
          }
        }
        break;
        
      case 'X': // Ctrl+C
        if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          if(cs->filter_anchor != cs->filter_pos) {
            copy_filter_selection_to_clipboard(cs);
            remove_filter_selection(cs);
            return TRUE;
          }
          else { // empty selection -> beep
            console_alert(cs->output_handle);
            return TRUE;
          }
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
  CONSOLE_CURSOR_INFO cci;
  
  assert(cs != NULL);
  
  if(cs->oritinal_title) {
    SetConsoleTitleW(cs->oritinal_title);
    
    hyper_console_free_memory(cs->oritinal_title);
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
  
  hyper_console_free_memory(cs->screen);
  hyper_console_free_memory(cs->attributes);
  
  hyper_console_free_memory(cs->highlight_attributes);
  hyper_console_free_memory(cs->result_attributes);
  hyper_console_free_memory(cs->filter_text);
  
  if(cs->current_result) {
    assert(cs->current_result->prev != NULL);
    assert(cs->current_result->prev->next == cs->current_result);
    assert(cs->current_result->next != NULL);
    assert(cs->current_result->next->prev == cs->current_result);
    
    cs->current_result->prev->next = NULL;
    while(cs->current_result) {
      struct match_t *tmp = cs->current_result;
      
      cs->current_result = tmp->next;
      
      hyper_console_free_memory(tmp);
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
  
  cci = cs->original_cursor;
  GetConsoleCursorInfo(cs->output_handle, &cci);
  if(0 != memcmp(&cci, &cs->original_cursor, sizeof(cci))) {
    SetConsoleCursorInfo(cs->output_handle, &cs->original_cursor);
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
    
  if(!GetConsoleCursorInfo(hConsoleOutput, &cs->original_cursor))
    return FALSE;
    
  cs->console_size    = csbi.dwSize;
  cs->original_pos    = csbi.dwCursorPosition;
  cs->last_result_pos = csbi.dwCursorPosition;
  
  if(filter) {
    int length = (int)wcslen(filter);
    
    start_search_mode(cs);
    
    if(resize_filter_text(cs, length)) {
      memcpy(cs->filter_text, filter, length * sizeof(wchar_t));
      cs->filter_pos = length;
    }
    
    reset_filter(cs, 0);
    update_filter_selection(cs);
  }
  
  result = run_search_mode(cs, event);
  finish_search_mode(cs);
  
  return result;
}
