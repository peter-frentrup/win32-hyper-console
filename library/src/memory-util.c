#include <hyper-console.h>

#include "memory-util.h"

#include <assert.h>


BOOL resize_array(void **arr, int *capacity, int item_size, int newsize) {
  int newcap;
  void *new_arr;
  
  assert(arr != NULL);
  assert(capacity != NULL);
  assert(item_size > 0);
  
  if(newsize < 0 || newsize > (1 << 30) / item_size)
    return FALSE;
    
  if(newsize <= *capacity)
    return TRUE;
    
  newcap = 2 * (*capacity);
  if(newcap <= 0)
    newcap = 1;
  while(newcap < newsize)
    newcap *= 2;
    
  new_arr = realloc(*arr, newcap * item_size);
  if(!new_arr) {
    //hyper_console_free_memory(*arr);
    //*arr = NULL;
    //*capacity = 0;
    return FALSE;
  }
  
  *arr = new_arr;
  *capacity = newcap;
  return TRUE;
}

HYPER_CONSOLE_API
void *hyper_console_allocate_memory(size_t size) {
  return malloc(size);
}

HYPER_CONSOLE_API
void hyper_console_free_memory(void *data) {
  free(data);
}
