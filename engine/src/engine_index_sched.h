#ifndef ENGINE_INDEX_SCHED_H
#define ENGINE_INDEX_SCHED_H

#include <atomic>
#include <cstdint>

namespace engine_index_sched
{

// ─── Dynamic Scheduler Shared State ──────────────────────────────
// When CODESCOPE_SCHED_SHM points to a valid shared-memory file
// created by the Rust scheduler (see server/src/scheduler/shm.rs),
// the engine reads `available_cores` atomically and spawns temporary
// parse threads to grab idle cores from the shared pool. When the env
// var is unset or the file is invalid, the engine falls back to the
// static CODESCOPE_WORKERS allocation — identical to pre-dynamic
// behaviour. The static path is the default for non-scheduler
// invocations (single-process `codescope index`, tests, etc.).
//
// Layout MUST match server/src/scheduler/shm.rs SchedState. The C++
// side only reads atomic fields and performs a CAS on
// available_cores in grab_cores(); no other writes.
// SAFETY: the scheduler creates and mmap's the file before spawning
// the worker subprocess, so g_sched_state is non-null only inside a
// worker that the scheduler deliberately started.
struct SchedState {
	uint32_t magic; // 0x53434844 ("SCHD")
	uint32_t version;
	std::atomic<uint32_t> total_cores;
	std::atomic<uint32_t> available_cores;
	std::atomic<uint32_t> active_workers;
	std::atomic<uint32_t> generation;
	std::atomic<uint32_t> mem_limit_mb;
	std::atomic<uint32_t> current_mem_mb;
	std::atomic<uint32_t> aggressive;
	uint32_t worker_count;
	uint32_t reserved[8];
	std::atomic<uint32_t> worker_status[64];
	std::atomic<uint32_t> worker_cores[64];
};

// Global shared-memory handle; nullptr = static mode. Owned by the
// scheduler (mmap'd file), never freed by the engine.
extern SchedState *g_sched_state;

// Parse-phase coordination globals. Reset at the start of every
// engine_index_project call; engine_index_files does not use them.
// engine_index_project is invoked sequentially from the FFI thread,
// so the globals are never concurrently re-initialised.
extern std::atomic<bool> g_parse_done;
extern std::atomic<uint32_t> g_active_parse_threads;

// Try to open the shared memory (env CODESCOPE_SCHED_SHM=path).
// On failure (file missing, too small, magic mismatch, mmap error)
// returns nullptr — silent fallback to static scheduling. No error
// is logged because the static path is the legitimate default.
SchedState *open_sched_state();

// Try to grab up to `max_want` cores from the shared pool via CAS.
// Returns the number actually grabbed (0 if none available or static
// mode). The caller must return_cores(count) when done.
uint32_t grab_cores(uint32_t max_want);

// Return `count` cores to the shared pool. No-op in static mode.
void return_cores(uint32_t count);

// ─── Constants ─────────────────────────────────────────────────
constexpr uint64_t kMaxFileSize = 5 * 1024 * 1024; // 5 MB default

// Fail-fast threshold: a file whose parse has failed at least this many
// times is permanently skipped on subsequent index runs. 1 = skip on the
// first failure (strict fail-fast, never retry). Override with the
// CODESCOPE_FAIL_RETRY_MAX env var (clamped to >= 1).
constexpr int kDefaultFailRetryMax = 1;

// Default number of parse workers used by engine_index_project when
// CODESCOPE_WORKERS is unset. The parse phase (tree-sitter + visitor +
// metrics) is pure CPU and does not touch SQLite (a single writer thread
// batches inserts), so on a 14-core machine 4 workers wasted ~70% of the
// cores; raising to 8 dropped the rustc full-index wall clock from ~42s to
// ~34.5s (-18%). 8 is the measured sweet spot: 8 -> 12 gives no further
// gain because the SQLite writer thread becomes the throughput ceiling, and
// it still leaves cores for other processes. Users can still override via
// the CODESCOPE_WORKERS env var.
constexpr int kDefaultParseWorkers = 8;

} // namespace engine_index_sched

#endif // ENGINE_INDEX_SCHED_H
