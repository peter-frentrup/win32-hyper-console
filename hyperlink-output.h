#ifndef __CONSOLE__HYPERLINK_OUTPUT_H_I_
#define __CONSOLE__HYPERLINK_OUTPUT_H_I_

#include <wchar.h>
#include <windows.h>

void start_hyperlink(const wchar_t *title);
void end_hyperlink(void);

BOOL hyperlink_system_handle_mouse_event(const MOUSE_EVENT_RECORD *er);

void init_hyperlink_system(void);
void done_hyperlink_system(void);

#endif // __CONSOLE__HYPERLINK_OUTPUT_H_I_
