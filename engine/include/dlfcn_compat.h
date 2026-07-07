#ifndef DLFCN_COMPAT_H
#define DLFCN_COMPAT_H

// Cross-platform dynamic library loading.
// On macOS/Linux: includes <dlfcn.h> for dlopen/dlsym/dlclose.
// On Windows: declares functions implemented in platform_win.cpp.

#ifdef _WIN32
#include <libloaderapi.h>

#define RTLD_LAZY   0
#define RTLD_NOW    0
#define RTLD_LOCAL  0
#define RTLD_GLOBAL 0

void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int   dlclose(void *handle);
char *dlerror(void);

#else
#include <dlfcn.h>
#endif

#endif // DLFCN_COMPAT_H
