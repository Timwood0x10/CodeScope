#ifndef POSIX_COMPAT_H
#define POSIX_COMPAT_H

// POSIX declarations for Unix-like systems (macOS, Linux).
// On Windows, use platform_win.h which provides rusage/waitpid compat.

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#endif // POSIX_COMPAT_H
