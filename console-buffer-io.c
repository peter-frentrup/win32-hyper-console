#include "console-buffer-io.h"
#include "debug.h"
#include "memory-util.h"
#include "text-util.h"

#include <assert.h>
#include <strsafe.h>


#define MAX_BUFFER 8192


#define MIN(A, B)  ((A) < (B) ? (A) : (B))
#define MAX(A, B)  ((A) > (B) ? (A) : (B))


struct send_input_t {
  HANDLE input_handle;
  
  INPUT_RECORD input_records[128];
  DWORD counter;
};

static BOOL flush_input(struct send_input_t *context);
static BOOL send_input(struct send_input_t *context, wchar_t ch);


BOOL console_read_output_character(
    HANDLE hConsoleOutput,
    LPWSTR lpCharacter,
    DWORD nLength,
    COORD dwReadCoord,
    LPDWORD lpNumberOfCharsRead
) {
  DWORD chars_read = 0;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(lpCharacter != NULL);
  assert(lpNumberOfCharsRead != NULL);
  assert(dwReadCoord.X >= 0);
  assert(dwReadCoord.Y >= 0);
  
  *lpNumberOfCharsRead = 0;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi) || csbi.dwSize.X <= 0) {
    debug_printf(L"console_read_output_character: GetConsoleScreenBufferInfo");
    return FALSE;
  }
  
  while(nLength > MAX_BUFFER) {
    int x;
    
    if(!ReadConsoleOutputCharacterW(hConsoleOutput, lpCharacter, MAX_BUFFER, dwReadCoord, &chars_read)) {
      debug_printf(L"console_read_output_character: ReadConsoleOutputCharacterW");
      return FALSE;
    }
    
    *lpNumberOfCharsRead += chars_read;
    lpCharacter += chars_read;
    nLength -= chars_read;
    x = dwReadCoord.X + chars_read;
    
    dwReadCoord.Y += x / csbi.dwSize.X;
    dwReadCoord.X = x % csbi.dwSize.X;
    
    if(dwReadCoord.Y >= csbi.dwSize.Y)
      return FALSE;
  }
  
  if(nLength > 0) {
    if(!ReadConsoleOutputCharacterW(hConsoleOutput, lpCharacter, nLength, dwReadCoord, &chars_read)) {
      debug_printf(L"console_read_output_character: ReadConsoleOutputCharacterW 2");
      return FALSE;
    }
    
    *lpNumberOfCharsRead += chars_read;
  }
  
  return TRUE;
}

/* Signature of [Read|Write]ConsoleOutputAttribute, ignoring const. */
typedef BOOL (__stdcall *attribute_rw_func_t)( HANDLE, WORD*, DWORD, COORD, LPDWORD);

static BOOL touch_output_attribute(
    HANDLE hConsoleOutput,
    LPWORD lpAttribute,
    DWORD nLength,
    COORD dwReadCoord,
    LPDWORD lpNumberOfAttrsTouched,
    attribute_rw_func_t read_or_write_func
) {
  DWORD attrs_touched = 0;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(read_or_write_func != NULL);
  assert(lpAttribute != NULL);
  assert(lpNumberOfAttrsTouched != NULL);
  assert(dwReadCoord.X >= 0);
  assert(dwReadCoord.Y >= 0);
  
  *lpNumberOfAttrsTouched = 0;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi) || csbi.dwSize.X <= 0) {
    debug_printf(L"console_read_output_attribute: GetConsoleScreenBufferInfo");
    return FALSE;
  }
  
  while(nLength > MAX_BUFFER) {
    int x;
    
    if(!read_or_write_func(hConsoleOutput, lpAttribute, MAX_BUFFER, dwReadCoord, &attrs_touched)) {
      debug_printf(L"console_read_output_attribute: read_or_write_func");
      return FALSE;
    }
    
    *lpNumberOfAttrsTouched += attrs_touched;
    lpAttribute += attrs_touched;
    nLength -= attrs_touched;
    x = dwReadCoord.X + attrs_touched;
    
    dwReadCoord.Y += x / csbi.dwSize.X;
    dwReadCoord.X = x % csbi.dwSize.X;
    
    if(dwReadCoord.Y >= csbi.dwSize.Y)
      return FALSE;
  }
  
  if(nLength > 0) {
    if(!read_or_write_func(hConsoleOutput, lpAttribute, nLength, dwReadCoord, &attrs_touched)) {
      debug_printf(L"console_read_output_attribute: read_or_write_func 2");
      return FALSE;
    }
    
    *lpNumberOfAttrsTouched += attrs_touched;
  }
  
  return TRUE;
}

BOOL console_read_output_attribute(
    HANDLE hConsoleOutput,
    LPWORD lpAttribute,
    DWORD nLength,
    COORD dwReadCoord,
    LPDWORD lpNumberOfAttrsRead
) {
  return touch_output_attribute(
      hConsoleOutput, lpAttribute, nLength, dwReadCoord, lpNumberOfAttrsRead,
      ReadConsoleOutputAttribute);
}

BOOL console_write_output_attribute(
    HANDLE hConsoleOutput,
    const LPWORD lpAttribute,
    DWORD nLength,
    COORD dwWriteCoord,
    LPDWORD lpNumberOfAttrsWritten
) {
  return touch_output_attribute(
      hConsoleOutput, (LPWORD)lpAttribute, nLength, dwWriteCoord, lpNumberOfAttrsWritten,
      (attribute_rw_func_t)WriteConsoleOutputAttribute);
}

static void invert_color_attributes(
    WORD *attributes,
    size_t count
) {
  while(count-- > 0) {
    WORD fg = *attributes & 0x000F;
    WORD bg = (*attributes & 0x00F0) >> 4;
    WORD rest = *attributes & 0xFF00;
    
    *attributes++ = rest | (fg << 4) | bg;
  }
}

static COORD console_output_invert_colors_helper(
    HANDLE hConsoleOutput,
    COORD console_size,
    COORD start,
    int length,
    WORD *attribute_buffer
) {
  DWORD attrs_read;
  DWORD attrs_written;
  int x;
  
  if(length <= 0)
    return start;
    
  if(!ReadConsoleOutputAttribute(hConsoleOutput, attribute_buffer, length, start, &attrs_read)) {
    debug_printf(L"invert_output_color_attributes: ReadConsoleOutputAttribute");
    return start;
  }
  
  invert_color_attributes(attribute_buffer, length);
  
  if(!WriteConsoleOutputAttribute(hConsoleOutput, attribute_buffer, length, start, &attrs_written)) {
    debug_printf(L"invert_output_color_attributes: WriteConsoleOutputAttribute");
    return start;
  }
  
  x = start.X + attrs_read;
  
  start.Y += x / console_size.X;
  start.X = x % console_size.X;
  
  return start;
}

static void invert_colors_start_end(
    HANDLE hConsoleOutput,
    COORD  console_size,
    COORD  start,
    COORD  end
) {
  WORD attributes[MAX_BUFFER];
  int start_index;
  int end_index;
  int length;
  
  if(console_size.X <= 0)
    return;
    
  start_index = start.Y * console_size.X + start.X;
  end_index   = end.Y * console_size.X + end.X;
  
  if(start.X == console_size.X) {
    start.Y++;
    start.X = 0;
  }
  
  length = end_index - start_index;
  assert(length >= 0);
  
  while(length > MAX_BUFFER) {
    start = console_output_invert_colors_helper(
        hConsoleOutput,
        console_size,
        start,
        MAX_BUFFER,
        attributes);
        
    length -= MAX_BUFFER;
  }
  
  console_output_invert_colors_helper(
      hConsoleOutput,
      console_size,
      start,
      length,
      attributes);
}

static int cmp_coords(const void *coord_ptr_a, const void *coord_ptr_b) {
  COORD a = *(const COORD*)coord_ptr_a;
  COORD b = *(const COORD*)coord_ptr_b;
  
  if(a.Y < b.Y)
    return -1;
    
  if(a.Y > b.Y)
    return +1;
    
  if(a.X < b.X)
    return -1;
    
  if(a.X > b.X)
    return +1;
    
  return 0;
}

void console_reinvert_colors(
    HANDLE hConsoleOutput,
    COORD old_start,
    COORD old_end,
    COORD new_start,
    COORD new_end
) {
  COORD coords[4];
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi) || csbi.dwSize.X <= 0) {
    debug_printf(L"console_invert_output_color: GetConsoleScreenBufferInfo");
    return;
  }
  
  coords[0] = old_start;
  coords[1] = old_end;
  coords[2] = new_start;
  coords[3] = new_end;
  
  qsort(coords, 4, sizeof(COORD), cmp_coords);
  
  invert_colors_start_end(hConsoleOutput, csbi.dwSize, coords[0], coords[1]);
  invert_colors_start_end(hConsoleOutput, csbi.dwSize, coords[2], coords[3]);
}

static void invert_rect(HANDLE hConsoleOutput, const SMALL_RECT *rect) {
  COORD start;
  COORD end;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi) || csbi.dwSize.X <= 0) {
    debug_printf(L"invert_rect: GetConsoleScreenBufferInfo");
    return;
  }
  
  start.X = rect->Left;
  end.X = rect->Right;
  
  for(start.Y = rect->Top; start.Y < rect->Bottom; start.Y++) {
    end.Y = start.Y;
    
    invert_colors_start_end(hConsoleOutput, csbi.dwSize, start, end);
  }
}

static int cmp_short(const void *short_ptr_a, const void *short_ptr_b) {
  SHORT a = *(SHORT*)short_ptr_a;
  SHORT b = *(SHORT*)short_ptr_b;
  
  if(a < b)
    return -1;
  if(a > b)
    return +1;
  return 0;
}

void console_reinvert_colors_rect(
    HANDLE hConsoleOutput,
    const SMALL_RECT *old_rect,
    const SMALL_RECT *new_rect
) {
  SMALL_RECT rect;
  
  assert(old_rect != NULL);
  assert(new_rect != NULL);
  assert(old_rect->Left <= old_rect->Right);
  assert(old_rect->Top <= old_rect->Bottom);
  assert(new_rect->Left <= new_rect->Right);
  assert(new_rect->Top <= new_rect->Bottom);
  
  //invert_rect(hConsoleOutput, old_rect);
  //invert_rect(hConsoleOutput, new_rect);
  //return;
  
  /* Upper part */
  if(new_rect->Top <= old_rect->Top) {
    rect = *new_rect;
    rect.Bottom = MIN(new_rect->Bottom, old_rect->Top);
    
    invert_rect(hConsoleOutput, &rect);
  }
  else {
    rect = *old_rect;
    rect.Bottom = MIN(old_rect->Bottom, new_rect->Top);
    
    invert_rect(hConsoleOutput, &rect);
  }
  
  /* Lower part */
  if(new_rect->Bottom >= old_rect->Bottom) {
    rect = *new_rect;
    rect.Top = MAX(new_rect->Top, old_rect->Bottom);
    
    invert_rect(hConsoleOutput, &rect);
  }
  else {
    rect = *old_rect;
    rect.Top = MAX(old_rect->Top, new_rect->Bottom);
    
    invert_rect(hConsoleOutput, &rect);
  }
  
  rect.Top    = MAX(old_rect->Top,    new_rect->Top);
  rect.Bottom = MIN(old_rect->Bottom, new_rect->Bottom);
  if(rect.Top <= rect.Bottom) {
    SHORT x_coords[4];
    
    x_coords[0] = old_rect->Left;
    x_coords[1] = old_rect->Right;
    x_coords[2] = new_rect->Left;
    x_coords[3] = new_rect->Right;
    
    qsort(x_coords, 4, sizeof(SHORT), cmp_short);
    
    rect.Left = x_coords[0];
    rect.Right = x_coords[1];
    invert_rect(hConsoleOutput, &rect);
    
    rect.Left = x_coords[2];
    rect.Right = x_coords[3];
    invert_rect(hConsoleOutput, &rect);
  }
}

void console_clean_lines(HANDLE hConsoleOutput, int num_lines) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  COORD pos;
  wchar_t *line_chars;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi) || csbi.dwSize.X <= 0) {
    debug_printf(L"console_clean_lines: GetConsoleScreenBufferInfo");
    return;
  }
  
  if(num_lines > csbi.dwSize.Y)
    num_lines = csbi.dwSize.Y;
    
  line_chars = allocate_memory(sizeof(wchar_t) * csbi.dwSize.X);
  if(!line_chars)
    return;
    
  for(pos.Y = 0; pos.Y < num_lines; pos.Y++) {
    DWORD num_read;
    SHORT x;
    
    pos.X = 0;
    if(!ReadConsoleOutputCharacterW(hConsoleOutput, line_chars, csbi.dwSize.X, pos, &num_read)) {
      debug_printf(L"console_clean_lines: ReadConsoleOutputCharacterW");
      break;
    }
    
    x = csbi.dwSize.X;
    while(x > 0 && line_chars[x - 1] == L' ')
      --x;
      
    if(x < csbi.dwSize.X) {
      DWORD num_write;
      pos.X = x;
      if(!FillConsoleOutputAttribute(hConsoleOutput, csbi.wAttributes, csbi.dwSize.X - pos.X, pos, &num_write)) {
        debug_printf(L"console_clean_lines: FillConsoleOutputAttribute");
        break;
      }
    }
  }
  
  free_memory(line_chars);
}

BOOL console_scroll_wheel(HANDLE hConsoleOutput, const MOUSE_EVENT_RECORD *er) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  short scroll_delta;
  int scroll_lines = 3;
  
  assert(er != NULL);
  
  if(er->dwEventFlags == MOUSE_WHEELED) {
    scroll_delta = (short)HIWORD(er->dwButtonState);
    
    SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &scroll_lines, 0);
    scroll_lines = (scroll_lines * scroll_delta) / WHEEL_DELTA;
    
    memset(&csbi, 0, sizeof(csbi));
    if(GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
      csbi.srWindow.Top -= (SHORT)scroll_lines;
      csbi.srWindow.Bottom -= (SHORT)scroll_lines;
      
      if(csbi.srWindow.Top < 0) {
        csbi.srWindow.Bottom += (0 - csbi.srWindow.Top);
        csbi.srWindow.Top = 0;
      }
      else if(csbi.srWindow.Bottom >= csbi.dwSize.Y) {
        csbi.srWindow.Top += (csbi.dwSize.Y - 1 - csbi.srWindow.Bottom);
        csbi.srWindow.Bottom = csbi.dwSize.Y - 1;
      }
      
      SetConsoleWindowInfo(hConsoleOutput, TRUE, &csbi.srWindow);
    }
    
    return TRUE;
  }
  
  return FALSE;
}

BOOL console_scroll_key(HANDLE hConsoleOutput, const KEY_EVENT_RECORD *er) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  SHORT scoll_delta = 0;
  
  assert(er != NULL);
  
  if(!er->bKeyDown) 
    return FALSE;
  
  memset(&csbi, 0, sizeof(csbi));
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) 
    return FALSE;
  
  switch(er->wVirtualKeyCode) {
    case VK_UP:
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        scoll_delta = -1;
      }
      break;
      
    case VK_DOWN:
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        scoll_delta = 1;
      }
      break;
    
    case VK_PRIOR:
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        scoll_delta = -(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
      }
      break;
    
    case VK_NEXT:
      if(er->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        scoll_delta = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
      }
      break;
  }
  
  if(scoll_delta) {
    csbi.srWindow.Top += scoll_delta;
    csbi.srWindow.Bottom += scoll_delta;
    
    if(csbi.srWindow.Top < 0) {
      csbi.srWindow.Bottom += (0 - csbi.srWindow.Top);
      csbi.srWindow.Top = 0;
    }
    else if(csbi.srWindow.Bottom >= csbi.dwSize.Y) {
      csbi.srWindow.Top += (csbi.dwSize.Y - 1 - csbi.srWindow.Bottom);
      csbi.srWindow.Bottom = csbi.dwSize.Y - 1;
    }
    
    SetConsoleWindowInfo(hConsoleOutput, TRUE, &csbi.srWindow);
    return TRUE;
  }
  
  return FALSE;
}


static BOOL flush_input(struct send_input_t *context) {
  DWORD written;
  
  assert(context != NULL);
  
  assert(context->counter >= 0);
  assert(context->counter <= sizeof(context->input_records) / sizeof(INPUT_RECORD));
  
  if(!WriteConsoleInputW(context->input_handle, context->input_records, context->counter, &written)) {
    debug_printf(L"flush_input: WriteConsoleInputW failed\n");
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
  context->input_records[context->counter].Event.KeyEvent.wVirtualScanCode = (WORD)MapVirtualKeyW(LOBYTE(vk_mode), MAPVK_VK_TO_VSC);
  context->counter++;
  
  context->input_records[context->counter] = context->input_records[context->counter - 1];
  context->input_records[context->counter].Event.KeyEvent.bKeyDown = FALSE;
  context->counter++;
  
  return TRUE;
}

void console_paste_from_clipboard(HANDLE hConsoleInput) {
  HGLOBAL copy_handle;
  wchar_t *copy_data;
  
  if(!IsClipboardFormatAvailable(CF_UNICODETEXT))
    return;
    
  if(!OpenClipboard(NULL))
    return;
    
  copy_handle = GetClipboardData(CF_UNICODETEXT);
  if(copy_handle) {
    copy_data = GlobalLock(copy_handle);
    if(copy_data) {
      struct send_input_t context[1];
      
      context->input_handle = hConsoleInput;
      context->counter = 0;
      
      for(; *copy_data; ++copy_data) {
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
    }
    
    GlobalUnlock(copy_handle);
  }
  
  CloseClipboard();
}

BOOL console_get_screen_word_start_end(HANDLE hConsoleOutput, COORD pos, COORD *start, COORD *end) {
  int length;
  int offset;
  wchar_t *screen;
  DWORD num_read;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(start != NULL);
  assert(end != NULL);
  assert(start != end);
  
  assert(pos.X >= 0);
  assert(pos.Y >= 0);
  
  *start = *end = pos;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi))
    return FALSE;
    
  length = csbi.dwSize.X;
  offset = 0;
  start->X = 0;
  start->Y = pos.Y;
  if(start->Y > 0) {
    offset += csbi.dwSize.X;
    length += csbi.dwSize.X;
    start->Y -= 1;
  }
  
  if(start->Y + 1 < csbi.dwSize.Y) {
    length += csbi.dwSize.X;
  }
  
  screen = allocate_memory(sizeof(wchar_t) * length);
  if(!screen)
    return FALSE;
    
  if(console_read_output_character(hConsoleOutput, screen, length, *start, &num_read)) {
    int s = console_get_word_start(screen, length, pos.X + offset);
    int e = console_get_word_end(screen, length, pos.X + offset);
    
    if(s < offset) {
      while(s < offset && iswspace(screen[s]))
        ++s;
    }
    
    if(e > offset + csbi.dwSize.X) {
      while(e > offset + csbi.dwSize.X && iswspace(screen[e - 1]))
        --e;
    }
    
    end->Y = start->Y + e / csbi.dwSize.X;
    end->X = start->X + e % csbi.dwSize.X;
    
    start->Y += s / csbi.dwSize.X;
    start->X += s % csbi.dwSize.X;
    
    free_memory(screen);
    return TRUE;
  }
  
  free_memory(screen);
  return FALSE;
}

void console_alert(HANDLE hConsoleOutput) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
//  Beep(800, 200);
  MessageBeep(0xFFFFFFFFU);
  
  if(GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
    COORD pos;
    DWORD num_read;
    WORD *attr = allocate_memory(csbi.dwSize.X * csbi.dwSize.Y * sizeof(WORD));
    
    pos.X = pos.Y = 0;
    
    if(attr && console_read_output_attribute(hConsoleOutput, attr, csbi.dwSize.X * csbi.dwSize.Y, pos, &num_read)) {
      FillConsoleOutputAttribute(
          hConsoleOutput,
          BACKGROUND_RED | BACKGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
          num_read,
          pos,
          &num_read);
          
      Sleep(200);
      
      console_write_output_attribute(hConsoleOutput, attr, num_read, pos, &num_read);
    }
    
    free_memory(attr);
  }
}
