#include "console-buffer-io.h"
#include "debug.h"
#include "memory-util.h"

#include <assert.h>


#define MAX_BUFFER 512


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



BOOL console_read_output_attribute(
    HANDLE hConsoleOutput,
    LPWORD lpAttribute,
    DWORD nLength,
    COORD dwReadCoord,
    LPDWORD lpNumberOfAttrsRead
) {
  DWORD attrs_read = 0;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  assert(lpAttribute != NULL);
  assert(lpNumberOfAttrsRead != NULL);
  assert(dwReadCoord.X >= 0);
  assert(dwReadCoord.Y >= 0);
  
  *lpNumberOfAttrsRead = 0;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi) || csbi.dwSize.X <= 0) {
    debug_printf(L"console_read_output_attribute: GetConsoleScreenBufferInfo");
    return FALSE;
  }
  
  while(nLength > MAX_BUFFER) {
    int x;
    
    if(!ReadConsoleOutputAttribute(hConsoleOutput, lpAttribute, MAX_BUFFER, dwReadCoord, &attrs_read)) {
      debug_printf(L"console_read_output_attribute: ReadConsoleOutputAttribute");
      return FALSE;
    }
    
    *lpNumberOfAttrsRead += attrs_read;
    lpAttribute += attrs_read;
    nLength -= attrs_read;
    x = dwReadCoord.X + attrs_read;
    
    dwReadCoord.Y += x / csbi.dwSize.X;
    dwReadCoord.X = x % csbi.dwSize.X;
    
    if(dwReadCoord.Y >= csbi.dwSize.Y)
      return FALSE;
  }
  
  if(nLength > 0) {
    if(!ReadConsoleOutputAttribute(hConsoleOutput, lpAttribute, nLength, dwReadCoord, &attrs_read)) {
      debug_printf(L"console_read_output_attribute: ReadConsoleOutputAttribute 2");
      return FALSE;
    }
    
    *lpNumberOfAttrsRead += attrs_read;
  }
  
  return TRUE;
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
    int x;
    
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

