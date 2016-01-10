#ifndef __CONSOLE__SEARCH_MODE_H__
#define __CONSOLE__SEARCH_MODE_H__

#include <windows.h>

BOOL console_handle_search_mode(HANDLE hConsoleInput, HANDLE hConsoleOutput, INPUT_RECORD *event, const wchar_t *filter);

#endif // __CONSOLE__SEARCH_MODE_H__
