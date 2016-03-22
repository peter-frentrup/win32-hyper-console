#include "console-history.h"

#include "memory-util.h"

#include <assert.h>

// TODO: use  text_line_t  and  text_array_t  from scroll-counter.c
struct history_line_t {
  int length;
  wchar_t content[1];
};

struct console_history_t {
  struct history_line_t **entries;
  int capacity;
  int count;
};


struct console_history_t *console_history_new(int options) {
  struct console_history_t *hist;
  
  hist = allocate_memory(sizeof(struct console_history_t));
  if(hist) {
    memset(hist, 0, sizeof(struct console_history_t));
  }
  
  return hist;
}


void console_history_free(struct console_history_t *hist) {
  int i;
  
  if(hist == NULL)
    return;
    
  for(i = 0; i < hist->count; ++i)
    free_memory(hist->entries[i]);
    
  free_memory(hist->entries);
  free_memory(hist);
}


int console_history_count(struct console_history_t *hist) {
  if(hist == NULL)
    return 0;
    
  return hist->count;
}

const wchar_t *console_history_get(struct console_history_t *hist, int index, int *length) {
  struct history_line_t *line;
  
  assert(hist != NULL);
  
  if(index < 0 || index >= hist->count) {
    if(length != NULL) {
      *length = 0;
    }
    
    return NULL;
  }
  
  line = hist->entries[index];
  
  if(length != NULL) {
    *length = line->length;
  }
  
  return &line->content[0];
}

void console_history_add(struct console_history_t *hist, const wchar_t *text, int text_length) {
  struct history_line_t *line;
  const wchar_t *prev_text;
  int prev_length;
  
  assert(text != NULL || text_length == 0);
  
  if(hist == NULL)
    return;
    
  if(text_length < 0) {
    size_t ulen = wcslen(text);
    
    if(ulen >= INT_MAX)
      return;
      
    text_length = (int)ulen;
  }
  
  if(text_length >= INT_MAX / 2)
    return;
  
  // Ignore empty entries
  if(text_length == 0)
    return;
  
  // Ignore duplicates. TODO: respect win32 console setting regarding this.
  prev_text = console_history_get(hist, hist->count - 1, &prev_length);
  if( prev_length == text_length &&
      memcmp(prev_text, text, sizeof(wchar_t) * text_length) == 0)
  {
    return;
  }
  
  if(!resize_array(
      (void**)&hist->entries, 
      &hist->capacity, 
      sizeof(hist->entries[0]), 
      hist->count + 1)) 
  {
    return;
  }
  
  line = allocate_memory(sizeof(struct history_line_t) + text_length * sizeof(wchar_t));
  if(line == NULL)
    return;
  
  line->length = text_length;
  memcpy(&line->content[0], text, text_length * sizeof(wchar_t));
  line->content[text_length] = L'\0';
  
  hist->entries[hist->count] = line;
  hist->count++;
}
