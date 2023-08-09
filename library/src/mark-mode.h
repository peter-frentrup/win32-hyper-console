#ifndef __CONSOLE__MARK_MODE_H__
#define __CONSOLE__MARK_MODE_H__

#include <windows.h>

struct mark_mode_settings_t {
  HANDLE input_handle;
  HANDLE output_handle;
  
  void  *callback_context;
  BOOL (*key_event_filter)(void *context, const KEY_EVENT_RECORD *er);
};

BOOL console_handle_mark_mode(
  struct mark_mode_settings_t *context,
  INPUT_RECORD                *event, 
  BOOL                         force_mark_mode);

#endif // __CONSOLE__MARK_MODE_H__
