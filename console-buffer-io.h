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

#endif // __CONSOLE_BUFFER_IO_H__
