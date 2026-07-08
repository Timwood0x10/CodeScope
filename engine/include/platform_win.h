#ifndef PLATFORM_WIN_H
#define PLATFORM_WIN_H

// ──────────────────────────────────────────────────────────────────
// Windows Platform Compatibility Header
//
// Centralizes Windows-specific adaptations for POSIX APIs that have no
// direct C++ standard-library equivalent. This header is a no-op on
// non-Windows platforms. On Windows, include this BEFORE any other
// system headers that reference dlfcn / sys/resource / sys/wait.
//
// Provides:
//   - dlfcn (dlopen/dlsym/dlclose) — implemented in platform_win.cpp
//   - sys/resource (getrusage/rusage) — implemented in platform_win.cpp
//   - sys/wait (waitpid/WNOHANG/WIFEXITED) — implemented in platform_win.cpp
//   - Common constants (PATH_MAX, F_OK, R_OK, W_OK, X_OK)
//
// NOTE: POSIX file I/O functions (open/read/write/close/access/unlink/etc.)
// are NOT macro-mapped here. Object-like macros like `#define close _close`
// corrupt C++ method names (e.g. GraphStore::close() → GraphStore::_close())
// causing link errors. Source files that need POSIX file I/O should use
// std::ifstream / std::ofstream (portable) or call _open/_read/_close
// directly behind an `#ifdef _WIN32` branch.
// ──────────────────────────────────────────────────────────────────

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <psapi.h>
#include <libloaderapi.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <process.h> // _spawnlp, _P_WAIT for fork()+execlp() replacement
#include <sys/stat.h>
#include <basetsd.h>

// ─── Type Definitions ───────────────────────────────────────────
typedef SSIZE_T ssize_t;

// ─── Path Length ────────────────────────────────────────────────
// MAX_PATH (260) is dangerously small for a code-indexing tool that walks
// arbitrary repository trees. Use 4096 (Linux default) for safety.
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// ─── File Access Modes ──────────────────────────────────────────
#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

// ─── dlfcn.h Compatibility ──────────────────────────────────────
// Implemented in platform_win.cpp
#define RTLD_LAZY 0
#define RTLD_NOW 0
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0

void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);

// ─── sys/resource.h Compatibility ───────────────────────────────
// Implemented in platform_win.cpp
#define RUSAGE_SELF 0

struct rusage {
	long ru_maxrss;
};

int getrusage(int who, struct rusage *usage);

// ─── sys/wait.h Compatibility ───────────────────────────────────
// Implemented in platform_win.cpp
#define WNOHANG 1
#define WIFEXITED(status) (((status) & 0xff) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

int waitpid(int pid, int *status, int options);

#endif // _WIN32
#endif // PLATFORM_WIN_H
