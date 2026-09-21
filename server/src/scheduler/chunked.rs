// chunked.rs — opt-in chunk-level parallel indexer with work-stealing.
//
// Split out of scheduler/mod.rs (see plan/rules/code_rules.md 1000-line
// rule). OPT-IN path: entered only when CODESCOPE_CPU_DYNAMIC /
// CODESCOPE_DYNAMIC_SCHED is explicitly set; the default stays the static
// proportional allocator in index_parallel.
//
// `use super::*` is deliberate rather than lazy: this is a child module of
// `scheduler`, so it inherits the parent's private helpers (error_json,
// rebuild_csr_all_projects, canonicalize_project_dir, resolve_self_exe)
// and the shared constants. Widening those to `pub` would advertise them
// to the whole crate for a split that is pure code motion.

use super::*;

/// Chunk-level parallel indexer with work-stealing (CPU-dynamic scheduling).
///
/// OPT-IN path: entered only when `CODESCOPE_CPU_DYNAMIC` /
/// `CODESCOPE_DYNAMIC_SCHED` is explicitly set. The DEFAULT (no flag) is the
/// static proportional allocator in `index_parallel` — see the project's
/// "static by default" scheduling principle (DYNAMIC_SCHED_REDESIGN.md).
///
/// Stability: each worker writes its OWN DB (no concurrent WAL writers),
/// uses a unique 1-based project_id, and the final merge reuses the proven
/// per-unit-DB + merge_module_dbs machinery from the static path.
/// Whether a chunked run counts as complete.
///
/// The chunked path can recover, so the module path's rule (`run_complete()`:
/// no failed worker AND a successful merge) would understate it: a worker that
/// dies loses every chunk it owned (one DB per worker), and Phase 3b re-indexes
/// them with a replacement worker. A non-zero `fail` is therefore not proof of
/// missing data — the queue is the authority (`queue_complete` = every chunk
/// DONE or FAILED, where FAILED is the normal file-level outcome).
///
/// A complete index is: the merge succeeded, every chunk reached a terminal
/// state, and a recovery round that ran did not itself fail. `fail` and
/// `recovered_chunks` stay in the response either way, so the deaths are
/// visible without being reported as missing data.
fn chunked_run_complete(
    merged: bool,
    retry_worker_attempted: bool,
    retry_worker_failed: bool,
    queue_complete: bool,
) -> bool {
    // A complete index is the merge plus a fully terminal queue; the only thing
    // that can invalidate it is a recovery round that itself failed. `fail` is
    // deliberately not part of the test: a dead worker whose chunks another
    // worker (or the recovery round) finished leaves no missing data, and the
    // count stays visible in the response.
    merged && queue_complete && !(retry_worker_attempted && retry_worker_failed)
}

pub(super) fn index_parallel_chunked(
    project_dir: &str,
    total_workers: u32,
    parallel: u32,
) -> String {
    let start = Instant::now();
    let total_workers = if total_workers == 0 {
        DEFAULT_TOTAL_WORKERS
    } else {
        total_workers
    };
    let parallel = if parallel == 0 {
        DEFAULT_PARALLEL
    } else {
        parallel
    };
    // Cap parallel at total_workers (see Bug 9 fix).
    let parallel = parallel.min(total_workers).max(1);

    let project_path_buf = match canonicalize_project_dir(project_dir) {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_chunked"),
    };
    let project_path = project_path_buf.to_string_lossy().to_string();

    let exe_path = match resolve_self_exe() {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_chunked"),
    };
    let exe_str = exe_path.to_string_lossy().to_string();

    let grammars_dir =
        std::env::var("GRAMMARS_DIR").unwrap_or_else(|_| "engine/grammars".to_string());

    let run_id = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let prefix_env = std::env::var("CODESCOPE_DB_PREFIX");
    let keep_db = prefix_env.is_ok();
    let db_prefix = prefix_env.unwrap_or_else(|_| format!("/tmp/codescope_chunked_{}", run_id));
    let _ = keep_db; // keep_db consumed in run_module_worker calls below

    // ── Phase 1: discover the GLOBAL file list ───────────────
    // One walk of the whole project yields every candidate source file.
    // Unlike the static path (which plans per-module), the chunked path
    // plans over the flat global list so chunks can span directory
    // boundaries and balance by byte weight.
    let discover_json = discover::discover_files(&project_path);
    let discover_val: Value = match serde_json::from_str(&discover_json) {
        Ok(v) => v,
        Err(e) => {
            return error_json(
                &format!(
                    "discover_files parse failed: {} [module=scheduler, method=index_parallel_chunked]",
                    e
                ),
                "scheduler",
                "index_parallel_chunked",
            );
        }
    };
    if discover_val["ok"] != true {
        return discover_json;
    }
    let all_paths: Vec<String> = discover_val["files"]
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_str().map(|s| s.to_string()))
                .collect()
        })
        .unwrap_or_default();
    if all_paths.is_empty() {
        return json!({
            "ok": true,
            "project_path": project_path,
            "duration_ms": start.elapsed().as_millis() as u64,
            "success": 0,
            "fail": 0,
            "total_nodes": 0,
            "total_edges": 0,
            "total_files_indexed": 0,
            "modules": [],
            "note": "no source files found"
        })
        .to_string();
    }

    // Build FileEntry list (path + size), sorted by path — plan_chunks
    // REQUIRES the input sorted by path (see chunk_plan.rs Invariant).
    let mut files: Vec<chunk_plan::FileEntry> = all_paths
        .iter()
        .map(|p| {
            let size = std::fs::metadata(p).map(|m| m.len()).unwrap_or(0);
            chunk_plan::FileEntry {
                path: p.clone(),
                size,
            }
        })
        .collect();
    files.sort_by(|a, b| a.path.cmp(&b.path));

    // ── Phase 2: plan chunks ────────────────────────────────
    let chunks = chunk_plan::plan_chunks(&files, chunk_plan::TARGET_BYTES, chunk_plan::MAX_BYTES);
    // Refuse to run rather than silently truncate. The chunk queue is a
    // fixed-size shared-memory array, so a plan with more chunks than the
    // queue holds used to be cut down with only a stderr warning — the
    // trailing files were never indexed while the result still reported
    // ok=true, i.e. a silently incomplete index. Failing here makes the
    // gap explicit and leaves the database untouched.
    if chunks.len() > chunk_queue::MAX_CHUNKS {
        return error_json(
            &format!(
                "plan_chunks produced {} chunks but the shared-memory queue holds at most {}; \
                 refusing to index a partial project [module=scheduler, method=index_parallel_chunked]",
                chunks.len(),
                chunk_queue::MAX_CHUNKS
            ),
            "scheduler",
            "index_parallel_chunked",
        );
    }
    let chunk_count = chunks.len() as u32;
    if chunk_count == 0 {
        return error_json(
            "plan_chunks returned zero chunks [module=scheduler, method=index_parallel_chunked]",
            "scheduler",
            "index_parallel_chunked",
        );
    }

    // Share the GLOBAL file list with workers via a temp JSON file.
    // Workers slice it by each chunk's (file_start, file_count).
    let files_json_path = format!("{}_chunk_files.json", db_prefix);
    let files_json = serde_json::to_string(&all_paths).unwrap_or_default();
    if let Err(e) = std::fs::write(&files_json_path, &files_json) {
        return error_json(
            &format!(
                "write chunk file list failed: {} [module=scheduler, method=index_parallel_chunked]",
                e
            ),
            "scheduler",
            "index_parallel_chunked",
        );
    }

    // ── Phase 3: create + fill the chunk queue ──────────────
    let shm_path = format!("/tmp/codescope_chunked_sched_{}.shm", std::process::id());
    let _ = std::fs::remove_file(&shm_path);
    let queue = match chunk_queue::ChunkQueue::create(&shm_path, chunk_count) {
        Ok(q) => q,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_chunked"),
    };
    for (idx, ch) in chunks.iter().take(chunk_count as usize).enumerate() {
        if let Err(e) = queue.write_chunk(
            idx as u32,
            ch.module_id,
            ch.file_start as u32,
            ch.file_count as u32,
            ch.total_bytes,
        ) {
            return error_json(
                &format!(
                    "write_chunk {} failed: {} [module=scheduler, method=index_parallel_chunked]",
                    idx, e
                ),
                "scheduler",
                "index_parallel_chunked",
            );
        }
    }

    eprintln!(
        "scheduler: [chunked] project={} files={} chunks={} workers={} parallel={}",
        project_path,
        all_paths.len(),
        chunk_count,
        total_workers,
        parallel
    );

    // ── Phase 3: spawn workers, each with its OWN DB ──────────
    // Every chunk-worker writes to a unique per-worker DB
    // (`{db_prefix}_chunk_{worker_id}.db`) with a unique project_id
    // (`worker_id + 1`), so there are NO concurrent WAL writers on a
    // shared DB — the original shared_db design corrupted the index
    // under concurrency. After all workers exit, Phase 4 merges the
    // per-worker DBs into the unified main DB via the SAME
    // merge_module_dbs machinery as the static path.
    let mut handles = Vec::new();
    let mut results: Vec<ModuleResult> = Vec::new();
    // Worker ids whose process failed, kept so their chunks can be released
    // and re-attempted below (Phase 3b).
    let mut failed_worker_ids: Vec<u32> = Vec::new();
    let active = Arc::new(AtomicU32::new(0));
    // Carries the worker id alongside the result so a failed worker's chunks
    // can be released and re-attempted (Phase 3b).
    let (tx, rx) = mpsc::channel::<(u32, ModuleResult)>();

    for worker_id in 0..total_workers {
        // Wait for a free slot if at concurrency cap.
        while active.load(Ordering::SeqCst) >= parallel {
            std::thread::sleep(POLL_INTERVAL);
        }
        active.fetch_add(1, Ordering::SeqCst);

        // CPU set for this worker (static binding, Linux only; empty
        // elsewhere so run_chunk_worker skips taskset wrapping).
        #[cfg(target_os = "linux")]
        let cpu_set = {
            let host_cores = std::thread::available_parallelism()
                .map(|n| n.get() as u32)
                .unwrap_or(1)
                .max(1);
            let cores_per_worker = (host_cores / total_workers.max(1)).max(1);
            let cpu_idx = (worker_id * cores_per_worker) % host_cores;
            format!("{}", cpu_idx)
        };
        #[cfg(not(target_os = "linux"))]
        let cpu_set: String = String::new();

        // Per-worker DB + unique project_id. Mirrors the static path's
        // (idx + 1) project_id scheme so merge's id-remap and project
        // disambiguation behave identically.
        let worker_db = format!("{}_chunk_{}.db", db_prefix, worker_id);
        let project_id: u64 = (worker_id as u64) + 1;

        let active_clone = Arc::clone(&active);
        let tx = tx.clone();
        let exe_str = exe_str.clone();
        let grammars_dir = grammars_dir.clone();
        let shm_path = shm_path.clone();
        let worker_db = worker_db.clone();
        let files_json_path = files_json_path.clone();

        let handle = std::thread::spawn(move || {
            let result = worker::run_chunk_worker(
                &exe_str,
                &shm_path,
                worker_id,
                &cpu_set,
                &worker_db,
                &files_json_path,
                project_id,
                &grammars_dir,
            );
            let _ = tx.send((worker_id, result));
            active_clone.fetch_sub(1, Ordering::SeqCst);
        });
        handles.push(handle);
    }

    drop(tx);

    // Collect results, remembering which worker each came from: the retry
    // round below releases exactly the chunks the failed workers own.
    while let Ok((worker_id, r)) = rx.recv() {
        if r.exit_code != 0 {
            failed_worker_ids.push(worker_id);
        }
        results.push(r);
    }

    // Wait for all threads to finish.
    for h in handles {
        let _ = h.join();
    }

    // ── Phase 3b: recover the chunks of workers that died ─────────
    // A chunk worker owns ONE DB covering every chunk it processed (they steal
    // work), and the merge below drops the DB of a failed worker — so a crash
    // or timeout discarded the chunks that worker had already marked DONE, and
    // their files were missing from the index with nothing but `complete:
    // false` to show for it (docs/CODE_REVIEW_2026-09-18.md #15, option C).
    //
    // The queue still records which chunks belong to which worker (`mark_done`
    // leaves `claimer_id` set), so they are released back to PENDING and ONE
    // replacement worker re-indexes them. One round only: a deterministic
    // crasher — a file that kills every worker that reads it — would kill the
    // replacement the same way, and looping on it would turn a crash into a
    // hang. Whatever the round does not recover is reported as before (`ok` and
    // `complete` stay false, `fail` counts the workers).
    let mut recovered_chunks = 0u32;
    let mut retry_worker_failed = false;
    let mut retry_worker_attempted = false;
    if !failed_worker_ids.is_empty() {
        for id in &failed_worker_ids {
            recovered_chunks += queue.release_worker_chunks(*id);
        }
        // The release above only finds chunks the dead worker had CLAIMED. A
        // worker can also die before claiming anything, which leaves chunks
        // that are still PENDING and now have nobody to pick them up — so the
        // queue, not the release count, decides whether work is missing.
        if !queue.is_complete() {
            retry_worker_attempted = true;
            let retry_id = total_workers;
            let retry_db = format!("{}_chunk_retry.db", db_prefix);
            // Project ids are 1..=total_workers for the first round; the
            // replacement takes the next one so the merge's id remap sees it as
            // its own worker, exactly like the others.
            let retry_project_id = (total_workers as u64) + 1;
            eprintln!(
                "scheduler: [chunked] {} chunk(s) released from {} failed worker(s); re-indexing as worker {}",
                recovered_chunks,
                failed_worker_ids.len(),
                retry_id
            );
            let retry_result = worker::run_chunk_worker(
                &exe_str,
                &shm_path,
                retry_id,
                "", // no CPU binding for the single recovery worker
                &retry_db,
                &files_json_path,
                retry_project_id,
                &grammars_dir,
            );
            retry_worker_failed = retry_result.exit_code != 0;
            eprintln!(
                "scheduler: [chunked] recovery worker {}: exit={} files={} nodes={}",
                retry_id,
                retry_result.exit_code,
                retry_result.files_indexed,
                retry_result.total_nodes
            );
            // Counted like any other worker: `success`/`fail` are derived from
            // `results`, so a replacement that also dies makes the run
            // incomplete rather than silently recovering on paper.
            results.push(retry_result);
        }
    }

    // Both per-run temporaries are removed here, after every worker has exited
    // and before the merge. Neither used to be removed at all, so a successful
    // run leaked them too: the queue path embeds the PID
    // (`/tmp/codescope_chunked_sched_<pid>.shm`), so the next run's
    // remove-before-create can never clean up an earlier process's file and
    // the chunk-file list accumulated in /tmp one JSON per run. Unlinking the
    // shm while the mapping is alive is fine on POSIX (the name is what goes
    // away, not the pages), and no code path reads either name after this
    // point — the response reports `db_prefix`/`main_db`, not these.
    let _ = std::fs::remove_file(&shm_path);
    let _ = std::fs::remove_file(&files_json_path);

    // ── Phase 4: merge per-worker DBs into the unified main DB ──
    // Mirror index_parallel Phase 6: ATTACH each worker DB to a fresh
    // main DB and INSERT OR IGNORE. Per-worker project_ids are unique
    // (worker_id + 1) and merge_module_dbs remaps ids to avoid
    // cross-worker collisions (same as the static path).
    let main_db = format!("{}_main.db", db_prefix);
    // main.db is ALWAYS rebuilt from the module DBs below — keep_db only
    // preserves the per-module DBs so workers can skip unchanged files.
    // Keeping main.db too would double-count rows on INSERT OR IGNORE.
    let _ = std::fs::remove_file(&main_db);
    let _ = std::fs::remove_file(format!("{}-wal", main_db));
    let _ = std::fs::remove_file(format!("{}-shm", main_db));

    let worker_db_paths: Vec<String> = results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .map(|r| r.db_path.clone())
        .collect();

    let merge_result = if worker_db_paths.is_empty() {
        MergeResult {
            merged: false,
            main_db_path: main_db.clone(),
            tables_merged: 0,
            rows_merged: 0,
            duration_ms: 0,
            error: Some(
                "no successful chunk-worker DBs to merge [module=scheduler, method=index_parallel_chunked]"
                    .to_string(),
            ),
        }
    } else {
        merge::merge_module_dbs(&main_db, &worker_db_paths)
    };

    // v0.2.5 (C2 fix): parallel chunk workers deferred CSR construction;
    // rebuild each project's CSR from the globally-remapped relation table.
    if merge_result.merged {
        rebuild_csr_all_projects(&main_db, "index_parallel_chunked");
    }

    // ── Phase 5: aggregate summary ────────────────────────────
    let success = results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .count();
    let fail = results.len() - success;
    let total_nodes: u64 = results.iter().map(|r| r.total_nodes).sum();
    let total_edges: u64 = results.iter().map(|r| r.total_edges).sum();
    let total_files_indexed: u64 = results.iter().map(|r| r.files_indexed).sum();

    let modules_json: Vec<Value> = results
        .iter()
        .map(|r| {
            json!({
                "name": r.name,
                "exit_code": r.exit_code,
                "files_indexed": r.files_indexed,
                "candidate_files": r.candidate_files,
                "total_nodes": r.total_nodes,
                "total_edges": r.total_edges,
                "time_parse_ms": r.time_parse_ms,
                "duration_secs": r.duration_secs,
                "workers": r.workers,
                "db_path": r.db_path,
                "error": r.error,
            })
        })
        .collect();

    // "ok" means the run completed AND produced a consistent index — not just
    // "at least one worker finished". See run_complete().
    let complete = chunked_run_complete(
        merge_result.merged,
        retry_worker_attempted,
        retry_worker_failed,
        queue.is_complete(),
    );
    json!({
        "ok": complete,
        "complete": complete,
        "project_path": project_path,
        "db_prefix": db_prefix,
        "main_db": merge_result.main_db_path,
        "merge": {
            "merged": merge_result.merged,
            "tables_merged": merge_result.tables_merged,
            "rows_merged": merge_result.rows_merged,
            "duration_ms": merge_result.duration_ms,
            "error": merge_result.error,
        },
        "total_workers": total_workers,
        "parallel": parallel,
        "sched_mode": "chunked",
        "duration_ms": start.elapsed().as_millis() as u64,
        "success": success,
        "fail": fail,
        // Chunks whose worker died and that a replacement re-indexed; a
        // non-zero count with a failed retry_worker means files are still
        // missing (the run says complete: false either way).
        "recovered_chunks": recovered_chunks,
        "retry_worker_failed": retry_worker_failed,
        "total_nodes": total_nodes,
        "total_edges": total_edges,
        "total_files_indexed": total_files_indexed,
        "modules": modules_json
    })
    .to_string()
}

#[cfg(test)]
mod tests {
    use super::chunked_run_complete;

    #[test]
    fn test_chunked_run_complete_accounts_for_recovery() {
        // A healthy run: merge ok, every chunk terminal, no recovery needed.
        assert!(chunked_run_complete(true, false, false, true));
        // A failed merge is never complete, whatever the queue says.
        assert!(!chunked_run_complete(false, false, false, true));

        // Failures whose chunks a replacement re-indexed: complete, with the
        // deaths still visible in `fail` / `recovered_chunks`.
        assert!(chunked_run_complete(true, true, false, true));

        // Every way the recovery can fall short stays incomplete:
        assert!(
            !chunked_run_complete(true, true, true, true),
            "the replacement died too"
        );
        assert!(
            !chunked_run_complete(true, true, false, false),
            "the queue still holds chunks nobody finished"
        );
        assert!(
            !chunked_run_complete(true, false, false, false),
            "a worker died before claiming anything and nothing recovered the work"
        );
    }
}
