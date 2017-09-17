#ifndef __CONSOLE__HYPERLINK_OUTPUT_H_I_
#define __CONSOLE__HYPERLINK_OUTPUT_H_I_

#include <hyper-console.h>

typedef struct dangling_hyperlinks_t *dangling_hyperlinks_ptr_t;

void hyperlink_system_free_dangling_hyperlinks(struct dangling_hyperlinks_t *links);
void hyperlink_system_move_links(struct dangling_hyperlinks_t *links, int delta_lines);
struct dangling_hyperlinks_t *hyperlink_system_cut_links_after_cursor(void);
void hyperlink_system_paste_and_activate_links(struct dangling_hyperlinks_t *links); // frees `links`

BOOL hyperlink_system_local_to_global(COORD local, int *line, int *column);

BOOL hyperlink_system_click(COORD local);
BOOL hyperlink_system_get_hover_title(COORD local, wchar_t *buf, size_t buf_len);

BOOL hyperlink_system_handle_events(INPUT_RECORD *event);

void hyperlink_system_start_input(int console_width, int pre_input_lines);
void hyperlink_system_end_input(void);

//void hyperlink_system_print_debug_info(void);

#endif // __CONSOLE__HYPERLINK_OUTPUT_H_I_
