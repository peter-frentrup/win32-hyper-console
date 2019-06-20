#ifndef __CONSOLE_BUFFER_IO_H__
#define __CONSOLE_BUFFER_IO_H__

#include <windows.h>

#define LITERAL_KEY_STATE  0x10000

BOOL console_read_output(
  HANDLE      hConsoleOutput,
  PCHAR_INFO  lpBuffer,
  COORD       dwBufferSize,
  COORD       dwBufferCoord,
  PSMALL_RECT lpReadRegion);

/** Read characters from the console buffer.
    Fullwidth CJK characters that take two cells are compressed, so the number of chars read 
    may be smaller than the number of cells specified by dwReadCoord.
 */
BOOL console_read_output_character(
  HANDLE hConsoleOutput,
  LPWSTR lpCharacter,
  DWORD nLength,
  COORD dwReadCoord,
  LPDWORD lpNumberOfCharsRead);

/** Read attributes from the console buffer.
    Fullwidth CJK characters that take two cells are compressed, so the number of chars read 
    may be smaller than the number of cells specified by dwReadCoord.
 */
BOOL console_read_output_attribute(
  HANDLE hConsoleOutput,
  LPWORD lpAttribute,
  DWORD nLength,
  COORD dwReadCoord,
  LPDWORD lpNumberOfAttrsRead);

BOOL console_write_output_attribute(
  HANDLE hConsoleOutput,
  const LPWORD lpAttribute,
  DWORD nLength,
  COORD dwWriteCoord,
  LPDWORD lpNumberOfAttrsWritten);

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

BOOL console_scroll_key(HANDLE hConsoleOutput, const KEY_EVENT_RECORD *er);

void console_paste_from_clipboard(HANDLE hConsoleInput);

BOOL console_get_screen_word_start_end(HANDLE hConsoleOutput, COORD pos, COORD *start, COORD *end);

void console_alert(HANDLE hConsoleOutput);

#endif // __CONSOLE_BUFFER_IO_H__
