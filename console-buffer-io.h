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

void console_reinvert_colors(
  HANDLE hConsoleOutput,
  COORD old_start,
  COORD old_end,
  COORD new_start,
  COORD new_end);

/* Invert old_rect and then new_rect.

   \param hConsoleHandle A console buffer handle.
   \param old_rect A rectangle. It must have sorted and coordinates. 
                   Right/Bottom coordinates are not included (like Win32 GDI functions, 
                   unlike many Win32 console functions).
                   Hence an empty rectangle has Left==Right or Top==Bottom.
   \param new_rect A rectangle. It must have sorted and coordinates. 
                   Right/Bottom coordinates are not included (like Win32 GDI functions, 
                   unlike many Win32 console functions).
                   Hence an empty rectangle has Left==Right or Top==Bottom.
 */
void console_reinvert_colors_rect(
  HANDLE hConsoleOutput,
  const SMALL_RECT *old_rect,
  const SMALL_RECT *new_rect);

void console_clean_lines(HANDLE hConsoleOutput, int num_lines);

BOOL console_scroll_wheel(HANDLE hConsoleOutput, const MOUSE_EVENT_RECORD *er);

void console_paste_from_clipboard(HANDLE hConsoleInput);

BOOL console_get_screen_word_start_end(HANDLE hConsoleOutput, COORD pos, COORD *start, COORD *end);

void console_alert(HANDLE hConsoleOutput);

#endif // __CONSOLE_BUFFER_IO_H__
