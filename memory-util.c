#include "memory-util.h"

#include <assert.h>


BOOL resize_array(void **arr, int *capacity, int item_size, int newsize) {
  int newcap;
  
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
    
  *arr = realloc(*arr, newcap * item_size);
  if(!*arr) {
    *capacity = 0;
    return FALSE;
  }
  
  *capacity = newcap;
  return TRUE;
}

void free_memory(void *data) {
  free(data);
}
