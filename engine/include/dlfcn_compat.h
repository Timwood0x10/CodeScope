#ifndef DLFCN_COMPAT_H
#define DLFCN_COMPAT_H

// Dynamic library loading for Unix-like systems (macOS, Linux).
// On Windows, use platform_win.h which provides dlopen/dlsym/dlclose compat.

#ifndef _WIN32
#include <dlfcn.h>
#endif

#endif // DLFCN_COMPAT_H
