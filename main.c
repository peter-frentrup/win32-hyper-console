#include "read-input.h"
#include "hyperlink-output.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>


int main() {
  BOOL multiline_mode = FALSE;
  wchar_t *str = NULL;
  
  init_hyperlink_system();
  
  printf("Finish with '");
  start_hyperlink(L"perform quit");
  set_hyperlink_input_text(L"quit");
  printf("quit");
  end_hyperlink();
  printf("'. ");
  
  printf("Switch to multi-line mode with '");
  start_hyperlink(L"enter multi-line mode");
  set_hyperlink_input_text(L"multi");
  printf("multi");
  end_hyperlink();
  printf("' and back with '");
  start_hyperlink(L"enter single-line mode");
  set_hyperlink_input_text(L"single");
  printf("single");
  end_hyperlink();
  printf("'\n");
  
  for(;;) {
    printf("\ntype ");
    start_hyperlink(L"need help?");
    set_hyperlink_input_text(L"help");
    printf("something");
    end_hyperlink();
    printf(": ");
    
    free_memory(str);
    str = read_input(multiline_mode);
    if(!str)
      continue;
      
    wprintf(L"you typed '%s'\n", str);
    
    if(wcscmp(str, L"multi") == 0) {
      wprintf(L"Switching to multi-line mode.\n", str);
      multiline_mode = TRUE;
    }
    
    if(wcscmp(str, L"single") == 0) {
      wprintf(L"Switching to single-line mode.\n", str);
      multiline_mode = FALSE;
    }
    
    if(wcscmp(str, L"help") == 0) {
      printf("The available options are '");
      start_hyperlink(L"perform quit");
      set_hyperlink_input_text(L"quit");
      printf("quit");
      end_hyperlink();
      
      printf("', '");
      start_hyperlink(L"change to multi line input mode");
      set_hyperlink_input_text(L"multi");
      printf("multi");
      end_hyperlink();
      
      printf("' and '");
      start_hyperlink(L"change to single line input mode");
      set_hyperlink_input_text(L"single");
      printf("single");
      end_hyperlink();
      
      printf("'\n");
      
      printf("You can use keyboard shortcuts Ctrl+C, Ctrl+X, Ctrl+V to access the clipboard.\n");
    }
    
    if(wcscmp(str, L"quit") == 0)
      break;
  }
  
  done_hyperlink_system();
  
  return 0;
}
