#ifndef __CONSOLE__TEXT_UTIL_H__
#define __CONSOLE__TEXT_UTIL_H__

#include <wchar.h>

wchar_t console_get_opposite_fence(wchar_t ch);
int console_find_opposite_fence(const wchar_t *text, int text_length, int pos);

int console_get_word_start(const wchar_t *text, int text_length, int pos);
int console_get_word_end(const wchar_t *text, int text_length, int pos);

wchar_t *append_text(wchar_t *dst, const wchar_t *dst_end, const wchar_t *src, const wchar_t *optional_src_end);

#endif // __CONSOLE__TEXT_UTIL_H__
