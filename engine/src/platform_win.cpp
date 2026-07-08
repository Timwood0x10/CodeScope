// platform_win.cpp — Windows implementations of POSIX/compat APIs.
// Compiled only on Windows (CMake WIN32 branch).
// Provides: dlopen/dlsym/dlclose, getrusage, waitpid.

#ifdef _WIN32

#include "platform_win.h"
// platform_win.h already includes windows.h, psapi.h, etc.

#include <errno.h>

// ─── dlopen / dlsym / dlclose ──────────────────────────────────
void *dlopen(const char *file, int mode)
{
	(void)mode;
	return (void *)LoadLibraryA(file);
}
void *dlsym(void *handle, const char *name)
{
	return (void *)GetProcAddress((HMODULE)handle, name);
}
int dlclose(void *handle)
{
	return FreeLibrary((HMODULE)handle) ? 0 : -1;
}
char *dlerror(void)
{
	// Return a stable, writable buffer — callers may mutate or the C string
	// API may expect modifiable memory. thread_local avoids races between
	// threads without needing a lock.
	static thread_local char buf[] = "dlfcn compat: see GetLastError()";
	return buf;
}

// ─── getrusage ──────────────────────────────────────────────────
int getrusage(int who, struct rusage *usage)
{
	(void)who;
	PROCESS_MEMORY_COUNTERS pmc;
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
		usage->ru_maxrss = (long)(pmc.PeakWorkingSetSize / 1024);
		return 0;
	}
	return -1;
}

// ─── waitpid ────────────────────────────────────────────────────
int waitpid(int pid, int *status, int options)
{
	HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE,
			       (DWORD)pid);
	if (!h) {
		// Process not found / no access — mirror POSIX failure semantics
		// (return -1, errno=ECHILD) instead of falsely reporting success.
		errno = ECHILD;
		if (status)
			*status = 0;
		return -1;
	}
	DWORD ret = WaitForSingleObject(h, (options & WNOHANG) ? 0 : INFINITE);
	if (ret == WAIT_TIMEOUT) {
		CloseHandle(h);
		return 0;
	}
	DWORD exit_code = STILL_ACTIVE;
	if (!GetExitCodeProcess(h, &exit_code)) {
		CloseHandle(h);
		errno = ECHILD;
		if (status)
			*status = 0;
		return -1;
	}
	CloseHandle(h);
	// Encode exit status in POSIX wait-status format so WIFEXITED/WEXITSTATUS
	// macros work: normal exit → (code << 8), low byte 0.
	if (status)
		*status = (int)((exit_code & 0xFF) << 8);
	return pid;
}

#endif // _WIN32
