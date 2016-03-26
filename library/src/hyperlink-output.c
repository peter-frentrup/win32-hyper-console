#include "hyperlink-output.h"

#include "console-buffer-io.h"
#include "read-input.h"
#include "scroll-counter.h"
#include "memory-util.h"
#include "debug.h"

#include <assert.h>
#include <stdio.h>
#include <windows.h>

#define LINE_CANARY_SIZE  3

struct hyperlink_t {
  struct hyperlink_t *prev_link;
  
  int start_global_line;
  int start_column;
  
  int end_global_line;
  int end_column;
  
  wchar_t *title;
  wchar_t *input_text;
  
  int inactive_attribute_count;
  WORD *inactive_attributes;
  
  WORD attr_previous;
  WORD attr_active;
};

struct hyperlink_collection_t {
  HANDLE output_handle;
  
  int console_width;
  
  struct console_scrollback_t *scrollback;
  
  int num_failed_open_links;
  int num_open_links;
  struct hyperlink_t *last_link;
  
  struct hyperlink_t *mouse_over_link;
  struct hyperlink_t *pressed_link;
  
  wchar_t old_title[256];
  
  WORD attr_default;
  WORD attr_link;
};

static void init_hyperlink_collection(struct hyperlink_collection_t *hc);
static void init_colors(struct hyperlink_collection_t *hc);
static void free_hyperlink_collection(struct hyperlink_collection_t *hc);

static void free_link_at(struct hyperlink_t **last_link);
static void clean_old_links(struct hyperlink_collection_t *hc, int first_keep_line);
static struct hyperlink_t *open_new_link(struct hyperlink_collection_t *hc);
static void close_link(struct hyperlink_collection_t *hc);
static void set_open_link_title(struct hyperlink_collection_t *hc, const wchar_t *title, int title_length);
static void set_open_link_input_text(struct hyperlink_collection_t *hc, const wchar_t *text, int text_length);
static WORD set_open_link_color(struct hyperlink_collection_t *hc, WORD attribute);

static BOOL is_global_position_before(int a_line, int a_col, int b_line, int b_col);
static struct hyperlink_t *find_link(struct hyperlink_collection_t *hc, COORD pos);
static int find_link_visual_position(struct hyperlink_collection_t *hc, const struct hyperlink_t *link, COORD *position);

static BOOL save_old_console_title(struct hyperlink_collection_t *hc);
static void set_console_title(struct hyperlink_collection_t *hc, const wchar_t *title);

static BOOL invert_link_colors(struct hyperlink_collection_t *hc, const struct hyperlink_t *link);
static BOOL activate_link(struct hyperlink_collection_t *hc, struct hyperlink_t *link);
static void deactivate_link(struct hyperlink_collection_t *hc, struct hyperlink_t *link);
static void activate_all_links(struct hyperlink_collection_t *hc);
static void deactivate_all_links(struct hyperlink_collection_t *hc);

static void on_mouse_enter_link(struct hyperlink_collection_t *hc);
static void on_mouse_leave_link(struct hyperlink_collection_t *hc);
static BOOL set_mouse_over_link(struct hyperlink_collection_t *hc, struct hyperlink_t *link);

static BOOL hs_handle_lbutton_down(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er);
static BOOL hs_handle_mouse_up(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er);
static BOOL hs_handle_mouse_move(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er);
static BOOL hs_handle_mouse_event(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er);

static BOOL hs_handle_key_event(struct hyperlink_collection_t *hc, const KEY_EVENT_RECORD *er);
static BOOL hs_handle_focus_event(struct hyperlink_collection_t *hc, const FOCUS_EVENT_RECORD *er);

static BOOL hs_handle_events(struct hyperlink_collection_t *hc, INPUT_RECORD *event);

static void hs_start_input(struct hyperlink_collection_t *hc, int console_width, int pre_input_lines);
static void hs_end_input(struct hyperlink_collection_t *hc);

//static void hs_print_debug_info(struct hyperlink_collection_t *hc);

static void init_hyperlink_collection(struct hyperlink_collection_t *hc) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(hc != NULL);
  
  memset(hc, 0, sizeof(struct hyperlink_collection_t));
  
  hc->output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  hc->scrollback = console_scrollback_new();
  
  hc->attr_default = 0x0F;
  if(GetConsoleScreenBufferInfo(hc->output_handle, &csbi)) {
    hc->console_width = csbi.dwSize.X;
    hc->attr_default = csbi.wAttributes;
  }
  
  init_colors(hc);
}

static void init_colors(struct hyperlink_collection_t *hc) {
  assert(hc != NULL);
  
  if((hc->attr_default & 0xF0) == (BACKGROUND_GREEN | BACKGROUND_BLUE)) {
    hc->attr_link = (hc->attr_default & 0xF0) | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  }
  else {
    hc->attr_link = (hc->attr_default & 0xF0) | FOREGROUND_GREEN | FOREGROUND_BLUE;
  }
  
  hc->attr_link = hc->attr_link | COMMON_LVB_UNDERSCORE;
}

static void free_hyperlink_collection(struct hyperlink_collection_t *hc) {
  assert(hc != NULL);
  
  if(hc->mouse_over_link)
    on_mouse_leave_link(hc);
    
  // hc->selected_link
  
  while(hc->last_link)
    free_link_at(&hc->last_link);
    
  console_scrollback_free(hc->scrollback);
  
  //hyper_console_free_memory(hc->old_title);
  memset(hc, 0, sizeof(struct hyperlink_collection_t));
}

static void free_link_at(struct hyperlink_t **last_link) {
  struct hyperlink_t *link;
  
  assert(last_link != NULL);
  
  link = *last_link;
  if(!link)
    return;
    
  *last_link = link->prev_link;
  
  hyper_console_free_memory(link->title);
  hyper_console_free_memory(link->input_text);
  hyper_console_free_memory(link->inactive_attributes);
  hyper_console_free_memory(link);
}

static void clean_old_links(struct hyperlink_collection_t *hc, int first_keep_line) {
  struct hyperlink_t **link_ptr;
  int skip_count;
  
  assert(hc);
  
  skip_count = hc->num_open_links;
  link_ptr = &hc->last_link;
  while(skip_count-- > 0) {
    assert(*link_ptr != NULL);
    
    link_ptr = &(*link_ptr)->prev_link;
  }
  
  while(*link_ptr && (*link_ptr)->end_global_line >= first_keep_line) {
    link_ptr = &(*link_ptr)->prev_link;
  }
  
  while(*link_ptr) {
    if(hc->mouse_over_link == *link_ptr) {
      on_mouse_leave_link(hc);
      hc->mouse_over_link = NULL;
    }
    
    if(hc->pressed_link == *link_ptr) {
      hc->pressed_link = NULL;
    }
    
    debug_printf(L"clean link %s in line %d < %d\n", 
      (*link_ptr)->title, 
      (*link_ptr)->start_global_line, 
      first_keep_line);
    
    free_link_at(link_ptr);
  }
}

static struct hyperlink_t *open_new_link(struct hyperlink_collection_t *hc) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  struct hyperlink_t *link;
  COORD top;
  int top_line;
  int top_column;
  int line;
  int column;
  
  assert(hc != NULL);
  
  if(hc->num_failed_open_links > 0) {
    hc->num_failed_open_links++;
    return NULL;
  }
  
  if(!GetConsoleScreenBufferInfo(hc->output_handle, &csbi)) {
    hc->num_failed_open_links++;
    return NULL;
  }
  
  console_scrollback_update(hc->scrollback, csbi.dwCursorPosition.Y);
  
  top.X = 0;
  top.Y = 0;
  if( !console_scollback_local_to_global(hc->scrollback, top, &top_line, &top_column) ||
      !console_scollback_local_to_global(hc->scrollback, csbi.dwCursorPosition, &line, &column))
  {
    hc->num_failed_open_links++;
    return NULL;
  }
  
  clean_old_links(hc, top_line);
  debug_printf(L"open new link at %d:%d (top = %d:%d)\n", line, column, top_line, top_column);
  
  link = allocate_memory(sizeof(struct hyperlink_t));
  if(!link) {
    hc->num_failed_open_links++;
    return NULL;
  }
  
  memset(link, 0, sizeof(struct hyperlink_t));
  
  link->start_column      = link->end_column      = column;
  link->start_global_line = link->end_global_line = line;
  
  link->attr_previous = csbi.wAttributes;
  link->attr_active = hc->attr_link;
  //SetConsoleTextAttribute(hc->output_handle, hc->attr_link);
  
  link->prev_link = hc->last_link;
  hc->last_link = link;
  hc->num_open_links++;
  return link;
}

static void close_link(struct hyperlink_collection_t *hc) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  struct hyperlink_t *link;
  int line;
  int column;
  
  assert(hc != NULL);
  
  if(hc->num_failed_open_links > 0) {
    hc->num_failed_open_links--;
    return;
  }
  
  link = hc->last_link;
  
  assert(hc->num_open_links > 0);
  assert(link != NULL);
  
  SetConsoleTextAttribute(hc->output_handle, link->attr_previous);
  
  hc->num_open_links--;
  if(!GetConsoleScreenBufferInfo(hc->output_handle, &csbi))
    return;
    
  console_scrollback_update(hc->scrollback, csbi.dwCursorPosition.Y);
  
  if(!console_scollback_local_to_global(hc->scrollback, csbi.dwCursorPosition, &line, &column))
    return;
    
  link->end_global_line = line;
  link->end_column      = column;
}

static void set_open_link_title(struct hyperlink_collection_t *hc, const wchar_t *title, int title_length) {
  struct hyperlink_t *link;
  
  assert(hc != NULL);
  assert(title != NULL || title_length == 0);
  
  if(hc->num_failed_open_links > 0)
    return;
    
  if(title_length < 0) {
    size_t ulen = wcslen(title);
    
    if(ulen >= INT_MAX)
      return;
    
    title_length = (int)ulen;
  }
  
  link = hc->last_link;
  assert(hc->num_open_links > 0);
  assert(link != NULL);
  
  hyper_console_free_memory(link->title);
  if(title) {
    link->title = allocate_memory((title_length + 1) * sizeof(wchar_t));
    if(link->title) {
      memcpy(
          link->title,
          title,
          title_length * sizeof(wchar_t));
      link->title[title_length] = L'\0';
    }
  }
  else {
    link->title = NULL;
  }
}

static void set_open_link_input_text(struct hyperlink_collection_t *hc, const wchar_t *text, int text_length) {
  struct hyperlink_t *link;
  
  assert(hc != NULL);
  assert(text != NULL || text_length == 0);
  
  if(hc->num_failed_open_links > 0)
    return;
    
  if(text_length < 0) {
    size_t ulen = wcslen(text);
    
    if(ulen >= INT_MAX)
      return;
    
    text_length = (int)ulen;
  }
  
  link = hc->last_link;
  assert(hc->num_open_links > 0);
  assert(link != NULL);
  
  hyper_console_free_memory(link->input_text);
  if(text) {
    link->input_text = allocate_memory((text_length + 1) * sizeof(wchar_t));
    if(link->input_text) {
      memcpy(
          link->input_text,
          text,
          text_length * sizeof(wchar_t));
      link->input_text[text_length] = L'\0';
    }
  }
  else {
    link->input_text = NULL;
  }
}

static WORD set_open_link_color(struct hyperlink_collection_t *hc, WORD attribute) {
  struct hyperlink_t *link;
  WORD old_attr;
  
  assert(hc != NULL);
  
  if(hc->num_failed_open_links > 0)
    return hc->attr_link;
  
  link = hc->last_link;
  assert(hc->num_open_links > 0);
  assert(link != NULL);
  
  old_attr = link->attr_active;
  link->attr_active = (attribute & 0x00FF) | COMMON_LVB_UNDERSCORE;
  
  return old_attr;
}

static BOOL is_global_position_before(int a_line, int a_col, int b_line, int b_col) {
  if(a_line < b_line)
    return TRUE;
    
  if(a_line > b_line)
    return FALSE;
    
  return a_col <= b_col;
}

static struct hyperlink_t *find_link(struct hyperlink_collection_t *hc, COORD pos) {
  int line;
  int column;
  struct hyperlink_t *link;
  
  assert(hc != NULL);
  
  if(!console_scollback_local_to_global(hc->scrollback, pos, &line, &column))
    return FALSE;
    
  for(link = hc->last_link; link != NULL; link = link->prev_link) {
    BOOL start_ok;
    BOOL end_ok;
    
    start_ok = is_global_position_before(link->start_global_line, link->start_column, line, column);
    end_ok = !is_global_position_before(link->end_global_line, link->end_column, line, column);
    
    if(start_ok && end_ok)
      return link;
  }
  
  return NULL;
}

/** Find the local position and length of a link.
  @param hc           The hyperlink collection containing this link.
  @param link         The actual link.
  @param position     Pointer where to store the start coordinate.

  @return The screen-buffer length of the link. Non-positive on error.
 */
static int find_link_visual_position(
    struct hyperlink_collection_t  *hc,
    const struct hyperlink_t       *link,
    COORD                          *position
) {
  COORD start;
  COORD end;
  int length;
  
  assert(hc != NULL);
  assert(link != NULL);
  assert(position != NULL);
  
  if( !console_scollback_global_to_local(hc->scrollback, link->start_global_line, link->start_column, &start) ||
      !console_scollback_global_to_local(hc->scrollback, link->end_global_line,   link->end_column,   &end))
  {
    return 0;
  }
  
  if(start.X >= hc->console_width) {
    start.X = 0;
    start.Y += 1;
  }
  
  if(end.X >= hc->console_width) {
    end.X = 0;
    end.Y += 1;
  }
  
  if(start.Y < 0) {
    start.X = 0;
    start.Y = 0;
  }
  
  length = (1 + end.Y - start.Y) * hc->console_width;
  length -= start.X;
  length -= hc->console_width - end.X;
  
  if(length <= 0)
    return 0;
    
  *position = start;
  return length;
}

static BOOL save_old_console_title(struct hyperlink_collection_t *hc) {
  //int length;
  
  assert(hc != NULL);
  
  GetConsoleTitleW(hc->old_title, ARRAYSIZE(hc->old_title));
  return TRUE;
}

static void set_console_title(struct hyperlink_collection_t *hc, const wchar_t *title) {
  assert(hc != NULL);
  
  if(title)
    SetConsoleTitleW(title);
}

static BOOL invert_link_colors(struct hyperlink_collection_t *hc, const struct hyperlink_t *link) {
  COORD start;
  int length;
  WORD *attributes;
  DWORD num_valid;
  
  assert(hc != NULL);
  assert(link != NULL);
  
  length = find_link_visual_position(hc, link, &start);
  if(length <= 0)
    return FALSE;
    
  attributes = allocate_memory(length * sizeof(WORD));
  if(attributes == NULL)
    return FALSE;
    
  if(console_read_output_attribute(hc->output_handle, attributes, length, start, &num_valid)) {
    WORD *att;
    
    for(att = attributes; att != attributes + length; ++att) {
      *att = (*att & 0xFF00) | ((*att & 0x00F0) >> 4) | ((*att & 0x000F) << 4);
    }
    
    if(WriteConsoleOutputAttribute(hc->output_handle, attributes, length, start, &num_valid)) {
      hyper_console_free_memory(attributes);
      return TRUE;
    }
  }
  
  hyper_console_free_memory(attributes);
  return FALSE;
}

static BOOL activate_link(struct hyperlink_collection_t *hc, struct hyperlink_t *link) {
  COORD start;
  int length;
  DWORD num_valid;
  WORD *new_attributes;
  
  assert(hc != NULL);
  assert(link != NULL);
  
  length = find_link_visual_position(hc, link, &start);
  if(length <= 0)
    return FALSE;
    
  link->inactive_attribute_count = 0;
  hyper_console_free_memory(link->inactive_attributes);
  
  link->inactive_attributes = allocate_memory(length * sizeof(WORD));
  if(link->inactive_attributes == NULL)
    return TRUE;
    
  if(console_read_output_attribute(hc->output_handle, link->inactive_attributes, length, start, &num_valid)) {
    link->inactive_attribute_count = length;
    
    new_attributes = allocate_memory(length * sizeof(WORD));
    if(new_attributes != NULL) {
      int i;
      for(i = 0; i < length; ++i)
        new_attributes[i] = link->attr_active;
        
      WriteConsoleOutputAttribute(hc->output_handle, new_attributes, length, start, &num_valid);
      
      hyper_console_free_memory(new_attributes);
    }
    
    return TRUE;
  }
  
  return FALSE;
}

static void deactivate_link(struct hyperlink_collection_t *hc, struct hyperlink_t *link) {
  COORD start;
  int length;
  DWORD num_valid;
  
  assert(hc != NULL);
  assert(link != NULL);
  
  length = find_link_visual_position(hc, link, &start);
  if(length <= 0)
    return;
    
  if(link->inactive_attributes == NULL || length != link->inactive_attribute_count)
    return;
    
  WriteConsoleOutputAttribute(hc->output_handle, link->inactive_attributes, length, start, &num_valid);
}

static void activate_all_links(struct hyperlink_collection_t *hc) {
  struct hyperlink_t **link_ptr;
  int skip_count;
  
  assert(hc);
  assert(hc->mouse_over_link == NULL);
  assert(hc->pressed_link == NULL);
  
  skip_count = hc->num_open_links;
  link_ptr = &hc->last_link;
  while(skip_count-- > 0) {
    assert(*link_ptr != NULL);
    
    link_ptr = &(*link_ptr)->prev_link;
  }
  
  while(*link_ptr) {
    if(activate_link(hc, *link_ptr)) {
      link_ptr = &(*link_ptr)->prev_link;
    }
    else {
      free_link_at(link_ptr);
    }
  }
}

static void deactivate_all_links(struct hyperlink_collection_t *hc) {
  struct hyperlink_t *link;
  int skip_count;
  
  assert(hc);
  
  skip_count = hc->num_open_links;
  link = hc->last_link;
  while(skip_count-- > 0) {
    assert(link != NULL);
    
    link = link->prev_link;
  }
  
  while(link) {
    deactivate_link(hc, link);
    link = link->prev_link;
  }
}


static void on_mouse_enter_link(struct hyperlink_collection_t *hc) {
  assert(hc != NULL);
  assert(hc->mouse_over_link != NULL);
  
  set_console_title(hc, hc->mouse_over_link->title);
  
  invert_link_colors(hc, hc->mouse_over_link);
}

static void on_mouse_leave_link(struct hyperlink_collection_t *hc) {
  assert(hc != NULL);
  assert(hc->mouse_over_link != NULL);
  
  set_console_title(hc, hc->old_title);
  
  invert_link_colors(hc, hc->mouse_over_link);
}

static BOOL set_mouse_over_link(struct hyperlink_collection_t *hc, struct hyperlink_t *link) {
  assert(hc != NULL);
  
  if(link != hc->mouse_over_link) {
    if(hc->mouse_over_link)
      on_mouse_leave_link(hc);
      
    hc->mouse_over_link = link;
    if(hc->mouse_over_link)
      on_mouse_enter_link(hc);
      
    return TRUE;
  }
  
  return FALSE;
}

static BOOL hs_handle_lbutton_down(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er) {
  assert(hc != NULL);
  assert(er != NULL);
  
  if(er->dwControlKeyState & SHIFT_PRESSED)
    return FALSE;
  
  set_mouse_over_link(hc, find_link(hc, er->dwMousePosition));
  
  if(hc->pressed_link && hc->pressed_link != hc->mouse_over_link)
    invert_link_colors(hc, hc->pressed_link);
    
  hc->pressed_link = hc->mouse_over_link;
  if(hc->pressed_link) {
    invert_link_colors(hc, hc->pressed_link);
    
    return TRUE;
  }
  
  return FALSE;
}

static BOOL hs_handle_mouse_up(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er) {
  assert(hc != NULL);
  assert(er != NULL);
  
  if(er->dwButtonState == 0) {
    if(hc->pressed_link) {
      struct hyperlink_t *link = find_link(hc, er->dwMousePosition);
    
      if(link == hc->pressed_link && hc->pressed_link->input_text) {
        if(!stop_current_input(FALSE, hc->pressed_link->input_text)) {
          // beep
        }
      }
      
      invert_link_colors(hc, hc->pressed_link);
      hc->pressed_link = NULL;
      return TRUE;
    }
  }
  
  return FALSE;
}

static BOOL hs_handle_mouse_move(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er) {
  assert(hc != NULL);
  assert(er != NULL);
  
  if(er->dwControlKeyState & SHIFT_PRESSED)
    return FALSE;
  
  if(er->dwButtonState == 0)
    return set_mouse_over_link(hc, find_link(hc, er->dwMousePosition));
  
  if(hc->pressed_link) {
    struct hyperlink_t *link = find_link(hc, er->dwMousePosition);
    
    if(link != hc->pressed_link)
      set_mouse_over_link(hc, NULL);
    else
      set_mouse_over_link(hc, hc->pressed_link);
      
    return TRUE;
  }
  
  return FALSE;
}

static BOOL hs_handle_mouse_event(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er) {
  assert(hc != NULL);
  assert(er != NULL);
  
  switch(er->dwEventFlags) {
    case 0:
      // button press/release
      if(er->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
        return hs_handle_lbutton_down(hc, er);
      }
      else
        return hs_handle_mouse_up(hc, er);
      break;
      
    case MOUSE_MOVED:
      return hs_handle_mouse_move(hc, er);
  }
  
  return FALSE;
}

static BOOL hs_handle_key_event(struct hyperlink_collection_t *hc, const KEY_EVENT_RECORD *er) {
  assert(hc != NULL);
  assert(er != NULL);
  
  set_mouse_over_link(hc, NULL);
  if(hc->pressed_link) {
    invert_link_colors(hc, hc->pressed_link);
    
    hc->pressed_link = NULL;
  }
  
  return FALSE;
}

static BOOL hs_handle_focus_event(struct hyperlink_collection_t *hc, const FOCUS_EVENT_RECORD *er) {
  assert(hc != NULL);
  assert(er != NULL);
  
  if(!er->bSetFocus) {
    set_mouse_over_link(hc, NULL);
    if(hc->pressed_link) {
      invert_link_colors(hc, hc->pressed_link);
      
      hc->pressed_link = NULL;
    }
    
    return FALSE;
  }
  
  return FALSE;
}

static BOOL hs_handle_events(struct hyperlink_collection_t *hc, INPUT_RECORD *event) {
  HANDLE input_handle;
  
  assert(hc != NULL);
  assert(event != NULL);
  
  input_handle = GetStdHandle(STD_INPUT_HANDLE);
  
  for(;;) {
    DWORD num_read;
    
    switch(event->EventType) {
      case KEY_EVENT:
        if(!hs_handle_key_event(hc, &event->Event.KeyEvent))
          return FALSE;
        break;
        
      case MOUSE_EVENT:
        if(!hs_handle_mouse_event(hc, &event->Event.MouseEvent))
          return FALSE;
        break;
        
      case FOCUS_EVENT:
        if(!hs_handle_focus_event(hc, &event->Event.FocusEvent))
          return FALSE;
        break;
        
      default:
        return FALSE;
    }
    
    if(!hc->pressed_link)
      return TRUE;
      
    if(!ReadConsoleInputW(input_handle, event, 1, &num_read) || num_read < 1)
      return TRUE;
  };
}

static void hs_start_input(struct hyperlink_collection_t *hc, int console_width, int pre_input_lines) {
  assert(hc != NULL);
  assert(console_width > 0);
  
  if(pre_input_lines < 0)
    pre_input_lines = 0;
  
  /* TODO: read_input should be able to state that it knows, that the local line pre_input_lines
     is some particular global line, so we don't whipe out all links in case the old_lines are not 
     fully visible any more.
     (That happens on Windows < 10 when first reducing the console width, thus cutting off lines, 
     and then increasing it again, because we do not cut of the old_lines in order to 
     allow detecting line-unwrapping in Windows 10)
     
     For non-wrapping consoles, not calling console_scrollback_update() here would be best.
   */
  console_scrollback_update(hc->scrollback, pre_input_lines);
  
  hc->console_width = console_width;
  
  hc->mouse_over_link = NULL;
  hc->pressed_link = NULL;
  
  activate_all_links(hc);
  
  save_old_console_title(hc);
}

static void hs_end_input(struct hyperlink_collection_t *hc) {
  assert(hc != NULL);
  
  deactivate_all_links(hc);
  
  hc->mouse_over_link = NULL;
  hc->pressed_link = NULL;
  
  set_console_title(hc, hc->old_title);
}

/*
static void hs_print_debug_info(struct hyperlink_collection_t *hc) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  COORD pos;
  int link_count = 0;
  
  assert(hc != NULL);
  
  if(!GetConsoleScreenBufferInfo(hc->output_handle, &csbi)) {
    fprintf(stderr, "GetConsoleScreenBufferInfo failed\n");
    return;
  }
  
  struct hyperlink_t *link = hc->last_link;
  while(link) {
    ++link_count;
    link = link->prev_link;
  }
  
  fprintf(stderr, "Number of hyperlinks: %d\n",
      link_count);
  
  fprintf(stderr, "Screen buffer %d lines x %d columns\n",
      (int)csbi.dwSize.Y, 
      (int)csbi.dwSize.X);
  fprintf(stderr, "Window at %d:%d size %d lines x %d columns\n",
      (int)csbi.srWindow.Top, 
      (int)csbi.srWindow.Left,
      (int)csbi.srWindow.Bottom - csbi.srWindow.Top + 1,
      (int)csbi.srWindow.Right - csbi.srWindow.Left + 1);
  fprintf(stderr, "Cursor at %d:%d\n",
      (int)csbi.dwCursorPosition.Y,
      (int)csbi.dwCursorPosition.X);
  
  pos = csbi.dwCursorPosition;
  pos.X = 0;
  while(pos.Y >= 0) {
    int line;
    int column;
    
    if(console_scollback_local_to_global(hc->scrollback, pos, &line, &column)) {
      fprintf(stderr, "Last known global position is %d:%d (local %d:%d)\n",
          line,
          column,
          (int)pos.Y,
          (int)pos.X);
      break;
    }
    
    --pos.Y;
  }
  
  if(pos.Y < 0) {
    fprintf(stderr, "No known global position\n");
  }
}
*/

static BOOL _have_hyperlink_system = FALSE;
struct hyperlink_collection_t _global_links[1];
static CRITICAL_SECTION _cs_global_links[1];

HYPER_CONSOLE_API
void hyper_console_start_link(const wchar_t *title) {
  assert(_have_hyperlink_system);
  
  EnterCriticalSection(_cs_global_links);
  
  open_new_link(_global_links);
  set_open_link_title(_global_links, title, -1);
  
  LeaveCriticalSection(_cs_global_links);
}

HYPER_CONSOLE_API
void hyper_console_end_link(void) {
  assert(_have_hyperlink_system);
  
  EnterCriticalSection(_cs_global_links);
  
  close_link(_global_links);
  
  LeaveCriticalSection(_cs_global_links);
}

HYPER_CONSOLE_API
void hyper_console_set_link_input_text(const wchar_t *text) {
  assert(_have_hyperlink_system);
  
  EnterCriticalSection(_cs_global_links);
  
  set_open_link_input_text(_global_links, text, text ? -1 : 0);
  
  LeaveCriticalSection(_cs_global_links);
}

HYPER_CONSOLE_API
WORD hyper_console_set_link_color(WORD attribute) {
  WORD old_color;
  
  assert(_have_hyperlink_system);
  
  EnterCriticalSection(_cs_global_links);
  
  old_color = set_open_link_color(_global_links, attribute);
  
  LeaveCriticalSection(_cs_global_links);
  
  return old_color;
}

BOOL hyperlink_system_handle_events(INPUT_RECORD *event) {
  BOOL handled;
  
  if(!_have_hyperlink_system)
    return FALSE;
  
  EnterCriticalSection(_cs_global_links);
  
  handled = hs_handle_events(_global_links, event);
  
  LeaveCriticalSection(_cs_global_links);
  
  return handled;
}

void hyperlink_system_start_input(int console_width, int pre_input_lines) {
  if(!_have_hyperlink_system)
    return;
    
  EnterCriticalSection(_cs_global_links);
  
  hs_start_input(_global_links, console_width, pre_input_lines);
  
  LeaveCriticalSection(_cs_global_links);
}

void hyperlink_system_end_input(void) {
  if(!_have_hyperlink_system)
    return;
    
  EnterCriticalSection(_cs_global_links);
  
  hs_end_input(_global_links);
  
  LeaveCriticalSection(_cs_global_links);
}

/*
void hyperlink_system_print_debug_info(void) {
  if(!_have_hyperlink_system)
    return;
    
  EnterCriticalSection(_cs_global_links);
  
  hs_print_debug_info(_global_links);
  
  LeaveCriticalSection(_cs_global_links);
}
*/

HYPER_CONSOLE_API
void hyper_console_init_hyperlink_system(void) {
  assert(!_have_hyperlink_system);
  
  InitializeCriticalSection(_cs_global_links);
  init_hyperlink_collection(_global_links);
  
  _have_hyperlink_system = TRUE;
}

HYPER_CONSOLE_API
void hyper_console_done_hyperlink_system(void) {
  assert(_have_hyperlink_system);
  
  _have_hyperlink_system = FALSE;
  
  free_hyperlink_collection(_global_links);
  DeleteCriticalSection(_cs_global_links);
}
