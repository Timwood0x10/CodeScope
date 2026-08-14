#include "engine_index_sched.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace engine_index_sched
{

SchedState *g_sched_state = nullptr; // nullptr = static mode

std::atomic<bool> g_parse_done{ false };
std::atomic<uint32_t> g_active_parse_threads{ 0 };

// Try to open the shared memory (env CODESCOPE_SCHED_SHM=path).
// On failure (file missing, too small, magic mismatch, mmap error)
// returns nullptr — silent fallback to static scheduling. No error
// is logged because the static path is the legitimate default.
SchedState *open_sched_state()
{
#ifndef _WIN32
	const char *path = getenv("CODESCOPE_SCHED_SHM");
	if (!path || !path[0])
		return nullptr;
	fprintf(stderr,
		"engine: opening shm path=%s "
		"[module=engine, method=open_sched_state]\n",
		path);
	// Open with O_RDWR: mmap below uses PROT_WRITE for CAS in grab_cores().
	// O_RDONLY + PROT_WRITE mmap fails with EACCES on POSIX, which would
	// silently disable dynamic scheduling.
	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr,
			"engine: shm open failed errno=%d path=%s "
			"[module=engine, method=open_sched_state]\n",
			errno, path);
		return nullptr;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 ||
	    st.st_size < static_cast<off_t>(sizeof(SchedState))) {
		close(fd);
		return nullptr;
	}
	// PROT_WRITE is required for the CAS in grab_cores(); the
	// scheduler creates the shm file with rw perms for worker procs.
	void *addr = mmap(nullptr, sizeof(SchedState), PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, 0);
	close(fd);
	if (addr == MAP_FAILED)
		return nullptr;
	auto *s = static_cast<SchedState *>(addr);
	if (s->magic != 0x53434844u) {
		munmap(addr, sizeof(SchedState));
		return nullptr;
	}
	return s;
#else
	return nullptr; // mmap-based dynamic scheduling not on Windows
#endif
}

// Try to grab up to `max_want` cores from the shared pool via CAS.
// Returns the number actually grabbed (0 if none available or static
// mode). The caller must return_cores(count) when done.
uint32_t grab_cores(uint32_t max_want)
{
	if (!g_sched_state || max_want == 0)
		return 0;
	while (true) {
		uint32_t avail = g_sched_state->available_cores.load(
			std::memory_order_relaxed);
		if (avail == 0)
			return 0;
		uint32_t take = std::min(max_want, avail);
		if (g_sched_state->available_cores.compare_exchange_weak(
			    avail, avail - take, std::memory_order_relaxed)) {
			return take;
		}
	}
}

// Return `count` cores to the shared pool. No-op in static mode.
void return_cores(uint32_t count)
{
	if (!g_sched_state || count == 0)
		return;
	g_sched_state->available_cores.fetch_add(count,
						 std::memory_order_relaxed);
}

} // namespace engine_index_sched
