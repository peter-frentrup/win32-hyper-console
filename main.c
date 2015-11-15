#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "read-input.h"

int main() {
  BOOL multiline_mode = FALSE;
  wchar_t *str = NULL;
  
  printf("Finish with 'quit'. Switch to multi-line mode with 'multi' and back with 'single'\n");
  
  do {
    printf("\nin: ");
    free_memory(str);
    str = read_input(multiline_mode);
    if(!str) 
      return 1;
    
    wprintf(L"you typed '%s'\n", str);
    
    if(wcscmp(str, L"multi") == 0) {
      wprintf(L"switching to multi-line mode.\n", str);
      multiline_mode = TRUE;
    }
    
    if(wcscmp(str, L"single") == 0) {
      wprintf(L"switching to single-line mode.\n", str);
      multiline_mode = FALSE;
    }
    
  } while(wcscmp(str, L"quit") != 0);
  
  return 0;
}
