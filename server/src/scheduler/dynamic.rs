// dynamic.rs — the retired shm-core-pool dynamic scheduler.
//
// Split out of scheduler/mod.rs (see plan/rules/code_rules.md 1000-line
// rule). Kept for reference only: the opt-in dynamic path now lives in
// chunked.rs, which replaced it. This file compiles and is covered by the
// unit tests but is never dispatched, so it can be deleted once the
// migration to the chunk-level scheduler is complete.
//
// `use super::*` inherits the parent module's private helpers and shared
// constants without widening their visibility crate-wide.

use super::*;

/// Dynamic-mode parallel indexer with worker queue + core reclaim.
///
/// Phases:
/// 1. `discover_modules` — same as static (file walk, no parsing).
/// 2. Create `SchedShm` — shared memory for cross-process core
///    accounting. Falls back to static mode on creation failure.
/// 3. Sort modules by file count desc — largest first so big modules
///    start early and benefit from cores freed by small modules.
/// 4. Dispatch workers from a queue, claiming cores from the shm pool
///    before each spawn and releasing them on completion. This is the
///    core reclaim: small modules finish and free their cores for the
///    next pending module while large modules retain their proportional
///    allocation until done.
/// 5. Aggregate summary (same as static).
/// 6. Merge per-module DBs (same as static).
///
/// Memory monitoring: child PIDs are sampled each poll tick via
/// `pgrep -P $$` + `ps -o rss=`; the total RSS is pushed to
/// `SchedShm::update_mem_usage`. If RSS exceeds `mem_limit_mb`, new
/// worker spawns are paused (existing workers keep running — we don't
/// kill them).
// Kept for reference; the chunk-level scheduler (index_parallel_chunked)
// replaces this path. Remove after migration is complete.
#[allow(dead_code)]
fn index_parallel_dynamic(project_dir: &str, total_workers: u32, parallel: u32) -> String {
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
    // Cap parallel at total_workers to avoid spawning more OS threads
    // than cores available. The shm pool caps cores, not thread count,
    // so without this cap a user could pass --parallel 100 and spawn
    // 100 idle threads all waiting for 0-core claims.
    let parallel = parallel.min(total_workers).max(1);

    let project_path_buf = match canonicalize_project_dir(project_dir) {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_dynamic"),
    };
    let project_path = project_path_buf.to_string_lossy().to_string();

    let exe_path = match resolve_self_exe() {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_dynamic"),
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
    let db_prefix = prefix_env.unwrap_or_else(|_| format!("/tmp/codescope_parallel_{}", run_id));
    let _ = keep_db; // keep_db consumed in run_module_worker calls below

    eprintln!(
        "scheduler: [dynamic] project={} workers={} parallel={} db_prefix={}",
        project_path, total_workers, parallel, db_prefix
    );

    // ── Phase 1: discover modules ──────────────────────────────
    let discover_json = discover::discover_modules(&project_path);
    let discover_val: Value = match serde_json::from_str(&discover_json) {
        Ok(v) => v,
        Err(e) => {
            return error_json(
                &format!(
                    "discover parse failed: {} [module=scheduler, method=index_parallel_dynamic]",
                    e
                ),
                "scheduler",
                "index_parallel_dynamic",
            );
        }
    };
    if discover_val["ok"] != true {
        return discover_json;
    }

    let modules = match discover_val["modules"].as_array() {
        Some(a) if !a.is_empty() => a,
        _ => {
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
                "note": "no source modules found"
            })
            .to_string();
        }
    };

    let total_files_sum: u64 = modules
        .iter()
        .filter_map(|m| m["files"].as_u64())
        .sum::<u64>()
        .max(1);

    // ── Phase 2: create SchedShm ───────────────────────────────
    // total_cores caps at total_workers so the dynamic scheduler
    // never claims more than the user asked for. available_parallelism
    // gives us the host CPU count, which we then min with total_workers.
    let dyn_config = DynSchedConfig::from_env();
    let host_cores: u32 = std::thread::available_parallelism()
        .map(|n| n.get() as u32)
        .unwrap_or(total_workers.max(1));
    let total_cores = host_cores.min(total_workers.max(1));
    let mem_limit = dyn_config.mem_limit_mb;
    let shm_path = dyn_config
        .shm_path
        .clone()
        .unwrap_or_else(DynSchedConfig::default_shm_path);

    eprintln!(
        "scheduler: [dynamic] shm_path={} total_cores={} mem_limit_mb={}",
        shm_path, total_cores, mem_limit
    );

    let shm = match SchedShm::create(&shm_path, total_cores, mem_limit) {
        Ok(s) => {
            // Set the aggressive poll flag from config (1 = 50ms, 0 = 100ms).
            s.set_aggressive(dyn_config.aggressive);
            s
        }
        Err(e) => {
            // Graceful fallback: disable dynamic sched and recurse into
            // the static path. The env override is process-local and
            // only affects this scheduler run.
            eprintln!(
                "scheduler: [dynamic] SchedShm create failed ({}), falling back to static mode [module=scheduler, method=index_parallel_dynamic]",
                e
            );
            // SAFETY: Rust 2024 marks set_var unsafe because it can race
            // with concurrent getenv readers. This code path runs in the
            // single-threaded scheduler init before any worker threads
            // spawn, so there are no concurrent readers.
            unsafe {
                std::env::set_var("CODESCOPE_DYNAMIC_SCHED", "0");
            }
            return index_parallel(project_dir, total_workers, parallel);
        }
    };

    // RAII guard: remove the shm filesystem path when the scheduler
    // exits, even on panic. Constructed IMMEDIATELY after create so
    // nothing between create and guard can leak the shm file.
    // SchedShm::drop also unlinks (via shm_unlink), so the second
    // unlink is a no-op (ENOENT) — harmless redundancy.
    struct ShmGuard {
        path: String,
    }
    impl Drop for ShmGuard {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.path);
        }
    }
    let _shm_guard = ShmGuard {
        path: shm_path.clone(),
    };

    // Propagate shm path via env so worker subprocesses can attach.
    // Command inherits env vars by default, so setting this once on
    // the scheduler process is enough.
    // SAFETY: Rust 2024 marks set_var unsafe because it can race with
    // concurrent getenv readers. This runs in the single-threaded
    // scheduler init before any worker threads spawn, so no races.
    unsafe {
        std::env::set_var("CODESCOPE_SCHED_SHM", &shm_path);
    }

    // ── Phase 3: sort modules by file count desc ─────────────
    // Largest first so big modules start early — they benefit most
    // from core reclaim since small modules finish and free cores
    // while the big module is still running.
    let mut queue: VecDeque<(String, u64, u64)> = modules
        .iter()
        .enumerate()
        .filter_map(|(i, m)| {
            let name = m["name"].as_str()?.to_string();
            let files = m["files"].as_u64().unwrap_or(0);
            // project_id is 1-indexed (see index_parallel for rationale).
            Some((name, files, (i as u64) + 1))
        })
        .collect();
    // VecDeque::from(Vec) preserves order; then sort in place.
    // We convert to Vec, sort, then convert back since VecDeque
    // has no built-in sort_by_key.
    {
        let mut v: Vec<_> = queue.drain(..).collect();
        v.sort_by_key(|b| std::cmp::Reverse(b.1));
        queue.extend(v);
    }

    // ── Phase 4: dynamic worker dispatch with core reclaim ────
    let (tx, rx) = mpsc::channel::<ModuleResult>();
    let active = Arc::new(AtomicU32::new(0));
    let shm = Arc::new(shm);
    let mut handles = Vec::new();
    let mut results: Vec<ModuleResult> = Vec::new();

    loop {
        // Drain completed workers (non-blocking). The worker thread
        // already released its claimed cores via shm.release_cores
        // before sending the result, so we just collect results here.
        while let Ok(r) = rx.try_recv() {
            results.push(r);
        }

        // Sample total RSS of child processes. Pauses new spawns if
        // over mem_limit (existing workers keep running — we don't
        // kill them, just don't start new ones).
        let child_pids = get_child_pids();
        let total_rss_mb = sample_total_rss_mb(&child_pids);
        shm.update_mem_usage(total_rss_mb);
        let mem_ok = total_rss_mb <= mem_limit;
        if !mem_ok {
            eprintln!(
                "scheduler: [dynamic] memory limit exceeded ({} > {}), pausing new spawns [module=scheduler, method=index_parallel_dynamic]",
                total_rss_mb, mem_limit
            );
        }

        // Try to start new workers while we have capacity, pending
        // tasks, available cores, and memory headroom.
        while active.load(Ordering::SeqCst) < parallel && !queue.is_empty() && mem_ok {
            if !shm.has_available() {
                // No cores to claim right now — wait for an active
                // worker to release some. Don't pop the task yet.
                break;
            }

            let (name, files, project_id) =
                queue.pop_front().expect("queue non-empty in dispatch loop");

            // Desired core count: proportional to file count, capped
            // at total_workers. Matches the static allocation formula.
            let want = if files == 0 {
                1
            } else {
                let raw = (files as u128 * total_workers as u128).div_ceil(total_files_sum as u128);
                let a = u32::try_from(raw as u64).unwrap_or(total_workers);
                std::cmp::max(1, std::cmp::min(a, total_workers))
            };

            // Claim cores atomically. If 0 returned, another worker
            // beat us to it — put the task back at the front and wait.
            let claimed = shm.claim_cores(want);
            if claimed == 0 {
                queue.push_front((name, files, project_id));
                break;
            }

            active.fetch_add(1, Ordering::SeqCst);

            let active_clone = Arc::clone(&active);
            let tx = tx.clone();
            let exe_str = exe_str.clone();
            let project_path = project_path.clone();
            let grammars_dir = grammars_dir.clone();
            let db_prefix = db_prefix.clone();
            let shm_clone = Arc::clone(&shm);

            let handle = std::thread::spawn(move || {
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    run_module_worker(
                        &exe_str,
                        &project_path,
                        &name,
                        files,
                        claimed,
                        &grammars_dir,
                        &db_prefix,
                        project_id,
                        None,
                        keep_db,
                    )
                }));
                // Release the cores we claimed so the next pending
                // worker can grab them. This is the core reclaim: a
                // small module finishing frees its cores for the next
                // big module still in the queue.
                // catch_unwind guarantees this runs even if the worker
                // panicked — without it, core leak + active underflow
                // would cause a permanent hang (H-C).
                shm_clone.release_cores(claimed);
                let result = match result {
                    Ok(r) => r,
                    Err(_) => ModuleResult {
                        name: name.clone(),
                        exit_code: -5,
                        total_nodes: 0,
                        total_edges: 0,
                        files_indexed: 0,
                        candidate_files: files,
                        time_parse_ms: 0,
                        duration_secs: 0,
                        workers: claimed,
                        db_path: String::new(),
                        project_id,
                        error: Some(
                            "worker panicked [module=scheduler, method=index_parallel_dynamic]"
                                .to_string(),
                        ),
                    },
                };
                let _ = tx.send(result);
                active_clone.fetch_sub(1, Ordering::SeqCst);
            });
            handles.push(handle);
        }

        // Exit when queue is drained AND no active workers remain.
        if active.load(Ordering::SeqCst) == 0 && queue.is_empty() {
            break;
        }

        // Sleep longer when memory-paused: existing workers need time to
        // finish and release RSS. 50ms polling burns CPU on pgrep+ps for
        // no benefit — 1s is a reasonable backoff.
        if !mem_ok {
            std::thread::sleep(Duration::from_secs(1));
        } else {
            std::thread::sleep(POLL_INTERVAL);
        }
    }

    drop(tx);

    // Drain any results that arrived between the last try_recv and
    // the channel closure.
    while let Ok(r) = rx.try_recv() {
        results.push(r);
    }

    for h in handles {
        let _ = h.join();
    }

    // ── Phase 4b: quarantine failed modules ───────────────────
    // Retry failed modules with crashing files excluded. Tries to claim
    // cores from the shm pool so the retry benefits from available cores
    // (e.g., after small modules finished and freed their cores).
    // Falls back to 1 worker if the pool is empty or shm is unavailable.
    let mut final_results: Vec<ModuleResult> = Vec::new();
    for r in results {
        if r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0) {
            final_results.push(r);
            continue;
        }
        eprintln!(
            "scheduler: [dynamic] module {} failed (exit={}, nodes={}) — starting quarantine",
            r.name, r.exit_code, r.total_nodes
        );
        let quarantined = quarantine::quarantine_module(
            &exe_str,
            &project_path,
            &r.name,
            &grammars_dir,
            &db_prefix,
        );
        if quarantined.is_empty() {
            final_results.push(r);
            continue;
        }
        let excluded_env = quarantined.join(",");
        // Try to claim up to 4 cores, or fall back to 1.
        let retry_workers = shm.claim_cores(4);
        let retry_workers = if retry_workers == 0 { 1 } else { retry_workers };
        let retry = run_module_worker(
            &exe_str,
            &project_path,
            &r.name,
            r.files_indexed,
            retry_workers,
            &grammars_dir,
            &db_prefix,
            r.project_id,
            Some(&excluded_env),
            keep_db,
        );
        // Release the claimed cores back to the pool.
        shm.release_cores(retry_workers);
        final_results.push(retry);
    }

    // ── Phase 5: aggregate summary ────────────────────────────
    let success = final_results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .count();
    let fail = final_results.len() - success;
    let total_nodes: u64 = final_results.iter().map(|r| r.total_nodes).sum();
    let total_edges: u64 = final_results.iter().map(|r| r.total_edges).sum();
    let total_files_indexed: u64 = final_results.iter().map(|r| r.files_indexed).sum();

    // ── Phase 6: merge per-module DBs into unified main DB ────
    let main_db = format!("{}_main.db", db_prefix);
    // main.db is ALWAYS rebuilt from the module DBs below — keep_db only
    // preserves the per-module DBs so workers can skip unchanged files.
    // Keeping main.db too would double-count rows on INSERT OR IGNORE.
    let _ = std::fs::remove_file(&main_db);
    let _ = std::fs::remove_file(format!("{}-wal", main_db));
    let _ = std::fs::remove_file(format!("{}-shm", main_db));

    let module_db_paths: Vec<String> = final_results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .map(|r| r.db_path.clone())
        .collect();

    let merge_result = if module_db_paths.is_empty() {
        MergeResult {
            merged: false,
            main_db_path: main_db.clone(),
            tables_merged: 0,
            rows_merged: 0,
            duration_ms: 0,
            error: Some("no successful module DBs to merge".to_string()),
        }
    } else {
        merge::merge_module_dbs(&main_db, &module_db_paths)
    };

    // v0.2.5 (C2 fix): parallel workers deferred CSR construction because
    // their packed BLOBs hold local entity ids that merge cannot remap.
    // Rebuild each project's CSR from the globally-remapped relation table.
    if merge_result.merged {
        rebuild_csr_all_projects(&main_db, "index_parallel");
    }

    let modules_json: Vec<Value> = final_results
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

    json!({
        "ok": success > 0,
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
        "sched_mode": "dynamic",
        "duration_ms": start.elapsed().as_millis() as u64,
        "success": success,
        "fail": fail,
        "total_nodes": total_nodes,
        "total_edges": total_edges,
        "total_files_indexed": total_files_indexed,
        "modules": modules_json
    })
    .to_string()
}

/// Get child PIDs of the current process via `pgrep -P $$`.
/// Returns an empty vec if pgrep is unavailable (e.g., unsupported
/// platform) — memory monitoring just degrades to no-op in that case.
/// Kept for reference by the old index_parallel_dynamic path.
#[allow(dead_code)]
fn get_child_pids() -> Vec<u32> {
    let parent_pid = std::process::id();
    let output = std::process::Command::new("pgrep")
        .args(["-P", &parent_pid.to_string()])
        .stderr(std::process::Stdio::null())
        .output();
    match output {
        Ok(out) if out.status.success() => String::from_utf8_lossy(&out.stdout)
            .lines()
            .filter_map(|l| l.trim().parse::<u32>().ok())
            .collect(),
        _ => Vec::new(),
    }
}
