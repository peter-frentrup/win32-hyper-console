#define UNICODE

#include "read-input.h"
#include "hyperlink-output.h"

#include <assert.h>
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
  
  WORD attr_previous;
};

struct hyperlink_collection_t {
  HANDLE output_handle;
  
  COORD console_size;
  
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

static int get_line_canary(struct hyperlink_collection_t *hc, int line);
static BOOL put_line_canary(struct hyperlink_collection_t *hc, int buffer_line, int canary);
static int guess_global_line_number(struct hyperlink_collection_t *hc, COORD pos);
static BOOL store_global_line_number(struct hyperlink_collection_t *hc, COORD pos, int global_line_number);

static void free_link_at(struct hyperlink_t **last_link);
static void clean_old_links(struct hyperlink_collection_t *hc, int first_keep_line);
static struct hyperlink_t *open_new_link(struct hyperlink_collection_t *hc);
static void close_link(struct hyperlink_collection_t *hc);
static void set_open_link_title(struct hyperlink_collection_t *hc, const wchar_t *title, int title_length);
static void set_open_link_input_text(struct hyperlink_collection_t *hc, const wchar_t *text, int text_length);

static BOOL save_old_console_title(struct hyperlink_collection_t *hc);
static void set_console_title(struct hyperlink_collection_t *hc, const wchar_t *title);

static BOOL invert_link_colors(struct hyperlink_collection_t *hc, const struct hyperlink_t *link);

static BOOL is_global_position_before(int a_line, int a_col, int b_line, int b_col);
static struct hyperlink_t *find_link(struct hyperlink_collection_t *hc, COORD pos);

static void on_mouse_enter_link(struct hyperlink_collection_t *hc);
static void on_mouse_leave_link(struct hyperlink_collection_t *hc);
static BOOL set_mouse_over_link(struct hyperlink_collection_t *hc, struct hyperlink_t *link);

static BOOL hs_handle_lbutton_down(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er);
static BOOL hs_handle_mouse_up(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er);
static BOOL hs_handle_mouse_move(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er);
static BOOL hs_handle_mouse_event(struct hyperlink_collection_t *hc, const MOUSE_EVENT_RECORD *er);

static BOOL hs_handle_key_event(struct hyperlink_collection_t *hc, const KEY_EVENT_RECORD *er);
static BOOL hs_handle_focus_event(struct hyperlink_collection_t *hc, const FOCUS_EVENT_RECORD *er);


static void init_hyperlink_collection(struct hyperlink_collection_t *hc) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(hc != NULL);
  
  memset(hc, 0, sizeof(struct hyperlink_collection_t));
  
  hc->output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  
  hc->console_size.X = 1;
  hc->console_size.Y = 1;
  
  hc->attr_default = 0x0F;
  if(GetConsoleScreenBufferInfo(hc->output_handle, &csbi)) {
    hc->attr_default = csbi.wAttributes;
    hc->console_size = csbi.dwSize;
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
}

static void free_hyperlink_collection(struct hyperlink_collection_t *hc) {
  assert(hc != NULL);
  
  if(hc->mouse_over_link)
    on_mouse_leave_link(hc);
    
  // hc->selected_link
  
  while(hc->last_link)
    free_link_at(&hc->last_link);
    
  //free(hc->old_title);
  memset(hc, 0, sizeof(struct hyperlink_collection_t));
}

/* To detect buffer scrolling, we store some kind of global line count ("canary") in the
   character attributes of a line.
   We therefore misuse the high-order attribute bytes of the first LINE_CANARY_SIZE many characters
   in a line.
   These reserved for COMMON_LVB_XXX attributes, which only function with DBCS and seem to have no
   effect otherwise (only until Win10 though).

   The assumption here is that 1) these bytes are normally unset and 2) no one else needs them.
   
   BUG: This does not work on Windows 10, because the COMMON_LVB_XXX attributes are used there.
   
   TODO: find another way to store line canary.
 */
static int get_line_canary(struct hyperlink_collection_t *hc, int buffer_line) {
  COORD pos;
  DWORD num_read;
  
  WORD attributes[LINE_CANARY_SIZE] = {0};
  
  assert(hc != NULL);
  assert(buffer_line >= 0);
  
  pos.X = 0;
  pos.Y = buffer_line;
  num_read = 0;
  if(ReadConsoleOutputAttribute(hc->output_handle, attributes, LINE_CANARY_SIZE, pos, &num_read)) {
    WORD *att;
    int canary = 0;
    
    for(att = attributes; att != attributes + LINE_CANARY_SIZE; ++att) {
      canary = (canary << 8) | HIBYTE(*att);
    }
    
    return canary;
  }
  
  return 0;
}

static BOOL put_line_canary(struct hyperlink_collection_t *hc, int buffer_line, int canary) {
  COORD pos;
  DWORD num_valid;
  WORD attributes[LINE_CANARY_SIZE] = {0};
  
  assert(hc != NULL);
  assert(buffer_line >= 0);
  
  pos.X = 0;
  pos.Y = buffer_line;
  num_valid = 0;
  if(ReadConsoleOutputAttribute(hc->output_handle, attributes, LINE_CANARY_SIZE, pos, &num_valid)) {
    WORD *att = attributes + LINE_CANARY_SIZE;
    
    while(att != attributes) {
      --att;
      
      *att = LOBYTE(*att) | ((canary & 0xFF) << 8);
      canary = canary >> 8;
    }
    
    if(WriteConsoleOutputAttribute(hc->output_handle, attributes, LINE_CANARY_SIZE, pos, &num_valid)) {
      return TRUE;
    }
  }
  
  return FALSE;
}

static int guess_global_line_number(struct hyperlink_collection_t *hc, COORD pos) {
  int canary;
  int test_line_offset;
  
  assert(hc != NULL);
  
  if(pos.Y < 0)
    return 0;
    
  canary = 0;
  test_line_offset = 0;
  if(pos.X >= LINE_CANARY_SIZE)
    canary = get_line_canary(hc, pos.Y);
    
  while(test_line_offset < pos.Y && !canary) {
    ++test_line_offset;
    canary = get_line_canary(hc, pos.Y - test_line_offset);
  }
  
  if(canary <= 0) {
    if(hc->last_link)
      return hc->last_link->end_global_line + 1 + test_line_offset;
      
    return 1 + test_line_offset;
  }
  
  return canary + test_line_offset;
}

static BOOL store_global_line_number(struct hyperlink_collection_t *hc, COORD pos, int global_line_number) {
  assert(hc != NULL);
  
  if(pos.Y < 0)
    return FALSE;
    
  if(global_line_number <= pos.Y)
    return FALSE;
    
  if(pos.X >= LINE_CANARY_SIZE) {
    if(put_line_canary(hc, pos.Y, global_line_number))
      return TRUE;
  }
  
  while(pos.Y > 0) {
    --pos.Y;
    --global_line_number;
    if(put_line_canary(hc, pos.Y, global_line_number))
      return TRUE;
  }
  
  return FALSE;
}

static void free_link_at(struct hyperlink_t **last_link) {
  struct hyperlink_t *link;
  
  assert(last_link != NULL);
  
  link = *last_link;
  if(!link)
    return;
    
  *last_link = link->prev_link;
  
  free(link->title);
  free(link->input_text);
  free(link);
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
    
    free_link_at(link_ptr);
  }
}

static struct hyperlink_t *open_new_link(struct hyperlink_collection_t *hc) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  struct hyperlink_t *link;
  int global_line;
  
  assert(hc != NULL);
  
  if(hc->num_failed_open_links > 0) {
    hc->num_failed_open_links++;
    return NULL;
  }
  
  if(!GetConsoleScreenBufferInfo(hc->output_handle, &csbi)) {
    hc->num_failed_open_links++;
    return NULL;
  }
  
  global_line = guess_global_line_number(hc, csbi.dwCursorPosition);
  if(global_line < 0) {
    hc->num_failed_open_links++;
    return NULL;
  }
  
  clean_old_links(hc, global_line - csbi.dwCursorPosition.Y);
  
  if(!store_global_line_number(hc, csbi.dwCursorPosition, global_line)) {
    hc->num_failed_open_links++;
    return NULL;
  }
  
  link = malloc(sizeof(struct hyperlink_t));
  if(!link) {
    hc->num_failed_open_links++;
    return NULL;
  }
  
  memset(link, 0, sizeof(struct hyperlink_t));
  
  link->start_column      = link->end_column      = csbi.dwCursorPosition.X;
  link->start_global_line = link->end_global_line = global_line;
  
  link->attr_previous = csbi.wAttributes;
  SetConsoleTextAttribute(hc->output_handle, hc->attr_link);
  
  link->prev_link = hc->last_link;
  hc->last_link = link;
  hc->num_open_links++;
  return link;
}

static void close_link(struct hyperlink_collection_t *hc) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  struct hyperlink_t *link;
  
  assert(hc != NULL);
  
  if(hc->num_failed_open_links > 0) {
    hc->num_failed_open_links--;
    return;
  }
  
  link = hc->last_link;
  
  assert(hc->num_open_links > 0);
  assert(link != NULL);
  
  hc->num_open_links--;
  if(GetConsoleScreenBufferInfo(hc->output_handle, &csbi)) {
    int global_line = guess_global_line_number(hc, csbi.dwCursorPosition);
    
    if(global_line < link->start_global_line)
      return;
      
    if(global_line == link->start_global_line && csbi.dwCursorPosition.X < link->start_column)
      return;
      
    store_global_line_number(hc, csbi.dwCursorPosition, global_line);
    
    link->end_global_line = global_line;
    link->end_column = csbi.dwCursorPosition.X;
    
    SetConsoleTextAttribute(hc->output_handle, link->attr_previous);
  }
}

static void set_open_link_title(struct hyperlink_collection_t *hc, const wchar_t *title, int title_length) {
  struct hyperlink_t *link;
  
  assert(hc != NULL);
  assert(title != NULL || title_length == 0);
  
  if(hc->num_failed_open_links > 0)
    return;
    
  if(title_length < 0) {
    title_length = wcslen(title);
    
    if(title_length < 0)
      return;
  }
  
  link = hc->last_link;
  assert(hc->num_open_links > 0);
  assert(link != NULL);
  
  free(link->title);
  if(title) {
    link->title = malloc((title_length + 1) * sizeof(wchar_t));
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
    text_length = wcslen(text);
    
    if(text_length < 0)
      return;
  }
  
  link = hc->last_link;
  assert(hc->num_open_links > 0);
  assert(link != NULL);
  
  free(link->input_text);
  if(text) {
    link->input_text = malloc((text_length + 1) * sizeof(wchar_t));
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

static BOOL save_old_console_title(struct hyperlink_collection_t *hc) {
  //int length;
  
  assert(hc != NULL);
  
  //free(hc->old_title);
  //hc->old_title = NULL;
  //
  //length = GetConsoleTitleW(NULL, 0);
  //if(length < 0)
  //  return FALSE;
  //
  //hc->old_title = malloc((length + 1) * sizeof(wchar_t));
  //if(!hc->old_title)
  //  return FALSE;
  //
  //hc->old_title[0] = L'\0';
  //GetConsoleTitleW(hc->old_title, length);
  //return TRUE;
  
  GetConsoleTitleW(hc->old_title, ARRAYSIZE(hc->old_title));
  return TRUE;
}

static void set_console_title(struct hyperlink_collection_t *hc, const wchar_t *title) {
  assert(hc != NULL);
  
  if(title)
    SetConsoleTitleW(title);
}

static BOOL invert_link_colors(struct hyperlink_collection_t *hc, const struct hyperlink_t *link) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  int top_global_line;
  COORD pos;
  DWORD length;
  WORD *attributes;
  DWORD num_valid;
  
  assert(hc != NULL);
  assert(link != NULL);
  
  if(!GetConsoleScreenBufferInfo(hc->output_handle, &csbi))
    return FALSE;
    
  pos = csbi.dwCursorPosition;
  top_global_line = guess_global_line_number(hc, pos);
  if(top_global_line <= 0)
    return FALSE;
    
  top_global_line -= pos.Y;
  
  length = (1 + link->end_global_line - link->start_global_line) * hc->console_size.X;
  length -= link->start_column;
  length -= hc->console_size.X - link->end_column;
  
  pos.X = link->start_column;
  pos.Y = link->start_global_line - top_global_line;
  
  if(pos.Y < 0) {
    length += link->start_column;
    length += pos.Y * hc->console_size.X;
    
    pos.X = 0;
    pos.Y = 0;
  }
  
  if(length <= 0)
    return FALSE;
    
  attributes = malloc(length * sizeof(WORD));
  if(attributes == NULL)
    return FALSE;
    
  if(ReadConsoleOutputAttribute(hc->output_handle, attributes, length, pos, &num_valid)) {
    WORD *att;
    
    for(att = attributes; att != attributes + length; ++att) {
      *att = (*att & 0xFF00) | ((*att & 0x00F0) >> 4) | ((*att & 0x000F) << 4);
    }
    
    if(WriteConsoleOutputAttribute(hc->output_handle, attributes, length, pos, &num_valid))
      return TRUE;
  }
  
  return FALSE;
}

static BOOL is_global_position_before(int a_line, int a_col, int b_line, int b_col) {
  if(a_line < b_line)
    return TRUE;
    
  if(a_line > b_line)
    return FALSE;
    
  return a_col <= b_col;
}

static struct hyperlink_t *find_link(struct hyperlink_collection_t *hc, COORD pos) {
  int global_line;
  struct hyperlink_t *link;
  
  assert(hc != NULL);
  
  global_line = guess_global_line_number(hc, pos);
  if(global_line <= 0)
    return NULL;
    
  for(link = hc->last_link; link != NULL; link = link->prev_link) {
    BOOL start_ok;
    BOOL end_ok;
    
    start_ok = is_global_position_before(link->start_global_line, link->start_column, global_line, pos.X);
    end_ok = !is_global_position_before(link->end_global_line, link->end_column, global_line, pos.X);
    
    if(start_ok && end_ok)
      return link;
  }
  
  return NULL;
}

static void on_mouse_enter_link(struct hyperlink_collection_t *hc) {
  assert(hc != NULL);
  assert(hc->mouse_over_link != NULL);
  
  if(save_old_console_title(hc))
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
      if(hc->pressed_link->input_text) {
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
  
  if(er->dwButtonState == 0) {
    return set_mouse_over_link(hc, find_link(hc, er->dwMousePosition));
  }
  else if(hc->pressed_link) {
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

static BOOL _have_hyperlink_system = FALSE;
struct hyperlink_collection_t _global_links[1];
static CRITICAL_SECTION _cs_global_links[1];

void start_hyperlink(const wchar_t *title) {
  assert(_have_hyperlink_system);
  
  EnterCriticalSection(_cs_global_links);
  
  open_new_link(_global_links);
  set_open_link_title(_global_links, title, -1);
  
  LeaveCriticalSection(_cs_global_links);
}

void end_hyperlink(void) {
  assert(_have_hyperlink_system);
  
  EnterCriticalSection(_cs_global_links);
  
  close_link(_global_links);
  
  LeaveCriticalSection(_cs_global_links);
}

void set_hyperlink_input_text(const wchar_t *text) {
  assert(_have_hyperlink_system);
  
  EnterCriticalSection(_cs_global_links);
  
  set_open_link_input_text(_global_links, text, text ? -1 : 0);
  
  LeaveCriticalSection(_cs_global_links);
}

BOOL hyperlink_system_handle_mouse_event(const MOUSE_EVENT_RECORD *er) {
  BOOL handled;
  
  if(!_have_hyperlink_system)
    return FALSE;
    
  EnterCriticalSection(_cs_global_links);
  
  handled = hs_handle_mouse_event(_global_links, er);
  
  LeaveCriticalSection(_cs_global_links);
  
  return handled;
}

BOOL hyperlink_system_handle_key_event(const KEY_EVENT_RECORD *er) {
  BOOL handled;
  
  if(!_have_hyperlink_system)
    return FALSE;
    
  EnterCriticalSection(_cs_global_links);
  
  handled = hs_handle_key_event(_global_links, er);
  
  LeaveCriticalSection(_cs_global_links);
  
  return handled;
}

BOOL hyperlink_system_handle_focus_event(const FOCUS_EVENT_RECORD *er) {
  BOOL handled;
  
  if(!_have_hyperlink_system)
    return FALSE;
    
  EnterCriticalSection(_cs_global_links);
  
  handled = hs_handle_focus_event(_global_links, er);
  
  LeaveCriticalSection(_cs_global_links);
  
  return handled;
}

void init_hyperlink_system(void) {
  assert(!_have_hyperlink_system);
  
  InitializeCriticalSection(_cs_global_links);
  init_hyperlink_collection(_global_links);
  
  _have_hyperlink_system = TRUE;
}

void done_hyperlink_system(void) {
  assert(_have_hyperlink_system);
  
  _have_hyperlink_system = FALSE;
  
  free_hyperlink_collection(_global_links);
  DeleteCriticalSection(_cs_global_links);
}
