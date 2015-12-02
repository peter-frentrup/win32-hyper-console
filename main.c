#include "read-input.h"
#include "hyperlink-output.h"

#include <assert.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

static void write_links_sentence(const wchar_t *text) {
  wchar_t *str = wcsdup(text);
  
  wchar_t *s = str;
  while(*s) {
    while(*s && !iswalnum(*s))
      putwchar(*s++);
    
    if(*s) {
      wchar_t ch;
      wchar_t *e = s + 1;
      while(*e && iswalnum(*e))
        ++e;
      
      fflush(stdout);
      ch = *e;
      *e = L'\0';
      start_hyperlink(s);
      set_hyperlink_input_text(s);
      *e = ch;
      
      while(s != e)
        putwchar(*s++);
      
      fflush(stdout);
      end_hyperlink();
    }
  }
  
  free(str);
}

static void write_unicode(const wchar_t *str) {
  int oldmode = _setmode(_fileno(stdout), _O_U8TEXT);
  /* Note that printf and other char* functions do not work at all in _O_U8TEXT mode. */
  
  wprintf(L"%s", str);
  
  _setmode(_fileno(stdout), oldmode);
}

int main() {
  BOOL multiline_mode = FALSE;
  wchar_t *str = NULL;
  
  init_hyperlink_system();
  
  printf("Finish with '");
  fflush(stdout);
  start_hyperlink(L"perform quit");
  set_hyperlink_input_text(L"quit");
  printf("quit");
  fflush(stdout);
  end_hyperlink();
  printf("'. ");
  
  printf("Switch to multi-line mode with '");
  fflush(stdout);
  start_hyperlink(L"enter multi-line mode");
  set_hyperlink_input_text(L"multi");
  printf("multi");
  fflush(stdout);
  end_hyperlink();
  printf("' and back with '");
  fflush(stdout);
  start_hyperlink(L"enter single-line mode");
  set_hyperlink_input_text(L"single");
  printf("single");
  fflush(stdout);
  end_hyperlink();
  printf("'\n");
  
  for(;;) {
    printf("\ntype ");
    fflush(stdout);
    start_hyperlink(L"need help?");
    set_hyperlink_input_text(L"help");
    printf("something");
    fflush(stdout);
    end_hyperlink();
    printf(": ");
    
    free_memory(str);
    str = read_input(multiline_mode);
    if(!str)
      continue;
      
    write_unicode(L"you typed '");
    write_unicode(str);
    write_unicode(L"'\n");
    
    if(wcscmp(str, L"multi") == 0) {
      printf("Switching to multi-line mode.\n");
      multiline_mode = TRUE;
    }
    
    if(wcscmp(str, L"single") == 0) {
      printf("Switching to single-line mode.\n");
      multiline_mode = FALSE;
    }
    
    if(wcscmp(str, L"help") == 0) {
      printf("The available options are '");
      fflush(stdout);
      start_hyperlink(L"perform quit");
      set_hyperlink_input_text(L"quit");
      printf("quit");
      fflush(stdout);
      end_hyperlink();
      
      printf("', '");
      fflush(stdout);
      start_hyperlink(L"change to multi line input mode");
      set_hyperlink_input_text(L"multi");
      printf("multi");
      fflush(stdout);
      end_hyperlink();
      
      printf("' and '");
      fflush(stdout);
      start_hyperlink(L"change to single line input mode");
      set_hyperlink_input_text(L"single");
      printf("single");
      fflush(stdout);
      end_hyperlink();
      
      printf("'\n");
      
      printf("You can use keyboard shortcuts Ctrl+C, Ctrl+X, Ctrl+V to access the clipboard.\n");
    }
    
    if(wcscmp(str, L"lorem") == 0) {
      write_links_sentence(
        L"Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt "
        L"ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
        L"ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in "
        L"reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur "
        L"sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est "
        L"laborum.\n"
      );
    }
    
    if(wcscmp(str, L"quit") == 0)
      break;
  }
  
  done_hyperlink_system();
  
  return 0;
}
