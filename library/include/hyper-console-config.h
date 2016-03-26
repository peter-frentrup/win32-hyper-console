#ifndef __HYPER_CONSOLE_CONFIG_H__
#define __HYPER_CONSOLE_CONFIG_H__



#ifdef __cplusplus
#  define HYPER_CONSOLE_EXTERN_C  extern "C"
#else
#  define HYPER_CONSOLE_EXTERN_C
#endif


#ifdef __GNUC__
#  ifdef HYPER_CONSOLE_BUILD_DLL
#    define HYPER_CONSOLE_API      HYPER_CONSOLE_EXTERN_C __attribute__((cdecl, dllexport))
#  else
#    define HYPER_CONSOLE_API      HYPER_CONSOLE_EXTERN_C __attribute__((cdecl, dllimport))
#  endif
#else
#  ifdef HYPER_CONSOLE_BUILD_DLL
#    define HYPER_CONSOLE_API      HYPER_CONSOLE_EXTERN_C __declspec(dllexport)
#  else
#    define HYPER_CONSOLE_API      HYPER_CONSOLE_EXTERN_C __declspec(dllimport)
#  endif
#endif


#endif
