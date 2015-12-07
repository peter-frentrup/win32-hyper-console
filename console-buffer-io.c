#include "console-buffer-io.h"

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
  
  *lpNumberOfCharsRead = 0;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi) || csbi.dwSize.X <= 0) {
    OutputDebugStringW(L"console_read_output_character: GetConsoleScreenBufferInfo");
    return FALSE;
  }
  
  while(nLength > MAX_BUFFER) {
    int x;
    
    if(!ReadConsoleOutputCharacterW(hConsoleOutput, lpCharacter, MAX_BUFFER, dwReadCoord, &chars_read)) {
      OutputDebugStringW(L"console_read_output_character: ReadConsoleOutputCharacterW");
      return FALSE;
    }
    
    *lpNumberOfCharsRead += chars_read;
    lpCharacter += chars_read;
    nLength -= chars_read;
    x = dwReadCoord.X + chars_read;
    
    dwReadCoord.Y += x / csbi.dwSize.X;
    dwReadCoord.Y = x % csbi.dwSize.X;
    
    if(dwReadCoord.Y >= csbi.dwSize.Y)
      return FALSE;
  }
  
  if(nLength > 0) {
    if(!ReadConsoleOutputCharacterW(hConsoleOutput, lpCharacter, nLength, dwReadCoord, &chars_read)) {
      OutputDebugStringW(L"console_read_output_character: ReadConsoleOutputCharacterW 2");
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
  
  *lpNumberOfAttrsRead = 0;
  
  if(!GetConsoleScreenBufferInfo(hConsoleOutput, &csbi) || csbi.dwSize.X <= 0) {
    OutputDebugStringW(L"console_read_output_attribute: GetConsoleScreenBufferInfo");
    return FALSE;
  }
  
  while(nLength > MAX_BUFFER) {
    int x;
    
    if(!ReadConsoleOutputAttribute(hConsoleOutput, lpAttribute, MAX_BUFFER, dwReadCoord, &attrs_read)) {
      OutputDebugStringW(L"console_read_output_attribute: ReadConsoleOutputAttribute");
      return FALSE;
    }
    
    *lpNumberOfAttrsRead += attrs_read;
    lpAttribute += attrs_read;
    nLength -= attrs_read;
    x = dwReadCoord.X + attrs_read;
    
    dwReadCoord.Y += x / csbi.dwSize.X;
    dwReadCoord.Y = x % csbi.dwSize.X;
    
    if(dwReadCoord.Y >= csbi.dwSize.Y)
      return FALSE;
  }
  
  if(nLength > 0) {
    if(!ReadConsoleOutputAttribute(hConsoleOutput, lpAttribute, nLength, dwReadCoord, &attrs_read)) {
      OutputDebugStringW(L"console_read_output_attribute: ReadConsoleOutputAttribute 2");
      return FALSE;
    }
      
    *lpNumberOfAttrsRead += attrs_read;
  }
  
  return TRUE;
}
