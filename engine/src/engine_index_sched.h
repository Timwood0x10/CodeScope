#ifndef ENGINE_INDEX_SCHED_H
#define ENGINE_INDEX_SCHED_H

#include <atomic>
#include <cstdint>

namespace engine_index_sched
{

// Parse-phase coordination globals. Reset at the start of every
// engine_index_project call; engine_index_files does not use them.
// engine_index_project is invoked sequentially from the FFI thread,
// so the globals are never concurrently re-initialised.
extern std::atomic<bool> g_parse_done;
extern std::atomic<uint32_t> g_active_parse_threads;

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
