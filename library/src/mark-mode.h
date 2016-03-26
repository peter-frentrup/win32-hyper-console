#ifndef __CONSOLE__MARK_MODE_H__
#define __CONSOLE__MARK_MODE_H__

#include <windows.h>

BOOL console_handle_mark_mode(HANDLE hConsoleInput, HANDLE hConsoleOutput, INPUT_RECORD *event, BOOL force_mark_mode);

#endif // __CONSOLE__MARK_MODE_H__
