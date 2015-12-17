#include "console-buffer-io.h"
#include "debug.h"

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

BOOL console_output_invert_colors(
    HANDLE hConsoleOutput,
    COORD  start,
    int    length
) {
  WORD attributes[MAX_BUFFER];
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi) || csbi.dwSize.X <= 0) {
    debug_printf(L"console_invert_output_color: GetConsoleScreenBufferInfo");
    return FALSE;
  }
  
  while(length > MAX_BUFFER) {
    start = console_output_invert_colors_helper(
      hConsoleOutput,
      csbi.dwSize,
      start,
      MAX_BUFFER,
      attributes);
    
    length-= MAX_BUFFER;
  }

  console_output_invert_colors_helper(
    hConsoleOutput,
    csbi.dwSize,
    start,
    length,
    attributes);
  
  return TRUE;
}
