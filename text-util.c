#include "text-util.h"

#include <assert.h>


wchar_t console_get_opposite_fence(wchar_t ch) {
  switch(ch) {
    case L'(':
      return L')';
      
    case L'[':
      return L']';
      
    case L'{':
      return L'}';
      
    case L')':
      return L'(';
      
    case L']':
      return L'[';
      
    case L'}':
      return L'{';
      
    default:
      return L'\0';
  }
}

int console_find_opposite_fence(const wchar_t *text, int text_length, int pos) {
  int direction;
  wchar_t fence;
  wchar_t other_fence;
  int depth;
  
  assert(text != NULL || text_length == 0);
  assert(pos >= 0);
  assert(pos <= text_length);
  
  fence = text[pos];
  other_fence = console_get_opposite_fence(fence);
  if(!other_fence)
    return -1;
    
  switch(fence) {
    case L'(':
    case L'[':
    case L'{':
      direction = +1;
      break;
      
    case L')':
    case L']':
    case L'}':
      direction = -1;
      break;
      
    default:
      return -1;
  }
  
  depth = 0;
  for(; pos >= 0 && pos < text_length; pos += direction) {
    if(text[pos] == fence) {
      ++depth;
      continue;
    }
    
    if(text[pos] == other_fence) {
      if(--depth == 0)
        return pos;
        
      continue;
    }
  }
  
  return -1;
}


int console_get_word_start(const wchar_t *text, int text_length, int pos) {
  assert(pos >= 0);
  assert(pos <= text_length);
  
  if(pos > 0) {
    if(iswalpha(text[pos])) {
      while(pos > 0 && iswalpha(text[pos - 1]))
        --pos;
    }
    else if(iswdigit(text[pos])) {
      while(pos > 0 && iswdigit(text[pos - 1]))
        --pos;
    }
    else if(iswspace(text[pos])) {
      while(pos > 0 && iswspace(text[pos - 1]))
        --pos;
    }
  }
  
  return pos;
}

int console_get_word_end(const wchar_t *text, int text_length, int pos) {
  assert(pos >= 0);
  assert(pos <= text_length);
  
  if(pos < text_length) {
    if(iswalpha(text[pos])) {
      while(pos < text_length && iswalpha(text[pos + 1]))
        ++pos;
    }
    else if(iswdigit(text[pos])) {
      while(pos < text_length && iswdigit(text[pos + 1]))
        ++pos;
    }
    else if(iswspace(text[pos])) {
      while(pos < text_length && iswspace(text[pos + 1]))
        ++pos;
    }
    
    return pos + 1;
  }
  
  return pos;
}
