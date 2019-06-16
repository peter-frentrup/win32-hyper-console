#define COBJMACROS

#include <hyper-console.h>

#include <wchar.h>

//#include "read-input.h"
//#include "console-history.h"
//#include "memory-util.h"
//#include "hyperlink-output.h"
//#include "debug.h"
//#include "text-util.h"

#include <assert.h>
#include <io.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>

#include <windows.h>
#include <shlobj.h>


#ifndef SYMLINK_FLAG_RELATIVE
#  define SYMLINK_FLAG_RELATIVE  1
#endif


static WORD file_link_color = FOREGROUND_RED | FOREGROUND_GREEN;
static WORD run_link_color = FOREGROUND_RED | FOREGROUND_BLUE;
static long interrupted = FALSE;

static void debug_printf(const wchar_t *format, ...) {
  va_list args;
  wchar_t buffer[1024];
  
  va_start(args, format);
  StringCbVPrintfW(buffer, sizeof(buffer), format, args);
  va_end(args);
  
  OutputDebugStringW(buffer);
}

static wchar_t *append_text(
  wchar_t       *dst,
  const wchar_t *dst_end,
  const wchar_t *src,
  const wchar_t *optional_src_end
) {
  assert(dst != NULL);
  assert(dst_end != NULL);
  assert(src != NULL);
  
  while(dst != dst_end && *src && src != optional_src_end)
    *dst++ = *src++;
    
  return dst;
}


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
  hyper_console_start_link(title);
  hyper_console_set_link_input_text(input_text);
  
  write_unicode(content);
  
  hyper_console_end_link();
}

static const wchar_t *get_extension(const wchar_t *filename) {
  const wchar_t *end = filename;
  const wchar_t *ext;
  
  assert(filename != NULL);
  
  while(*end)
    ++end;
    
  ext = end;
  while(ext != filename) {
    if(*ext == L'.')
      return ext;
      
    if(*ext == L'\\' || *ext == L'/')
      break;
      
    --ext;
  }
  
  return end;
}

static BOOL has_any_extension(const wchar_t *filename, const wchar_t *extensions) {
  const wchar_t *file_ext;
  wchar_t file_ext_upper[MAX_PATH];
  wchar_t other_ext_upper[MAX_PATH];
  const wchar_t *end;
  
  assert(filename != NULL);
  if(!extensions)
    return FALSE;
    
  file_ext = get_extension(filename);
  end = file_ext;
  while(*end)
    ++end;
    
  if(end - file_ext > ARRAYSIZE(file_ext_upper))
    return FALSE;
    
  memcpy(file_ext_upper, file_ext, (end - file_ext) * sizeof(wchar_t));
  CharUpperBuffW(file_ext_upper, (DWORD)(end - file_ext));
  
  while(*extensions) {
    const wchar_t *next = extensions;
    
    while(*next && *next != L';')
      ++next;
      
    if(next - extensions == end - file_ext) {
      memcpy(other_ext_upper, extensions, (next - extensions) * sizeof(wchar_t));
      CharUpperBuffW(other_ext_upper, (DWORD)(next - extensions));
      
      if(0 == wmemcmp(file_ext_upper, other_ext_upper, end - file_ext))
        return TRUE;
    }
    
    extensions = next;
    if(*extensions)
      ++extensions;
  }
  
  return FALSE;
}

static BOOL run_command_needs_quotes(const wchar_t *filename) {
  for(; *filename; ++filename) {
    switch(*filename) {
      case L'(':
      case L')':
      case L' ':
      case L'^':
        return TRUE;
    }
  }
  
  return FALSE;
}

static void write_file_link(
  const wchar_t *filename,
  const wchar_t *optional_owning_directory,
  const wchar_t *optional_text,
  const wchar_t *optional_args,
  BOOL is_directory
) {
  wchar_t cmd[2 * MAX_PATH + 20];
  
  assert(filename != NULL);
  if(optional_owning_directory && *optional_owning_directory == L'\0')
    optional_owning_directory = NULL;
    
  if(is_directory) {
    const wchar_t *final_baskslash = L"\\";
    
    if(filename) {
      const wchar_t *end = filename;
      while(*end)
        ++end;
        
      if(end[-1] == L'\\')
        final_baskslash = L"";
    }
    
    if(optional_owning_directory) {
      const wchar_t *dir_baskslash = L"\\";
      
      if(*optional_owning_directory) {
        const wchar_t *end = optional_owning_directory;
        while(*end)
          ++end;
          
        if(end[-1] == L'\\')
          dir_baskslash = L"";
      }
      
      StringCbPrintfW(cmd, sizeof(cmd), L"cd+dir %s%s%s%s",
                      optional_owning_directory,
                      dir_baskslash,
                      filename,
                      final_baskslash);
    }
    else
      StringCbPrintfW(cmd, sizeof(cmd), L"cd+dir %s%s", filename, final_baskslash);
      
    if(!optional_text)
      optional_text = cmd + 7;
      
    write_simple_link(cmd, cmd, optional_text);
  }
  else if(has_any_extension(filename, _wgetenv(L"PATHEXT"))) {
    BOOL need_quote;
    const wchar_t *quote;
    
    need_quote = run_command_needs_quotes(filename);
    if(!need_quote && optional_owning_directory)
      need_quote = run_command_needs_quotes(optional_owning_directory);
      
    quote = need_quote ? L"\"" : L"";
    
    StringCbPrintfW(cmd, sizeof(cmd), L"run %s%s%s%s%s%s%s",
                    quote,
                    optional_owning_directory ? optional_owning_directory : L"",
                    optional_owning_directory ? L"\\" : L"",
                    filename,
                    quote,
                    (optional_args && *optional_args) ? L" " : L"",
                    optional_args ? optional_args : L"");
                    
    if(!optional_text)
      optional_text = cmd + 4;
      
    fflush(stdout);
    hyper_console_start_link(cmd);
    hyper_console_set_link_input_text(cmd);
    hyper_console_set_link_color(run_link_color);
    
    write_unicode(optional_text);
    
    hyper_console_end_link();
  }
  else {
    if(optional_owning_directory)
      StringCbPrintfW(cmd, sizeof(cmd), L"open %s\\%s", optional_owning_directory, filename);
    else
      StringCbPrintfW(cmd, sizeof(cmd), L"open %s", filename);
      
    if(!optional_text)
      optional_text = cmd + 5;
      
    fflush(stdout);
    hyper_console_start_link(cmd);
    hyper_console_set_link_input_text(cmd);
    hyper_console_set_link_color(file_link_color);
    
    write_unicode(optional_text);
    
    hyper_console_end_link();
  }
}

static void write_path_links(wchar_t *path, int start) {
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
    
    write_file_link(path, NULL, path + start, NULL, TRUE);
    
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

/* This is REPARSE_DATA_BUFFER. That structure is not included in Windows SDK,
   but some Mingw headers and the Windows Driver SDK have it.
 */
struct reparse_data_buffer_t {
  ULONG  ReparseTag;
  USHORT ReparseDataLength;
  USHORT Reserved;
  union {
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      ULONG  Flags;
      WCHAR  PathBuffer[1];
    } SymbolicLinkReparseBuffer;
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      WCHAR  PathBuffer[1];
    } MountPointReparseBuffer;
    struct {
      UCHAR DataBuffer[1];
    } GenericReparseBuffer;
  };
};

static void print_reparse_point_info(const wchar_t *file, DWORD reparse_tag, BOOL is_directory) {
  struct {
    struct reparse_data_buffer_t header;
    wchar_t _rest[MAX_PATH - 1];
  } data;
  BOOL success;
  DWORD nOutBufferSize;
  
  HANDLE hDir = CreateFileW(
                  file,
                  0,
                  FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
                  NULL,
                  OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                  NULL);
                  
  if(hDir ==  INVALID_HANDLE_VALUE) {
    debug_printf(L"DeviceIoControl failed: %d at %s\n", (int)GetLastError(), file);
    return;
  }
  
  success = DeviceIoControl(
              hDir,
              FSCTL_GET_REPARSE_POINT,
              NULL,
              0,
              &data,
              sizeof(data),
              &nOutBufferSize,
              NULL);
              
  if(success) {
    wchar_t *subs;
    DWORD length;
    BOOL relative = FALSE;
    wchar_t dst_path[MAX_PATH];
    
    if(reparse_tag == IO_REPARSE_TAG_SYMLINK) {
      DWORD offset = data.header.SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(WCHAR);
      length       = data.header.SymbolicLinkReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
      subs         = data.header.SymbolicLinkReparseBuffer.PathBuffer + offset;
      relative     = (data.header.SymbolicLinkReparseBuffer.Flags & SYMLINK_FLAG_RELATIVE) != 0;
    }
    else {
      DWORD offset = data.header.MountPointReparseBuffer.SubstituteNameOffset / sizeof(WCHAR);
      length       = data.header.MountPointReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
      subs         = data.header.MountPointReparseBuffer.PathBuffer + offset;
    }
    
    if((uint8_t*)(subs + length) <= (uint8_t*)&data + sizeof(data) &&
        length < MAX_PATH)
    {
      // e.g. \??\C:\...
      if( length >= 7 &&
          subs[0] == L'\\' &&
          subs[1] == L'?' &&
          subs[2] == L'?' &&
          subs[3] == L'\\' &&
          subs[4] != L'\0' &&
          subs[5] == L':' &&
          subs[6] == L'\\')
      {
        subs += 4;
        length -= 4;
      }
      
      memcpy(dst_path, subs, length * sizeof(wchar_t));
      dst_path[length] = L'\0';
      
      printf(" [");
      if(relative) {
        wchar_t dst_fullpath[MAX_PATH];
        
        StringCbPrintfW(dst_fullpath, sizeof(dst_fullpath), L"%s\\..\\%s", file, dst_path);
        
        write_file_link(dst_fullpath, NULL, NULL, NULL, is_directory);
      }
      else {
        write_file_link(dst_path, NULL, NULL, NULL, is_directory);
      }
      printf("]");
    }
  }
  else {
    debug_printf(L"DeviceIoControl failed: %d at %s\n", (int)GetLastError(), file);
  }
  
  CloseHandle(hDir);
}

static void print_shortcut_info(const wchar_t *file) {
  HRESULT hr;
  IShellLinkW *sl;
  IPersistFile *pf;
  
  CoInitialize(NULL);
  
  sl = NULL;
  hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void**)&sl);
  if(SUCCEEDED(hr)) {
  
    pf = NULL;
    hr = IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void**)&pf);
    if(SUCCEEDED(hr)) {
    
      hr = IPersistFile_Load(pf, file, STGM_READ);
      if(SUCCEEDED(hr)) {
        wchar_t path[MAX_PATH];
        WIN32_FIND_DATAW wfd;
        
        hr = IShellLinkW_GetPath(sl, path, ARRAYSIZE(path), &wfd, 0);
        if(hr == S_OK) {
          wchar_t args[MAX_PATH];
          BOOL is_directory = ( wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
          
          *args = L'\0';
          hr = IShellLinkW_GetArguments(sl, args, ARRAYSIZE(args));
          if(FAILED(hr))
            *args = L'\0';
            
          printf(" [");
          write_file_link(path, NULL, NULL, args, is_directory);
          printf("]");
          
        }
      }
      
      IPersistFile_Release(pf);
    }
    
    IShellLinkW_Release(sl);
  }
  
  CoUninitialize();
}

static void list_directory(void) {
  WIN32_FIND_DATAW ffd;
  LARGE_INTEGER filesize;
  wchar_t path[MAX_PATH];
  wchar_t link[MAX_PATH + 20];
  DWORD length;
  unsigned num_files = 0;
  unsigned num_directories = 0;
  uint64_t file_sizes = 0;
  
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
    printf("Error %d in FindFirstFileW\n", (int)GetLastError());
    return;
  }
  
  path[length] = L'\0';
  
  do {
    wchar_t datetime_string[20];
    BOOL is_directory = ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    
    if(InterlockedOr(&interrupted, FALSE)) {
      FindClose(hFind);
      return;
    }
    
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
    
    if (is_directory) {
      const char *kind = "<DIR>";
      ++num_directories;
      
      if(ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if(ffd.dwReserved0 == IO_REPARSE_TAG_SYMLINK) {
          kind = "<SYMLINKD>";
        }
        else {
          kind = "<JUNCTION>";
        }
      }
      
      printf("    %-15s", kind);
    }
    else {
      ++num_files;
      
      if(ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        printf("    %-15s", "<SYMLINK>");
      }
      else {
        filesize.LowPart = ffd.nFileSizeLow;
        filesize.HighPart = ffd.nFileSizeHigh;
        printf("%18" PRIu64 " ", (uint64_t)filesize.QuadPart);
        
        file_sizes += (uint64_t)filesize.QuadPart;
      }
    }
    
    write_file_link(ffd.cFileName, path, ffd.cFileName, NULL, is_directory);
    
    if(!is_directory && has_any_extension(ffd.cFileName, L".LNK")) {
      StringCbPrintfW(link, sizeof(link), L"%s\\%s", path, ffd.cFileName);
      
      print_shortcut_info(link);
    }
    
    if(ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      StringCbPrintfW(link, sizeof(link), L"%s\\%s", path, ffd.cFileName);
      
      print_reparse_point_info(link, ffd.dwReserved0, ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    
    printf("\n");
  } while(FindNextFileW(hFind, &ffd) != 0);
  
  FindClose(hFind);
  
  printf("%16u File(s), %16" PRIu64 " Bytes\n", num_files, file_sizes);
  printf("%16u Directories\n", num_directories);
}

struct show_tree_t {
  wchar_t indent_buffer[100];
  wchar_t *indent_end;
  
  wchar_t path[MAX_PATH];
  wchar_t *path_end;
  
  const wchar_t *indent_more;
  const wchar_t *indent_more_sub;
  const wchar_t *indent_last;
  const wchar_t *indent_last_sub;
};

static BOOL find_next_directory(HANDLE *hFind, WIN32_FIND_DATAW *ffd) {
  assert(hFind != NULL);
  assert(ffd != NULL);
  
  if(*hFind == INVALID_HANDLE_VALUE)
    return FALSE;
    
  while(FindNextFileW(*hFind, ffd)) {
    if(InterlockedOr(&interrupted, FALSE))
      break;
      
    if(ffd->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
      continue;
      
    if(ffd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      return TRUE;
  }
  
  FindClose(*hFind);
  *hFind = INVALID_HANDLE_VALUE;
  ffd->cFileName[0] = L'\0';
  
  return FALSE;
}

static void show_directory_tree_helper(struct show_tree_t *context) {
  WIN32_FIND_DATAW ffd;
  wchar_t *indent_start;
  wchar_t *indent_buffer_end;
  
  wchar_t *s;
  wchar_t *path_end;
  wchar_t *path_buffer_end;
  
  HANDLE hFind = INVALID_HANDLE_VALUE;
  
  assert(context != NULL);
  
  indent_start      = context->indent_end;
  indent_buffer_end = context->indent_buffer + ARRAYSIZE(context->indent_buffer) - 1;
  
  path_end        = context->path_end;
  path_buffer_end = context->path + ARRAYSIZE(context->indent_buffer) - 1;
  
  assert(path_end >= context->path);
  assert(path_end < context->path + ARRAYSIZE(context->path));
  assert(indent_start >= context->indent_buffer);
  assert(indent_start <= indent_buffer_end);
  
  if(InterlockedOr(&interrupted, FALSE))
    return;
    
  s = append_text(path_end, path_buffer_end, L"\\*", NULL);
  if(s == path_buffer_end)
    return;
    
  *s = L'\0';
  hFind = FindFirstFileW(context->path, &ffd);
  if(hFind == INVALID_HANDLE_VALUE) {
    debug_printf(L"Error %d in FindFirstFileW\n", (int)GetLastError());
    return;
  }
  
  if((ffd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) ||
      !(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
  {
    find_next_directory(&hFind, &ffd);
  }
  
  if(wcscmp(ffd.cFileName, L".") == 0)
    find_next_directory(&hFind, &ffd);
    
  if(wcscmp(ffd.cFileName, L"..") == 0)
    find_next_directory(&hFind, &ffd);
    
  while(hFind != INVALID_HANDLE_VALUE) {
    WIN32_FIND_DATAW dir_ffd = ffd;
    BOOL have_next;
    
    s = append_text(path_end, path_buffer_end, L"\\", NULL);
    s = append_text(s, path_buffer_end, dir_ffd.cFileName, NULL);
    *s = L'\0';
    context->path_end = s;
    
    have_next = find_next_directory(&hFind, &ffd);
    if(have_next) {
      s = append_text(indent_start, indent_buffer_end, context->indent_more, NULL);
      *s = L'\0';
    }
    else {
      if(InterlockedOr(&interrupted, FALSE)) {
        FindClose(hFind);
        hFind = INVALID_HANDLE_VALUE;
        break;
      }
      
      s = append_text(indent_start, indent_buffer_end, context->indent_last, NULL);
      *s = L'\0';
    }
    
    write_unicode(context->indent_buffer);
    
    write_file_link(context->path, NULL, dir_ffd.cFileName, NULL, TRUE);
    
    if(dir_ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      print_reparse_point_info(context->path, dir_ffd.dwReserved0, TRUE);
      
      printf("\n");
    }
    else {
      printf("\n");
      
      if(have_next) {
        s = append_text(indent_start, indent_buffer_end, context->indent_more_sub, NULL);
        *s = L'\0';
      }
      else {
        s = append_text(indent_start, indent_buffer_end, context->indent_last_sub, NULL);
        *s = L'\0';
      }
      
      context->indent_end = s;
      
      if(context->path_end < path_buffer_end) {
        show_directory_tree_helper(context);
      }
    }
  }
}

static void show_directory_tree(void) {
  struct show_tree_t context[1];
  DWORD length;
  
  memset(context, 0, sizeof(context));
  
  //context->indent_more     = L"+---";
  //context->indent_more_sub = L"|   ";
  //context->indent_last     = L"\\---";
  //context->indent_last_sub = L"    ";
  
  context->indent_more     = L"\x251C\x2500\x2500\x2500";
  context->indent_more_sub = L"\x2502   ";
  context->indent_last     = L"\x2514\x2500\x2500\x2500";
  context->indent_last_sub = L"    ";
  
  length = GetCurrentDirectoryW(ARRAYSIZE(context->path) - 1, context->path);
  
  printf("\n");
  write_path_links(context->path, 0);
  printf("\n");
  
  if(length > 0 && context->path[length - 1] == L'\\')
    --length;
    
    
  context->indent_end = context->indent_buffer;
  context->path_end = context->path + length;
  
  show_directory_tree_helper(context);
}

static void list_drives(void) {
  wchar_t drive[4] = L"A:\\";
  wchar_t volume_name[MAX_PATH];
  wchar_t text[MAX_PATH];
  int i;
  DWORD drives_available = GetLogicalDrives();
  
  printf("\n");
  
  for(i = 0; i < 32; ++i) {
    if(drives_available & (1 << i)) {
      drive[0] = (wchar_t)(L'A' + i);
      
      if(GetVolumeInformationW(drive, volume_name, ARRAYSIZE(volume_name), NULL, NULL, NULL, NULL, 0)) {
      
        StringCbPrintfW(text, sizeof(text), L"%s (%c:)", volume_name, L'A' + i);
        
        printf("  ");
        write_file_link(drive, NULL, text, NULL, TRUE);
        printf("\n");
      }
      
    }
  }
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
      hyper_console_start_link(s);
      hyper_console_set_link_input_text(s);
      *e = ch;
      
      while(s != e)
        putwchar(*s++);
        
      fflush(stdout);
      hyper_console_end_link();
    }
  }
  
  free(str);
}

static int hex(wchar_t ch) {
  if(ch >= L'0' && ch <= L'9')
    return ch - L'0';
    
  if(ch >= L'a' && ch <= L'f')
    return 10 + ch - L'a';
    
  if(ch >= L'A' && ch <= L'F')
    return 10 + ch - L'A';
    
  return -1;
}

static void change_color(const wchar_t *arg) {
  int bg;
  int fg;
  
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
  
  if(!GetConsoleScreenBufferInfo(hStdout, &csbi)) {
    printf("GetConsoleScreenBufferInfo failed.\n");
    return;
  }
  
  while(*arg == L' ')
    ++arg;
    
  if(first_word_equals(arg, L"/?")) {
    printf(
      "Change the default foreground and background color of the console window.\n"
      "\n"
      "color [attr]\n"
      "\n"
      "  attr   Specifies the color attributes.\n"
      "\n"
      "The color attributes are a 2-digit hex number. "
      "The first digit specifies background, the second forground. "
      "Each digit can be one of\n"
      "\n"
      "    0 = black          8 = dark gray \n"
      "    1 = dark blue      9 = blue      \n"
      "    2 = dark green     A = green     \n"
      "    3 = teal           B = cyan      \n"
      "    4 = dark red       C = red       \n"
      "    5 = purple         D = magenta   \n"
      "    6 = ochre          E = yellow    \n"
      "    7 = light gray     F = white     \n"
      "\n");
      
    printf("Example: ");
    write_simple_link(L"red text on white background", L"color fc", L"color fc");
    printf("\n");
    
    for(bg = 0; bg < 16; ++bg) {
      printf(" ");
      
      for(fg = 0; fg < 16; ++fg) {
        SetConsoleTextAttribute(hStdout, csbi.wAttributes);
        printf(" ");
        
        if(fg != bg) {
          wchar_t link[10];
          
          StringCbPrintfW(link, sizeof(link), L"color %x%x", bg, fg);
          fflush(stdout);
          hyper_console_start_link(link);
          hyper_console_set_link_input_text(link);
          hyper_console_set_link_color((WORD)((bg << 4) | fg));
          
          SetConsoleTextAttribute(hStdout, (WORD)((bg << 4) | fg));
          printf("%x%x", bg, fg);
          
          fflush(stdout);
          hyper_console_end_link();
        }
        else {
          printf("  ");
        }
      }
      
      SetConsoleTextAttribute(hStdout, csbi.wAttributes);
      printf("\n");
    }
  }
  else {
    bg = hex(*arg);
    if(bg >= 0) {
      ++arg;
      fg = hex(*arg);
      
      if(fg >= 0) {
        ++arg;
        while(*arg == L' ')
          ++arg;
          
        if(*arg == L'\0') {
          char cmd[10];
          
          StringCbPrintfA(cmd, sizeof(cmd), "color %x%x", bg, fg);
          
          system(cmd);
//          if(fg == bg) {
//            printf("Foreground and background colors must differ.");
//            return;
//          }
//
//          if(!SetConsoleTextAttribute(hStdout, (bg << 4) | fg)) {
//            printf("SetConsoleTextAttribute failed.\n");
//            return;
//          }
//
//          printf("New color was applied.\n");

          return;
        }
      }
    }
  }
  
  {
    wchar_t link[10];
    
    StringCbPrintfW(link, sizeof(link), L"color %02x", (unsigned)csbi.wAttributes);
    
    printf("Current setting is ");
    fflush(stdout);
    hyper_console_start_link(link);
    hyper_console_set_link_input_text(link);
    
    printf("color %02x", (unsigned)csbi.wAttributes);
    fflush(stdout);
    
    hyper_console_end_link();
    printf(".\n");
  }
}

static void open_document(const wchar_t *arg) {

  const wchar_t *filename;
  uintptr_t success_flag;
  
  while(*arg == L' ')
    ++arg;
    
  filename = arg;
  
  success_flag = (uintptr_t)ShellExecuteW(NULL, NULL, filename, NULL, NULL, SW_SHOW);
  
  if(success_flag <= 32) {
    printf("Error %d in ShellExecuteW.\n", (int)success_flag);
  }
}

static void goto_bottom(void) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
  
  printf("Scrolling down.\n");
  
  if(GetConsoleScreenBufferInfo(hStdOut, &csbi)) {
    COORD pos;
    pos.X = 0;
    pos.Y = csbi.dwSize.Y - 2; // one line is added before next prompt
    SetConsoleCursorPosition(hStdOut, pos);
  }
}

static void show_secret_help_callback(void *dummy) {
  //show_help();
  printf("Type something and finish with <ENTER> or <ESCAPE>.\n");
}

static BOOL secret_key_event_filter(void *context, const KEY_EVENT_RECORD *er) {
  if(er->bKeyDown) {
    if(er->wVirtualKeyCode == VK_F1) {
      hyper_console_interrupt(show_secret_help_callback, NULL);
      return TRUE;
    }
  }
  return FALSE;
}

static void ask_secret(void) {
  struct hyper_console_settings_t settings;
  wchar_t *str;
  
  memset(&settings, 0, sizeof(settings));
  settings.size = sizeof(settings);
  //settings.need_more_input_predicate = need_more_input_predicate;
  //settings.auto_completion = auto_completion;
  //settings.line_continuation_prompt = L"...>";
  settings.key_event_filter = secret_key_event_filter;
  //settings.first_tab_column = 4;
  settings.flags = HYPER_CONSOLE_NO_ECHO;
  
  printf("Hidden input:");
  str = hyper_console_readline(&settings);
  if(str) {
    write_unicode(L"you typed '");
    write_unicode(str);
    write_unicode(L"'\n");
  }
}

static void ask_secret_callback(void *dummy) {
  ask_secret();
}

static void show_help(void) {

  printf("Available commands\n");
  
  write_simple_link(L"scroll to window bottom", L"bottom", L"bottom");
  printf("\t Advance cursor to bottom of the console buffer.\n");
  
  write_simple_link(L"change directory", L"cd", L"cd");
  printf("\t Get or change the current directory.\n");
  
  write_simple_link(L"change and list directory", L"cd+dir .", L"cd+dir");
  printf("\t Change directory and then list its content.\n");
  
  write_simple_link(L"clear screen", L"cls", L"cls");
  printf("\t Clear the screen.\n");
  
  write_simple_link(L"get or set console color", L"color /?", L"color");
  printf("\t Modify the console color.\n");
  
//  write_simple_link(L"debug information", L"debug", L"debug");
//  printf("\t Show the current console buffer status.\n");

  write_simple_link(L"list current directory", L"dir", L"dir");
  printf("\t List the content of the current directory.\n");
  
  write_simple_link(L"list drives", L"drives", L"drives");
  printf("\t List all available drives.\n");
  
  write_simple_link(L"command help", L"help", L"help");
  printf("\t Display this help ");
  write_simple_link(L"Keyboard shortcut for 'help'", L"help", L"[F1]");
  printf(".\n");
  
  write_simple_link(L"Lorem ipsum dolor sit...", L"lorem", L"lorem");
  printf("\t Print some text full of dummy links (for testing purposes).\n");
  
  write_simple_link(L"multi-line input", L"multi", L"multi");
  printf("\t Switch to multi-line input mode.\n");
  
  write_simple_link(L"exit program", L"quit", L"quit");
  printf("\t Exit the program.\n");
  
  write_simple_link(L"open .", L"open .", L"open");
  printf("\t Open an arbitrary file or directory.\n");
  
  write_simple_link(L"run pause", L"run pause", L"run");
  printf("\t Execute an arbitrary command.\n");
  
  write_simple_link(L"Echo a secret", L"secret", L"secret");
  printf("\t Input a secret (hidden input) ");
  write_simple_link(L"Keyboard shortcut for 'secret'", L"secret", L"[F2]");
  printf(".\n");
  
  write_simple_link(L"single-line input", L"single", L"single");
  printf("\t Switch to single-line input mode (default).\n");
  
  write_simple_link(L"show directory tree", L"tree", L"tree");
  printf("\t Show the current directory tree.\n");
  
  printf("\nYou can use keyboard shortcuts Ctrl+C, Ctrl+X, Ctrl+V to access the clipboard.\n");
}

static void show_help_callback(void *dummy) {
  show_help();
}

static void handle_sigint(int sig) {
  signal(sig, handle_sigint);
  
  InterlockedExchange(&interrupted, TRUE);
}

static BOOL need_more_input_predicate(void *context, const wchar_t *buffer, int len, int cursor_pos) {
  int k = 0;
  while(len-- > 0) {
    switch(*buffer++) {
      case L'(':
        ++k;
        break;
      case L')':
        --k;
        break;
    }
  }
  
  return k != 0;
}

static wchar_t **auto_completion(void *context, const wchar_t *buffer, int len, int cursor_pos, int *completion_start, int *completion_end) {
  static const wchar_t *all_words[] = {
    L"bottom",
    L"cd",
    L"cd+dir",
    L"cls",
    L"color",
    L"dir",
    L"drives",
    L"help",
    L"lorem",
    L"multi",
    L"open",
    L"quit",
    L"run",
    L"secret",
    L"single",
    L"tree"
  };
  const size_t num_words = sizeof(all_words) / sizeof(all_words[0]);
  
  int word_start = cursor_pos;
  int word_end = cursor_pos;
  wchar_t **results;
  wchar_t **next;
  int i;
  
  while(word_start > 0 && buffer[word_start - 1] > L' ')
    --word_start;
    
  while(word_end < len && buffer[word_end] > L' ')
    ++word_end;
    
  *completion_start = word_start;
  *completion_end = word_end;
  
  results = hyper_console_allocate_memory(sizeof(wchar_t*) * (num_words + 1));
  if(!results)
    return NULL;
    
  next = results;
  for(i = 0; i < num_words; ++i) {
    size_t word_len = wcslen(all_words[i]);
    if(word_len < cursor_pos - word_start)
      continue;
      
    if(0 == memcmp(all_words[i], buffer + word_start, (cursor_pos - word_start) * sizeof(wchar_t))) {
      *next = hyper_console_allocate_memory(sizeof(wchar_t) * (word_len + 1));
      if(*next) {
        memcpy(*next, all_words[i], sizeof(wchar_t) * (word_len + 1));
        ++next;
      }
    }
  }
  
  *next = NULL;
  return results;
}

static BOOL key_event_filter(void *context, const KEY_EVENT_RECORD *er) {
  if(er->bKeyDown) {
    switch(er->wVirtualKeyCode) {
      case VK_F1:
        hyper_console_interrupt(show_help_callback, NULL);
        return TRUE;
      
      case VK_F2:
        hyper_console_interrupt(ask_secret_callback, NULL);
        return TRUE;
    }
  }
  return FALSE;
}

int main() {
  wchar_t *str = NULL;
  struct hyper_console_settings_t settings;
  
  memset(&settings, 0, sizeof(settings));
  settings.size = sizeof(settings);
  settings.default_input = L"help";
  settings.history = hyper_console_history_new(0);
  settings.need_more_input_predicate = need_more_input_predicate;
  settings.auto_completion = auto_completion;
  settings.line_continuation_prompt = L"...>";
  settings.key_event_filter = key_event_filter;
  settings.first_tab_column = 4;
  
  signal(SIGINT, handle_sigint);
  
  hyper_console_init_hyperlink_system();
  
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
    InterlockedExchange(&interrupted, FALSE);
    
    //printf("\ntype ");
    //write_simple_link(L"need help?", L"help", L"something");
    //printf(": ");
    printf("\n");
    write_current_directory_path();
    printf(">");
    
    hyper_console_free_memory(str);
    str = hyper_console_readline(&settings);
    if(!str)
      continue;
      
    if(wcscmp(str, L"multi") == 0) {
      printf("Switching to multi-line mode. Try entering 'f( <ENTER> ) <ENTER>'\n");
      settings.flags |= HYPER_CONSOLE_FLAGS_MULTILINE;
      continue;
    }
    
    if(wcscmp(str, L"single") == 0) {
      printf("Switching to single-line mode.\n");
      settings.flags &= ~HYPER_CONSOLE_FLAGS_MULTILINE;
      continue;
    }
    
    if(wcscmp(str, L"bottom") == 0) {
      goto_bottom();
      continue;
    }
    
    if(wcscmp(str, L"dir") == 0) {
      list_directory();
      continue;
    }
    
    if(first_word_equals(str, L"cd")) {
      change_directory(str + 2);
      continue;
    }
    
    if(first_word_equals(str, L"cd+dir")) {
      change_directory(str + 6);
      list_directory();
      continue;
    }
    
    if(wcscmp(str, L"drives") == 0) {
      list_drives();
      continue;
    }
    
    if(wcscmp(str, L"tree") == 0) {
      show_directory_tree();
      continue;
    }
    
    if(first_word_equals(str, L"cls")) {
      system("cls");
      continue;
    }
    
    if(first_word_equals(str, L"color")) {
      change_color(str + 5);
      continue;
    }
    
//    if(wcscmp(str, L"debug") == 0) {
//      hyperlink_system_print_debug_info();
//      continue;
//    }

    if(first_word_equals(str, L"open")) {
      open_document(str + 4);
      continue;
    }
    
    if(first_word_equals(str, L"run")) {
      _wsystem(str + 3);
      continue;
    }
    
    if(first_word_equals(str, L"secret")) {
      ask_secret();
      continue;
    }
    
    if(wcscmp(str, L"help") == 0) {
      show_help();
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
  
  hyper_console_free_memory(str);
  hyper_console_done_hyperlink_system();
  hyper_console_history_free(settings.history);
  
  return 0;
}
