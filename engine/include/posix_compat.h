#ifndef POSIX_COMPAT_H
#define POSIX_COMPAT_H

// Cross-platform declarations for POSIX APIs used by the engine.
// On macOS/Linux: includes the real POSIX headers.
// On Windows: declares functions implemented in platform_win.cpp

#ifdef _WIN32

#define RUSAGE_SELF 0
#define WNOHANG 1
#define WIFEXITED(status)  (((status) & 0xff) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

struct rusage {
    long ru_maxrss;
};

int getrusage(int who, struct rusage *usage);
int waitpid(int pid, int *status, int options);

#else
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#endif // POSIX_COMPAT_H
