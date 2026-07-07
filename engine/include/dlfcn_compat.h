#ifndef DLFCN_COMPAT_H
#define DLFCN_COMPAT_H

// Platform-compatible dynamic library loading.
// - macOS/Linux: <dlfcn.h> (dlopen, dlsym, dlclose)
// - Windows:     <libloaderapi.h> (LoadLibrary, GetProcAddress, FreeLibrary)

#ifdef _WIN32
#include <libloaderapi.h>
// Map dlopen-family calls to Win32 equivalents.
// Grammar .so/.dll loading happens via platform-specific code path.
// These typedefs are provided for source compatibility.
#define RTLD_LAZY   0
#define RTLD_NOW    0
#define RTLD_LOCAL  0
#define RTLD_GLOBAL 0

static inline void *dlopen(const char *file, int mode) {
    (void)mode;
    // For .so files on Windows: the engine loads grammars as .dll
    return LoadLibraryA(file);
}

static inline void *dlsym(void *handle, const char *name) {
    return (void *)GetProcAddress((HMODULE)handle, name);
}

static inline int dlclose(void *handle) {
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}

static inline const char *dlerror(void) {
    return "dlfcn compat: see GetLastError()";
}
#else
#include <dlfcn.h>
#endif

#endif // DLFCN_COMPAT_H
