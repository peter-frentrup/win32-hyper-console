#include "text-util.h"

#include <assert.h>


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
