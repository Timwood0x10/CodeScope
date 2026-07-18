# Memory-Bulk Index Path Optimization Plan

**Scope**: `engine/src/engine_index_project.cpp` + `engine/src/store/`
**Goal**: Small modules (≤ 2,000 files / ≤ 50k nodes) bypass the streaming
`BoundedQueue` + single-writer path and aggregate `FileResult` in-memory,
flushing once via `insertFileResultBatch`. Large modules continue to use the
existing streaming pipeline unchanged.

**Hard constraint (non-negotiable)**: The current data-processing logic must
NOT be modified. Specifically:
- Parse logic (`parse_worker_fn`, tree-sitter invocation, IR translation,
  metrics computation) — read-only.
- Storage logic (`insertFileResultBatch`, `buildGraph`, `populateSymbols`,
  `resolveStagedMetrics`, `createIndexes`, CSR build, FTS build) — read-only.
- Read logic (all `findCallers`, `findCallees`, query paths) — read-only.

**Allowed changes**: Reorganize the orchestration in
`engine_index_project.cpp` so that for small modules the parse workers write
`FileResult` directly into an in-memory aggregator (one vector per worker,
merged at end) instead of pushing through `BoundedQueue` to a single writer.
The aggregator flushes via the existing `insertFileResultBatch` in one call,
wrapped in `BulkPragmaGuard`. No new SQL, no new schema, no new storage
methods. Pure scheduling/orchestration optimization.

---

## Conformance to `plan/rules/code_rules.md`

- **File size limit (≤ 1000 lines)**: New file `engine/src/store/store_membulk.h`
  (~80 lines) + `.cpp` (~250 lines). Both well under the limit.
  `engine_index_project.cpp` is currently 1247 lines — it is already over the
  limit. **This plan must NOT increase its line count.** The new branch logic
  must be extracted into `store_membulk.cpp` so `engine_index_project.cpp`
  stays at or below current length.
- **English comments, Google C++ Style, C++23**: All new code follows.
- **RAII, no raw new/delete**: `std::vector<FileResult>`, `std::mutex`,
  `std::thread` — all RAII.
- **No magic numbers**: `kMemBulkFileThreshold = 2000`,
  `kMemBulkNodeEstimate = 5`, `kMemBulkFlushBatchSize = 500`.
- **Error handling — no silent failures, full trace chain**: Every failure
  logs `[module=store_membulk, method=<fn>]` + SQLite error, and returns
  `false` / propagates `writer_error`.
- **Public function doc comments**: Every public method on the new
  `MemBulkAggregator` class has a `@param`/`@return`/`@throws` block.
- **No git commit** (per global memory + `code_rules.md`).

---

## Current Bottleneck Analysis (from user's benchmark)

| Module | Files | Workers | parse | buildGraph | Issue |
|---|---|---|---|---|---|
| compiler | 2,014 | 1 | 9,364ms (83%) | 1,619ms (14%) | parse-bound, single worker |
| src | 4,503 | 1→13 | 7,789ms→13,607ms | 1,639ms→5,512ms | **rebalance made it slower** |
| library | 1,699 | 1→14 | fail→22,012ms | fail→6,104ms | **DB lock conflict on first try** |

**Root causes identified**:
1. `BoundedQueue` (capacity = `max(2*hardware_concurrency, 8)`,
   `engine_index_project.cpp:315-316`) adds mutex/cond_var overhead per file.
2. Single writer thread holds one SQLite transaction for the entire parse
   phase (`engine_index_project.cpp:336-389`). Multi-module parallel runs
   hit SQLite WAL contention → "DB lock conflict".
3. `codescope-parallel.sh:248-282` rebalance logic kills the running
   `index_module` and restarts it with more workers. The restart discards
   partial in-memory state and re-parses from scratch; kill of the parent
   subshell PID may leave the `codescope` child as an orphan still writing.
4. Worker count (`CODESCOPE_WORKERS`) only controls parse workers; the
   single-writer SQLite path cannot benefit from more workers once the
   queue saturates.

**What this plan changes**: Add a second code path selected by file-count
threshold. Small modules skip the queue+writer and aggregate in-memory. The
existing streaming path remains **byte-for-byte unchanged** for large
modules. The `codescope-parallel.sh` rebalance logic is removed and replaced
with proportional pre-allocation (shell-script change, not C++).

**What this plan does NOT change**:
- `parse_worker_fn` body (the parse logic itself).
- `insertFileResultBatch` SQL.
- `buildGraph`, `populateSymbolsFromGraph`, `resolveStagedMetrics`,
  `createIndexesAfterBulkLoad`, `buildCSR`, `buildFTSFromGraph` — all
  unchanged, called in the same order.
- DB schema, pragmas (except the already-existing `BulkPragmaGuard`).
- The `FileResult` struct layout.
- All read paths / query paths.

---

## Architecture (in-memory bulk path)

```
                Small module (jobs.size() <= kMemBulkFileThreshold)
                ─────────────────────────────────────────────────────
   parse worker 1 ──► thread_local vector<FileResult> ──┐
   parse worker 2 ──► thread_local vector<FileResult> ──┤  merge (lock, O(1) splice)
   parse worker N ──► thread_local vector<FileResult> ──┘
                                                          │
                                                          ▼
                                            MemBulkAggregator::flush()
                                                          │
                                            BulkPragmaGuard (sync=OFF, 64MB cache)
                                                          │
                                            g_store->beginTransaction()
                                            g_store->insertFileResultBatch(all)
                                            g_store->commitTransaction()
                                                          │
                                                          ▼
                                            buildGraph / populateSymbols
                                            / resolveStagedMetrics (unchanged)
```

vs. the existing streaming path (unchanged, large modules):

```
   parse workers ──► BoundedQueue<FileResult> ──► single writer thread
                                                     └─► insertFileResultBatch(batch=50)
                                                         inside single transaction
```

---

## Tasks

### Task 1 — Create `store_membulk.h` skeleton with class declaration

**File**: `engine/src/store/store_membulk.h` (new)

**What to do**:
- Define `namespace store { class MemBulkAggregator { ... }; }`.
- Public interface:
  - `explicit MemBulkAggregator(size_t estimated_files);`
  - `~MemBulkAggregator();`
  - `void reservePerWorker(size_t n);` — hint for thread_local vector sizing.
  - `bool flush(GraphStore &store, uint64_t project_id);` — single bulk
    insert, returns false on failure (error already logged).
  - `size_t size() const noexcept;` — total FileResult count aggregated.
  - `size_t memoryEstimateBytes() const noexcept;` — sum of
    `FileResult.records.size() * sizeof(ir::Record)` for observability.
- Private members:
  - `std::vector<FileResult> aggregated_;`
  - `std::mutex merge_mtx_;`
  - `size_t per_worker_hint_ = 64;`
- Internal helper `mergeFrom(std::vector<FileResult> &&local);` —
  lock, then `aggregated_.insert(end, make_move_iterator(local.begin()),
  make_move_iterator(local.end)); local.clear();`.
- Document the class purpose at the top: "Bypasses the streaming
  BoundedQueue for small modules. Each parse worker collects FileResult in
  a thread-local vector, then merges into the aggregator once at thread
  exit. flush() performs a single insertFileResultBatch call under
  BulkPragmaGuard."
- Include guard `#pragma once`. Forward-declare `GraphStore` and
  `FileResult` to keep the header light.
- Every public method has a doc comment block (`@param`/`@return`/`@throws`
  where applicable) per `code_rules.md` line 10.

**Acceptance criteria**:
- File exists, ≤ 100 lines, `#pragma once`, namespace `store`.
- Header compiles standalone when included from a .cpp that pulls in
  `store.h`:
  `clang++ -std=c++23 -Iengine/src -Iengine/src/store -fsyntax-only
  engine/src/store/store_membulk.h` exits 0.
- All public methods have doc comments with `@param` and `@return`.
- No magic numbers in the header (constants live in the .cpp).

---

### Task 2 — Implement `store_membulk.cpp` with flush logic

**File**: `engine/src/store/store_membulk.cpp` (new)

**What to do**:
- Include `store_membulk.h`, `store.h`, `<algorithm>`, `<mutex>`,
  `<vector>`, `<cstdio>`.
- Define named constants at the top:
  ```cpp
  namespace {
  // Flush batch size for insertFileResultBatch. Larger batches amortize
  // SQL prepare cost; 500 keeps peak memory bounded (~150 MB for 10k files).
  constexpr size_t kMemBulkFlushBatchSize = 500;
  // Initial per-worker vector capacity (most files yield 5-20 records).
  constexpr size_t kMemBulkPerWorkerHint = 64;
  }
  ```
- Constructor: `aggregated_.reserve(estimated_files);` — avoids rehash
  during merge.
- `mergeFrom`: lock `merge_mtx_`, then move-iterate `local` into
  `aggregated_`. After merge, `local.clear()` (caller reuses the buffer).
  Log on lock contention > 10 ms? No — keep it simple, no logging here.
- `flush`:
  1. `BulkPragmaGuard guard(&store);` — RAII, sets `synchronous=OFF` +
     64 MB cache, restores on destruction. Already exists
     (`store.h:572-583`).
  2. `store.beginTransaction();`
  3. Sort `aggregated_` by file size descending (same ordering as current
     `jobs` sort at `engine_index_project.cpp:280-283`) to keep the writer
     pattern consistent. **Actually skip this** — the aggregator is filled
     by workers in non-deterministic order, and `insertFileResultBatch`
     does not depend on order. Document this decision in a comment.
  4. Loop over `aggregated_` in chunks of `kMemBulkFlushBatchSize`,
     calling `store.insertFileResultBatch(project_id, chunk)` each time.
     Rationale: one giant batch would make one multi-VALUES SQL very large
     (SQLite parameter limit is `SQLITE_MAX_VARIABLE_NUMBER` = 32766);
     chunking avoids hitting that limit.
  5. On `insertFileResultBatch` returning `false`, call
     `store.rollbackTransaction()`, log
     `[module=store_membulk, method=flush] insertFileResultBatch failed:
     <store.error()>`, return `false`. **No silent failure**.
  6. On all chunks succeeding, `store.commitTransaction();` return `true`.
- `memoryEstimateBytes`: iterate `aggregated_`, sum
  `fr.records.size() * sizeof(ir::Record) +
   fr.metrics.size() * sizeof(MetricRow) +
   fr.file_path.size() + fr.language.size()`. O(N), called once at end for
  logging.
- File length budget: ≤ 250 lines including comments.

**Acceptance criteria**:
- File compiles: `clang++ -std=c++23 -Iengine/src -Iengine/src/store -c
  engine/src/store/store_membulk.cpp -o /tmp/smb.o` exits 0.
- File length ≤ 250 lines (verify with `wc -l`).
- `flush()` returns `false` on any `insertFileResultBatch` failure and
  logs an error message containing both the string
  `module=store_membulk` and the SQLite error string.
- No raw `new`/`delete` (verify with
  `grep -E '\bnew[[:space:]]' engine/src/store/store_membulk.cpp` returns
  nothing — placement new is also disallowed here).
- No magic numbers outside the named constants block.

---

### Task 3 — Add unit test for `MemBulkAggregator`

**File**: `engine/tests/test_membulk.cpp` (new)

**What to do**:
- Test 1: `EmptyFlushReturnsTrue` — aggregator with 0 files, flush on a
  freshly-opened `GraphStore` (in-memory `:memory:` DB), expect `true` and
  `size() == 0`.
- Test 2: `SingleWorkerMerge` — create aggregator with `estimated_files=4`.
  Simulate one worker by calling `mergeFrom` with a vector of 4 fake
  `FileResult` objects (each with 1 record). Verify `size() == 4`. Flush
  and verify `semantic_records` row count in DB == 4.
- Test 3: `MultiWorkerConcurrentMerge` — spawn 8 threads, each calling
  `mergeFrom` with 16 `FileResult` objects (128 total). After all join,
  verify `size() == 128`. Flush and verify DB row count.
- Test 4: `FlushFailureRollsBackAndLogs` — inject failure by closing the
  store connection mid-flush (or using a DB path that becomes
  unwritable). Verify `flush()` returns `false`. **Hard to inject
  reliably** — instead, verify by reading stderr capture during the test
  that the error message contains `module=store_membulk`.
- Use the existing test harness convention (check sibling
  `engine/tests/test_*.cpp` for the test framework in use — likely a
  vendored `minctest.h` or similar; match it exactly).
- Test file must stay ≤ 500 lines per `code_rules.md`.

**Acceptance criteria**:
- `cd engine/build-release && cmake --build . --target test_membulk &&
  ./test_membulk` exits 0.
- All 4 named tests above are present and pass.
- Test file length ≤ 500 lines.
- Tests cover: empty input, single-thread merge, multi-thread concurrent
  merge, failure path with error message verification.

---

### Task 4 — Wire in-memory bulk path into `engine_index_project`

**File**: `engine/src/engine_index_project.cpp` (modify, lines 295–620)

**What to do**:
- After the `jobs` vector is sorted and language pointers pre-loaded
  (currently ends around `engine_index_project.cpp:293`), insert a branch:

  ```cpp
  // Path selection: small modules use in-memory aggregation, large
  // modules use the streaming BoundedQueue + single writer. The
  // threshold is set so peak memory stays under ~150 MB (10k files *
  // ~15 KB FileResult). The data-processing logic (parse, store, read)
  // is identical in both paths; only the worker→writer transport differs.
  constexpr size_t kMemBulkFileThreshold = 2000;

  if (jobs.size() <= kMemBulkFileThreshold) {
      return engine_index_project_membulk(
          project_id, dir, lang_filter_set, max_file_size,
          filter, jobs, lang_ptrs, is_reindex);
  }
  // ... existing streaming path unchanged ...
  ```

- **Refactor shape**: Extract the new path into a non-member function
  `engine_index_project_membulk(...)` declared in
  `engine_internal.h` and defined in a **new** file
  `engine/src/engine_index_project_membulk.cpp`. This keeps
  `engine_index_project.cpp` from growing past its current 1247 lines
  (which already violates the 1000-line rule; this task must not make it
  worse).

- `engine_index_project_membulk`:
  1. Construct `store::MemBulkAggregator agg(jobs.size());`
  2. Define `parse_worker_fn` lambda. **This must be a verbatim copy of
     the existing `parse_worker_fn` body** (lines 395-567) EXCEPT for one
     change: where the existing code does `result_queue.push(std::move(result))`,
     the new code appends to a thread-local `std::vector<FileResult> local_buf`
     and, when `local_buf.size() >= 64` OR the worker exits the loop, calls
     `agg.mergeFrom(std::move(local_buf))`.
     - Rationale: keeps parse logic unchanged, only swaps the transport.
     - The lambda captures `agg` by reference.
  3. Determine `num_workers` exactly as the current code does (lines
     569-583) — same `CODESCOPE_WORKERS` env logic, same fallback to 4.
  4. Spawn workers, join. Same error logging as current code on thread
     spawn failure.
  5. After all workers join, call `bool ok = agg.flush(*g_store, project_id);`
     - On `!ok`, return `dupString("{\"ok\":false,\"error\":\"membulk flush failed\"}");`
       — the flush itself has already logged the detailed SQLite error.
  6. Continue into the SAME post-parse code path as the streaming version:
     `buildGraph`, `callgraph_ready` UPDATE, `populateSymbolsFromGraph`,
     `resolveStagedMetrics`, `createIndexesAfterBulkLoad`,
     `buildVectorsFromGraph`, `buildCSR`, async model/state, final JSON.
     **This post-parse code is currently inline in
     `engine_index_project` (lines 620–760). To avoid duplicating it,
     extract it into a helper `engine_index_post_parse(...)` that both
     paths call. The helper lives in
     `engine_index_project_membulk.cpp` (or a small
     `engine_index_post_parse.cpp` if file length forces it).**

- **Critically**: The streaming path (`engine_index_project.cpp:295-620`)
  must remain byte-for-byte identical EXCEPT for the insertion of the
  branch statement and the extraction of post-parse code into the shared
  helper. Use `git diff` to verify the streaming-path lines are unchanged.

**Acceptance criteria**:
- `clang++ -std=c++23 ... engine/src/engine_index_project.cpp
  engine/src/engine_index_project_membulk.cpp` compiles cleanly.
- `wc -l engine/src/engine_index_project.cpp` shows ≤ 1247 lines (must not
  increase). The new code lives in
  `engine/src/engine_index_project_membulk.cpp` (≤ 600 lines) and possibly
  `engine/src/engine_index_post_parse.cpp` (≤ 600 lines).
- Running `codescope worker <db> <dir> "" "test" 0` on a directory with
  ≤ 2000 source files produces identical `graph_nodes` count, identical
  `graph_edges` count, and identical `semantic_records` count compared to
  the same run on the `dev` branch baseline. Capture both with
  `sqlite3 <db> "SELECT COUNT(*) FROM graph_nodes; SELECT COUNT(*) FROM
  graph_edges; SELECT COUNT(*) FROM semantic_records;"` and diff.
- The streaming path (directory with > 2000 files) produces identical
  counts to `dev` baseline.
- `CODESCOPE_INDEX_MODE=fast CODESCOPE_WORKERS=4` env vars still work in
  both paths.
- Post-parse sequence (`buildGraph` → `populateSymbolsFromGraph` →
  `resolveStagedMetrics` → `createIndexesAfterBulkLoad` →
  `buildVectorsFromGraph` → `buildCSR`) is invoked in the same order in
  both paths.

---

### Task 5 — Remove rebalance logic from `codescope-parallel.sh`

**File**: `codescope-parallel.sh` (modify, lines 150-288)

**What to do**:
- Replace the entire "Step 2: Dynamic worker dispatch" block (lines
  150-288) with a **proportional pre-allocation** scheme:

  ```bash
  # Step 2: Proportional worker allocation (no rebalance)
  # Each module gets workers proportional to its file count.
  # No rebalancing — small modules use in-memory bulk path and don't
  # benefit from extra workers; large modules are already streaming.
  ```

- Compute per-module worker allocation:
  ```bash
  TOTAL_FILES_SUM=0
  while IFS=: read -r name count; do
      TOTAL_FILES_SUM=$((TOTAL_FILES_SUM + count))
  done < "$TMP_MODULES"

  declare -a MODULE_ALLOC=()
  while IFS=: read -r name count; do
      # ceil(count * TOTAL_WORKERS / TOTAL_FILES_SUM), min 1
      alloc=$(( (count * TOTAL_WORKERS + TOTAL_FILES_SUM - 1) / TOTAL_FILES_SUM ))
      [ "$alloc" -lt 1 ] && alloc=1
      MODULE_ALLOC+=("$name:$count:$alloc")
  done < "$TMP_MODULES"
  ```

- Start all modules in parallel (up to `PARALLEL` concurrent):
  ```bash
  ACTIVE=0
  for entry in "${MODULE_ALLOC[@]}"; do
      IFS=: read -r name count alloc <<< "$entry"
      (
          result=$(index_module "$name" "$count" "$alloc")
          echo "$result" >> "${DB_PREFIX}_SUMMARY.txt"
          IFS=: read -r mname ec nodes dur wkrs <<< "$result"
          echo "  [DONE] $mname → ${nodes} nodes ${dur}s (${wkrs} workers)"
          log_metric "MODULE:${mname}" "exit=${ec} nodes=${nodes} duration=${dur}s workers=${wkrs}"
      ) &
      ACTIVE=$((ACTIVE + 1))
      # If we've hit PARALLEL, wait for one to finish before starting next.
      # Simple approach: just start all and let the OS scheduler handle it.
      # Since PARALLEL == number of modules in practice, this is fine.
  done
  wait
  ```

- **Delete** the rebalance logic (lines 234-282), the
  `MODULE_STATE_DIR` tracking (lines 166-169), the
  `find_crashing_file` function call in the rebalance path, the
  `MODULE_QUEUE` array (line 155), and the metrics background monitor
  (lines 162-164) — keep the metrics monitor, it's useful. Actually,
  keep everything that's still referenced; only remove the rebalance
  branch.

- **Keep** the per-file quarantine step (Step 3, lines 292-330) unchanged.
  It's a different feature (binary-search for crashing files) and
  orthogonal to this optimization.

**Acceptance criteria**:
- `bash -n codescope-parallel.sh` exits 0 (syntax check).
- `shellcheck codescope-parallel.sh` produces no new warnings compared to
  the `dev` baseline (run `shellcheck` on both, diff the outputs).
- Running the script on the user's 3-module test project completes without
  the `[REBALANCE]` log line ever appearing.
- The script's final summary reports the same total node count as the
  previous run (72,493) when run on the same project, OR a higher count
  if the rebalance bug was previously losing nodes.
- No `kill "$old_pid"` remains in the script (rebalance was the only user
  of process killing). Verify with
  `grep -n 'kill ' codescope-parallel.sh` returning nothing.

---

### Task 6 — Add integration benchmark for in-memory bulk path

**File**: `benchmarks/run_membulk_benchmark.sh` (new)

**What to do**:
- Create a benchmark script that:
  1. Runs `codescope worker` on a directory with exactly 2,000 source
     files (use a symlink farm if needed, or pick the `compiler` module
     which has 2,014 files — trim 14 with a temp dir if needed).
  2. Runs the same workload 3 times each with two configurations:
     - **Baseline (streaming path)**: temporarily lower
       `kMemBulkFileThreshold` to 0 by setting
       `CODESCOPE_FORCE_STREAMING=1` env var (add this read in Task 4:
       if set, always take the streaming branch regardless of file
       count — useful for A/B testing).
     - **New (in-memory bulk path)**: default, `CODESCOPE_FORCE_STREAMING`
       unset.
  3. Records wall time, peak RSS, graph_nodes count, graph_edges count.
  4. Outputs a Markdown table to `benchmarks/results/membulk_bench.md`.
- The benchmark must clean up all `*.db*` files between runs to avoid
  WAL contamination.

**Acceptance criteria**:
- Script runs to completion on macOS (the user's platform) without
  errors.
- The resulting Markdown table shows the in-memory bulk path achieving
  ≥ 20% faster wall time than the streaming path on the 2,000-file
  workload. (Target: parse phase 9,364ms → ≤ 7,500ms.)
- The resulting table shows identical node/edge counts between the two
  paths (proving no data loss).
- Script is ≤ 200 lines.

---

### Task 7 — Verify end-to-end parity and update docs

**Files to modify**:
- `docs/optimization/optimization-english.md` (add section)
- `docs/optimization/optimization-chinese.md` (add section)
- `CHANGELOG.md` (add entry under `Unreleased`)

**What to do**:
- Run the full parallel indexer on the user's 10,631-file test project
  with the new code:
  ```bash
  CODESCOPE_WORKERS=16 CODESCOPE_PARALLEL=3 \
    ./codescope-parallel.sh /path/to/project /tmp/bench_membulk
  ```
- Capture: total wall time, total nodes, total edges, per-module timings,
  peak RSS across all `codescope` processes.
- Compare against the user's current benchmark:
  - Wall time: 28s → target ≤ 18s.
  - Total nodes: 72,493 → must be ≥ 72,493 (no regressions; the
    rebalance bug fix may increase this).
  - Total edges: 40,751 → must be ≥ 40,751.
  - "DB 锁冲突" failures: 1 → must be 0.
- Write up the results in both `optimization-english.md` and
  `optimization-chinese.md` under a new section "Memory-Bulk Index Path
  (2026-07-18)". Include: motivation, approach, A/B numbers, peak memory
  verification, no-regression verification.
- Add `CHANGELOG.md` entry:
  ```
  ### Changed
  - Small modules (≤ 2000 files) now use an in-memory bulk aggregation
    path, bypassing the streaming BoundedQueue. 20%+ faster parse phase
    for small modules. Large modules continue to use the streaming path
    unchanged.
  - Removed dynamic worker rebalance from `codescope-parallel.sh`.
    Replaced with proportional pre-allocation. Eliminates "DB lock
    conflict" failures on concurrent module indexing.
  ```

**Acceptance criteria**:
- The 10,631-file test run completes with wall time ≤ 18s and zero DB
  lock failures.
- `git diff --stat` shows the new `store_membulk.{h,cpp}`,
  `engine_index_project_membulk.cpp`, the modified
  `engine_index_project.cpp`, the modified `codescope-parallel.sh`, the
  new test files, the new benchmark script, and the doc updates. No
  other files touched.
- `wc -l engine/src/engine_index_project.cpp` returns ≤ 1247.
- Both `optimization-english.md` and `optimization-chinese.md` contain
  the new section with concrete A/B numbers.
- `CHANGELOG.md` contains the new entry under `Unreleased`.

---

## Risk Mitigation

| Risk | Mitigation |
|---|---|
| Memory blow-up on pathological inputs (huge files with many records) | `kMemBulkFileThreshold = 2000` keeps peak ≤ ~150 MB. Add assertion in `flush()` that `aggregated_.size() <= 10 * kMemBulkFileThreshold` (sanity bound) — log and continue if exceeded. |
| Thread-local vector merge contention | Workers merge only once at thread exit (not per-file), so mutex is held ~N times total where N = num_workers. Negligible. |
| Multi-VALUES SQL hitting `SQLITE_MAX_VARIABLE_NUMBER` (32766 params) | Task 2 chunks flush into batches of `kMemBulkFlushBatchSize = 500` files. Each file contributes ~20-50 bound params, so 500 files × 50 params = 25,000 params — under the limit with margin. Verify with a test that pushes 2000 files. |
| Regression in streaming path for large modules | Task 4's acceptance criteria include byte-for-byte diff of the streaming path lines. Task 5's criteria include node/edge count parity. |
| `engine_index_project.cpp` already over 1000-line limit | Task 4 explicitly extracts the new code into separate files and verifies `wc -l` does not increase. Long-term, the file should be split further, but that's out of scope here. |
| Rebalance removal breaks the quarantine retry loop | The quarantine step (Step 3 in the script) does NOT depend on rebalance. It independently re-runs `index_module` with `find_crashing_file`. Verify by reading the script: quarantine uses `index_module "$module_name" "0" "1"` directly, never touching the rebalance state. |

---

## Execution Order

Tasks must be executed in this order due to dependencies:

1. **Task 1** — Create `store_membulk.h` (no deps).
2. **Task 2** — Implement `store_membulk.cpp` (depends on Task 1).
3. **Task 3** — Unit tests (depends on Task 2).
4. **Task 4** — Wire into `engine_index_project` (depends on Tasks 1-3).
5. **Task 5** — Remove rebalance from shell script (independent of C++ work,
   but should land after Task 4 so the full system can be benchmarked).
6. **Task 6** — Integration benchmark (depends on Tasks 4-5).
7. **Task 7** — End-to-end verification + docs (depends on Task 6).

**Estimated effort**: 2-3 days for an engineer familiar with the codebase.

**No git commits** — all work stays in the working tree per
`code_rules.md` and global memory.
