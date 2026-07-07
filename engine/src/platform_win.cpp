// platform_win.cpp — Windows implementations of POSIX/compat APIs.
// Compiled only on Windows (CMake WIN32 branch).
// Provides: dlopen/dlsym/dlclose, getrusage, waitpid.

#ifdef _WIN32

#include "../include/dlfcn_compat.h"
#include "../include/posix_compat.h"

#include <windows.h>
#include <psapi.h>
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
	return (char *)"dlfcn compat: see GetLastError()";
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
		if (status)
			*status = 0;
		return pid;
	}
	DWORD ret = WaitForSingleObject(h, (options & WNOHANG) ? 0 : INFINITE);
	if (ret == WAIT_TIMEOUT) {
		CloseHandle(h);
		return 0;
	}
	DWORD exit_code = STILL_ACTIVE;
	GetExitCodeProcess(h, &exit_code);
	CloseHandle(h);
	if (status)
		*status = (int)exit_code;
	return pid;
}

#endif // _WIN32
