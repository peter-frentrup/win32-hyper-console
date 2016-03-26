#include <hyper-console.h>

#include "debug.h"

#include <windows.h>
#include <strsafe.h>


void debug_printf(const wchar_t *format, ...) {
  va_list args;
  wchar_t buffer[1024];
  
  va_start(args, format);
  StringCbVPrintfW(buffer, sizeof(buffer), format, args);
  va_end(args);
  
  OutputDebugStringW(buffer);
}
