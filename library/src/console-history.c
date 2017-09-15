#include "console-history.h"

#include "memory-util.h"

#include <assert.h>


// TODO: use  text_line_t  and  text_array_t  from scroll-counter.c
struct history_line_t {
  int length;
  wchar_t content[1]; // 0-terminated
};

struct hyper_console_history_t {
  struct history_line_t **entries;
  int capacity;
  int count;
  
  wchar_t *future_entry; // 0-terminated
  int      future_entry_length;
};


HYPER_CONSOLE_API
struct hyper_console_history_t *hyper_console_history_new(int options) {
  struct hyper_console_history_t *hist;
  
  hist = hyper_console_allocate_memory(sizeof(struct hyper_console_history_t));
  if(!hist)
    return NULL;
    
  memset(hist, 0, sizeof(struct hyper_console_history_t));
  
  return hist;
}


HYPER_CONSOLE_API
void hyper_console_history_free(struct hyper_console_history_t *hist) {
  int i;
  
  if(hist == NULL)
    return;
    
  for(i = 0; i < hist->count; ++i)
    hyper_console_free_memory(hist->entries[i]);
    
  hyper_console_free_memory(hist->entries);
  hyper_console_free_memory(hist->future_entry);
  hyper_console_free_memory(hist);
}


int console_history_count(struct hyper_console_history_t *hist) {
  if(hist == NULL)
    return 0;
    
  return hist->count;
}

const wchar_t *console_history_get(struct hyper_console_history_t *hist, int index, int *length) {
  struct history_line_t *line;
  
  if(hist == NULL) {
    if(length)
      *length = 0;
    return NULL;
  }
  
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

const wchar_t *console_history_get_future(struct hyper_console_history_t *hist, int *length) {
  if(hist == NULL) {
    if(length)
      *length = 0;
    return NULL;
  }
  
  if(length)
    *length = hist->future_entry_length;
    
  return hist->future_entry;
}

void console_history_set_future(struct hyper_console_history_t *hist, const wchar_t *text, int text_length) {
  wchar_t *old_entry;
  
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
    
  old_entry = hist->future_entry;
  hist->future_entry_length = 0;
  if(text) {
    hist->future_entry = hyper_console_allocate_memory((text_length + 1) * sizeof(wchar_t));
    if(hist->future_entry) {
      memcpy(hist->future_entry, text, text_length * sizeof(wchar_t));
      hist->future_entry[text_length] = L'\0';
      hist->future_entry_length = text_length;
    }
  }
  else 
    hist->future_entry = NULL;
  
  hyper_console_free_memory(old_entry);
}

void console_history_add(struct hyper_console_history_t *hist, const wchar_t *text, int text_length) {
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
    
  // Ignore duplicates. TODO: ignore old duplicates if GetConsoleHistoryInfo() says so.
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
  
  line = hyper_console_allocate_memory(sizeof(struct history_line_t) + text_length * sizeof(wchar_t));
  if(line == NULL)
    return;
    
  line->length = text_length;
  memcpy(&line->content[0], text, text_length * sizeof(wchar_t));
  line->content[text_length] = L'\0';
  
  hist->entries[hist->count] = line;
  hist->count++;
}
