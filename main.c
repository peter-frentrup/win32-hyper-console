#include "read-input.h"
#include "memory-util.h"
#include "hyperlink-output.h"

#include <assert.h>
#include <io.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>


static void write_unicode(const wchar_t *str) {
  int oldmode;
  
  fflush(stdout);
  oldmode = _setmode(_fileno(stdout), _O_U8TEXT);
  /* Note that printf and other char* functions do not work at all in _O_U8TEXT mode. */
  
  wprintf(L"%s", str);
  
  fflush(stdout);
  _setmode(_fileno(stdout), oldmode);
}

static void write_simple_link(const wchar_t *title, const wchar_t *input_text, const wchar_t *content) {
  fflush(stdout);
  start_hyperlink(title);
  set_hyperlink_input_text(input_text);
  
  write_unicode(content);
  
  end_hyperlink();
}

static void write_path_links(wchar_t *path, int start) {
  wchar_t link[MAX_PATH + 20];
  
  while(path[start]) {
    DWORD next = start;
    wchar_t ch;
    
    while(path[next] == L'\\')
      ++next;
      
    ch = path[next];
    path[next] = L'\0';
    write_unicode(path + start);
    path[next] = ch;
    start = next;
    
    while(path[next] && path[next] != L'\\')
      ++next;
      
    ch = path[next];
    path[next] = L'\0';
    
    StringCbPrintfW(link, sizeof(link), L"cd %s\\", path);
    write_simple_link(link, link, path + start);
    
    path[next] = ch;
    start = next;
  }
}

static void write_current_directory_path(void) {
  wchar_t path[MAX_PATH];
  
  GetCurrentDirectoryW(ARRAYSIZE(path), path);
  write_path_links(path, 0);
}

static void change_directory(const wchar_t *command_rest) {
  while(*command_rest == ' ')
    ++command_rest;
  
  if(*command_rest) {
    SetCurrentDirectoryW(command_rest);
    return;
  }
  
  write_current_directory_path();
  printf("\\\n");
}

static void list_directory(void) {
  WIN32_FIND_DATAW ffd;
  LARGE_INTEGER filesize;
  wchar_t path[MAX_PATH];
  wchar_t link[MAX_PATH + 10];
  DWORD length;
  
  HANDLE hFind = INVALID_HANDLE_VALUE;
  
  length = GetCurrentDirectoryW(ARRAYSIZE(path), path);
  
  printf(" Directory ");
  write_path_links(path, 0);
  printf("\n\n");
  
  if(length + 2 >= ARRAYSIZE(path))
    return;
    
  StringCbCatW(path, sizeof(path), L"\\*");
  
  hFind = FindFirstFileW(path, &ffd);
  if(hFind == INVALID_HANDLE_VALUE) {
    printf("FindFirstFileW\n");
    return;
  }
  
  path[length] = L'\0';
  
  do {
    wchar_t datetime_string[20];
    
    FILETIME filetime = ffd.ftCreationTime;
    SYSTEMTIME datetime;
    
    FileTimeToLocalFileTime( &filetime, &filetime );
    FileTimeToSystemTime( &filetime, &datetime );
    
    StringCbPrintfW(
      datetime_string, sizeof(datetime_string), 
      L"%4d-%02d-%02d  %02d:%02d ", 
      (int)datetime.wYear,
      (int)datetime.wMonth,
      (int)datetime.wDay,
      (int)datetime.wHour,
      (int)datetime.wMinute);
    
    write_unicode(datetime_string);
    
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      printf("    <DIR>          ");
      
      StringCbPrintfW(link, sizeof(link), L"cd %s\\%s\\", path, ffd.cFileName);
      
      write_simple_link(link, link, ffd.cFileName);
    }
    else {
      filesize.LowPart = ffd.nFileSizeLow;
      filesize.HighPart = ffd.nFileSizeHigh;
      printf("%18" PRIu64 " ", (uint64_t)filesize.QuadPart);
      
      write_unicode(ffd.cFileName);
    }
    
    printf("\n");
  } while(FindNextFileW(hFind, &ffd) != 0);
}

static BOOL first_word_equals(const wchar_t *str, const wchar_t *word) {
  while(*word && *str && *word == *str) {
    ++word;
    ++str;
  }
  
  if(*word)
    return FALSE;
    
  return *str == L' ' || *str == L'\0';
}

static void write_links_sentence(const wchar_t *text) {
  wchar_t *str = wcsdup(text);
  
  wchar_t *s = str;
  while(*s) {
    while(*s && !iswalnum(*s))
      putwchar(*s++);
      
    if(*s) {
      wchar_t ch;
      wchar_t *e = s + 1;
      while(*e && iswalnum(*e))
        ++e;
        
      fflush(stdout);
      ch = *e;
      *e = L'\0';
      start_hyperlink(s);
      set_hyperlink_input_text(s);
      *e = ch;
      
      while(s != e)
        putwchar(*s++);
        
      fflush(stdout);
      end_hyperlink();
    }
  }
  
  free(str);
}

int main() {
  BOOL multiline_mode = FALSE;
  wchar_t *str = NULL;
  
  init_hyperlink_system();
  
  write_simple_link(L"show available options", L"help", L"Help");
  printf(" needed? ");
  
  printf("Finish with '");
  write_simple_link(L"exit program", L"quit", L"quit");
  printf("'. ");
  
  printf("Switch to multi-line mode with '");
  write_simple_link(L"enter multi-line mode", L"multi", L"multi");
  printf("' and back with '");
  write_simple_link(L"enter single-line mode", L"single", L"single");
  printf("'\n");
  
  for(;;) {
    //printf("\ntype ");
    //write_simple_link(L"need help?", L"help", L"something");
    //printf(": ");
    printf("\n");
    write_current_directory_path();
    printf(">");
    
    free_memory(str);
    str = read_input(multiline_mode, L"help");
    if(!str)
      continue;
    
    if(wcscmp(str, L"multi") == 0) {
      printf("Switching to multi-line mode.\n");
      multiline_mode = TRUE;
      continue;
    }
    
    if(wcscmp(str, L"single") == 0) {
      printf("Switching to single-line mode.\n");
      multiline_mode = FALSE;
      continue;
    }
    
    if(wcscmp(str, L"dir") == 0) {
      list_directory();
      continue;
    }
    
    if(wcscmp(str, L"pwd") == 0) {
      write_current_directory_path();
      printf("\\\n");
      continue;
    }
    
    if(first_word_equals(str, L"cd")) {
      change_directory(str + 2);
      continue;
    }
    
    if(wcscmp(str, L"help") == 0) {
      printf("The available options are '");
      write_simple_link(L"print working directory path", L"pwd", L"pwd");
      printf("', '");
      write_simple_link(L"change directory", L"cd", L"cd");
      printf("', '");
      write_simple_link(L"list current directory", L"dir", L"dir");
      printf("', '");
      write_simple_link(L"exit program", L"quit", L"quit");
      printf("', '");
      write_simple_link(L"change to multi line input mode", L"multi", L"multi");
      printf("' and '");
      write_simple_link(L"change to single line input mode", L"single", L"single");
      printf("'\n");
      
      printf("You can use keyboard shortcuts Ctrl+C, Ctrl+X, Ctrl+V to access the clipboard.\n");
      continue;
    }
    
    if(wcscmp(str, L"lorem") == 0) {
      write_links_sentence(
          L"Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt "
          L"ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
          L"ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in "
          L"reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur "
          L"sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est "
          L"laborum.\n"
      );
      continue;
    }
    
    if(wcscmp(str, L"quit") == 0)
      break;
    
    write_unicode(L"you typed '");
    write_unicode(str);
    write_unicode(L"'\n");
  }
  
  done_hyperlink_system();
  
  return 0;
}
