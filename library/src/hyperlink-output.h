#ifndef __CONSOLE__HYPERLINK_OUTPUT_H_I_
#define __CONSOLE__HYPERLINK_OUTPUT_H_I_

#include <hyper-console.h>

void hyperlink_system_clear_links_after_cursor(void);

BOOL hyperlink_system_handle_events(INPUT_RECORD *event);

void hyperlink_system_start_input(int console_width, int pre_input_lines);
void hyperlink_system_end_input(void);

//void hyperlink_system_print_debug_info(void);

#endif // __CONSOLE__HYPERLINK_OUTPUT_H_I_
