#ifndef __CONSOLE_BUFFER_IO_H__
#define __CONSOLE_BUFFER_IO_H__

#include <windows.h>

BOOL console_read_output_character(
    HANDLE hConsoleOutput,
    LPWSTR lpCharacter,
    DWORD nLength,
    COORD dwReadCoord,
    LPDWORD lpNumberOfCharsRead);

BOOL console_read_output_attribute(
    HANDLE hConsoleOutput,
    LPWORD lpAttribute,
    DWORD nLength,
    COORD dwReadCoord,
    LPDWORD lpNumberOfAttrsRead);

BOOL console_output_invert_colors(
    HANDLE hConsoleOutput,
    COORD  start,
    int    length);

void console_clean_lines(HANDLE hConsoleOutput, int num_lines);
    
#endif // __CONSOLE_BUFFER_IO_H__
