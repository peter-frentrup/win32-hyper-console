#ifndef __CONSOLE__HYPERLINK_OUTPUT_H_I_
#define __CONSOLE__HYPERLINK_OUTPUT_H_I_

#include <wchar.h>
#include <windows.h>

void start_hyperlink(const wchar_t *title);
void set_hyperlink_input_text(const wchar_t *text);
WORD set_hyperlink_color(WORD attribute);
void end_hyperlink(void);

BOOL hyperlink_system_handle_mouse_event(const MOUSE_EVENT_RECORD *er);
BOOL hyperlink_system_handle_key_event(const KEY_EVENT_RECORD *er);
BOOL hyperlink_system_handle_focus_event(const FOCUS_EVENT_RECORD *er);

void hyperlink_system_start_input(int console_width, int pre_input_lines);
void hyperlink_system_end_input(void);

void hyperlink_system_print_debug_info(void);

void init_hyperlink_system(void);
void done_hyperlink_system(void);

#endif // __CONSOLE__HYPERLINK_OUTPUT_H_I_
