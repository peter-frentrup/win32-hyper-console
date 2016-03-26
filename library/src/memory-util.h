#ifndef __CONSOLE__MEMORY_UTIL_H__
#define __CONSOLE__MEMORY_UTIL_H__

#include <windows.h>


/** Resize an array.
  
  @param arr        A pointer to the array variable.
  @param capacity   A pointer to the array capacity variable (current maximum length)
  @param item_size  The size of a single item.
  @param newsize    The new minimum number of items.
  
  @return TRUE on success, FALSE on failure.
  
  On success, the variable @a *arr will be set to the new array and @a *capacity is the new maximum 
  length.
  If this function fails (out-of-memory, integer overflow), the original array and capacity 
  remain unchanged.
  Any new items are uninitialized, while old items are copied over from the old array.
  
  The array @a *arr may be freed with free_memory().
 */
BOOL resize_array(void **arr, int *capacity, int item_size, int newsize);


/** Allocate a block of memory.
 */
void *allocate_memory(size_t size);


#endif // __CONSOLE__MEMORY_UTIL_H__
