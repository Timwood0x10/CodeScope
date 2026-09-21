// chunk_queue_ops.rs — worker-facing queue operations.
//
// Split out of chunk_queue.rs (see plan/rules/code_rules.md 1000-line
// rule). A second `impl ChunkQueue` block: claiming, marking done/failed,
// stale-chunk recovery and the completion/progress counters all read the
// atomically-shared header, so they live together and stay apart from the
// mmap setup/teardown in the parent module. `use super::*` reaches the
// private struct fields and `now_ms` helper without widening their
// visibility crate-wide.

use super::*;

impl ChunkQueue {
    /// Worker atomically claims the next PENDING chunk via CAS.
    ///
    /// Linear-scans `chunks[0..chunk_count]` for the first `PENDING`
    /// slot and CAS-es it to `CLAIMED` with `claimer_id = worker_id`.
    /// Returns the claimed slot index, or `None` if no chunk is
    /// pending. Multiple workers calling concurrently are serialised
    /// by the CAS — losers simply retry the scan (§6.1).
    ///
    /// Records `started_at_ms` so [`ChunkQueue::reset_stale`] can
    /// detect crashed workers.
    pub fn claim_next(&self, worker_id: u32) -> Option<u32> {
        // SAFETY: self.ptr is valid for the lifetime of self; reads via
        // shared reference are safe because all mutable fields are atomic.
        let state = unsafe { &*self.ptr };
        let count = state.header.chunk_count;
        for i in 0..count {
            let slot = &state.chunks[i as usize];
            // Acquire on success pairs with the Release store in
            // mark_done/mark_failed/reset_stale so a claimer observes
            // the full prior state of the slot (weak memory: Apple
            // Silicon reorders). Relaxed on failure — a lost CAS just
            // retries the scan and carries no cross-thread dependency.
            match slot.status.compare_exchange(
                STATUS_PENDING,
                STATUS_CLAIMED,
                Ordering::Acquire,
                Ordering::Relaxed,
            ) {
                Ok(_) => {
                    slot.claimer_id.store(worker_id, Ordering::Relaxed);
                    slot.started_at_ms.store(now_ms(), Ordering::Relaxed);
                    return Some(i);
                }
                Err(_) => continue,
            }
        }
        None
    }

    /// Mark chunk `idx` as DONE. Records `finished_at_ms`. No-op if
    /// the slot is out of range (caller bug — we don't panic).
    pub fn mark_done(&self, idx: u32) {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        if idx >= state.header.chunk_count {
            return;
        }
        let slot = &state.chunks[idx as usize];
        // Write finished_at BEFORE the status store so a Release/Acquire
        // pair makes it (and the worker's parse side-effects) visible to
        // any observer that reads status == DONE via an Acquire load.
        slot.finished_at_ms.store(now_ms(), Ordering::Relaxed);
        slot.status.store(STATUS_DONE, Ordering::Release);
    }

    /// Mark chunk `idx` as FAILED. Records `finished_at_ms`. No-op if
    /// the slot is out of range.
    pub fn mark_failed(&self, idx: u32) {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        if idx >= state.header.chunk_count {
            return;
        }
        let slot = &state.chunks[idx as usize];
        // Same Release discipline as mark_done: publish finished_at (and
        // the worker's parse side-effects) before the status store so an
        // Acquire reader that observes FAILED also sees the prior writes.
        slot.finished_at_ms.store(now_ms(), Ordering::Relaxed);
        slot.status.store(STATUS_FAILED, Ordering::Release);
    }

    /// Increment the failed-file counter on chunk `idx`.
    /// Called by the parse loop when a single file fails (§7.3).
    /// No-op if the slot is out of range.
    pub fn inc_failed_files(&self, idx: u32) {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        if idx >= state.header.chunk_count {
            return;
        }
        let slot = &state.chunks[idx as usize];
        slot.failed_files.fetch_add(1, Ordering::Release);
    }

    /// Reset a CLAIMED chunk back to PENDING if its worker has timed
    /// out (crash recovery, §8). Returns true if the reset happened.
    ///
    /// `timeout_ms` is the maximum allowed gap between `started_at_ms`
    /// and now. The CAS only succeeds if the slot is still CLAIMED —
    /// if the worker raced and finished just before us, we leave its
    /// DONE/FAILED state alone.
    pub fn reset_stale(&self, idx: u32, timeout_ms: u64) -> bool {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        if idx >= state.header.chunk_count {
            return false;
        }
        let slot = &state.chunks[idx as usize];
        let started = slot.started_at_ms.load(Ordering::Acquire);
        if started == 0 {
            // A PENDING slot carrying 0 has never been claimed and is not
            // stale — nobody owns it.
            //
            // A CLAIMED slot carrying 0 is a different thing: claim_next()
            // publishes CLAIMED with its CAS and stores the timestamp after,
            // and status/started_at_ms are separate atomics, so an observer
            // can legitimately see CLAIMED with the previous 0 (a worker that
            // crashed in that window leaves it that way for good). Returning
            // false here — as this used to — made such a chunk permanently
            // unrecoverable: the watchdog skipped it as "never claimed" and
            // its files were never indexed. Treat it as stale instead: another
            // worker re-claims it and re-indexes its files, which the design
            // tolerates (each worker writes its own DB and the merge is
            // INSERT OR IGNORE), so the worst case is one chunk of redundant
            // work instead of a permanently missing slice of the index.
            if slot.status.load(Ordering::Acquire) != STATUS_CLAIMED {
                return false;
            }
        }
        let elapsed = if started == 0 {
            u64::MAX // claimed but never stamped: as stale as it gets
        } else {
            now_ms().saturating_sub(started)
        };
        if elapsed < timeout_ms {
            return false;
        }
        // CAS CLAIMED → PENDING so we don't clobber a worker that just
        // finished (race between watchdog and worker completion).
        // Release on success publishes the recycle so the next claimer's
        // Acquire CAS in claim_next observes a clean slot; Relaxed on
        // failure (a lost race carries no cross-thread dependency).
        match slot.status.compare_exchange(
            STATUS_CLAIMED,
            STATUS_PENDING,
            Ordering::Release,
            Ordering::Relaxed,
        ) {
            Ok(_) => {
                slot.claimer_id.store(u32::MAX, Ordering::Relaxed);
                slot.started_at_ms.store(0, Ordering::Relaxed);
                true
            }
            Err(_) => false,
        }
    }

    /// Return every chunk claimed by `worker_id` to PENDING so a replacement
    /// worker can re-index them; returns how many were released.
    ///
    /// Used when a chunk worker dies or times out (see chunked.rs). Such a
    /// worker owns ONE per-worker DB covering every chunk it processed — they
    /// steal work — so dropping that DB (what the merge does for a failed
    /// worker) throws away chunks it had already marked DONE. Nobody would
    /// redo them and their files would be missing from the index with only
    /// `complete: false` to show for it.
    ///
    /// `mark_done`/`mark_failed` leave `claimer_id` set, which is what makes
    /// the queue a record of "which chunks did worker N finish" — the
    /// information this release needs. Every status is released, not only
    /// CLAIMED: a DONE chunk's rows live in the vanished DB, and a FAILED one
    /// was judged by a worker that did not survive to report it.
    ///
    /// Called after every worker thread has been joined, so the stores below
    /// have no concurrent reader.
    pub fn release_worker_chunks(&self, worker_id: u32) -> u32 {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        let mut released = 0u32;
        for i in 0..state.header.chunk_count {
            let slot = &state.chunks[i as usize];
            // init() sets claimer_id = u32::MAX for a chunk that has never
            // been claimed, so a real worker id can only match a real claim.
            if slot.claimer_id.load(Ordering::Acquire) != worker_id {
                continue;
            }
            slot.claimer_id.store(u32::MAX, Ordering::Relaxed);
            slot.started_at_ms.store(0, Ordering::Relaxed);
            slot.finished_at_ms.store(0, Ordering::Relaxed);
            // Release publishes the cleared fields to whoever claims next.
            slot.status.store(STATUS_PENDING, Ordering::Release);
            released += 1;
        }
        released
    }

    /// Returns true if every chunk is in DONE or FAILED state.
    /// Used by the scheduler's main loop to detect completion.
    /// Scan every chunk and reclaim any `CLAIMED` chunk whose worker has
    /// been silent longer than `timeout_ms` (orphaned by a crashed
    /// worker). Returns the number of chunks reset to `PENDING`.
    ///
    /// Called by idle workers (when `claim_next` finds no `PENDING` chunk)
    /// so a crash mid-chunk cannot permanently strand files: another
    /// worker re-claims the orphaned chunk and re-indexes its files
    /// (idempotent — the worker writes to its OWN per-worker DB, so no
    /// duplicate rows appear in the final merge). See DYNAMIC_SCHED_REDESIGN.md §8.
    pub fn reset_all_stale(&self, timeout_ms: u64) -> u32 {
        let count = self.chunk_count();
        let mut reclaimed = 0u32;
        for i in 0..count {
            if self.reset_stale(i, timeout_ms) {
                reclaimed += 1;
            }
        }
        reclaimed
    }

    pub fn is_complete(&self) -> bool {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        let count = state.header.chunk_count;
        for i in 0..count {
            // Acquire pairs with the Release store in mark_done/mark_failed:
            // once the scheduler observes every chunk DONE/FAILED it gates
            // the resolve phase, so it must see all of each worker's prior
            // writes (weak memory ordering on Apple Silicon).
            let s = state.chunks[i as usize].status.load(Ordering::Acquire);
            if s != STATUS_DONE && s != STATUS_FAILED {
                return false;
            }
        }
        true
    }

    /// Number of chunks currently in PENDING state.
    pub fn pending_count(&self) -> u32 {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        let count = state.header.chunk_count;
        let mut n = 0u32;
        for i in 0..count {
            if state.chunks[i as usize].status.load(Ordering::Acquire) == STATUS_PENDING {
                n += 1;
            }
        }
        n
    }

    /// Number of chunks in DONE state.
    pub fn done_count(&self) -> u32 {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        let count = state.header.chunk_count;
        let mut n = 0u32;
        for i in 0..count {
            if state.chunks[i as usize].status.load(Ordering::Acquire) == STATUS_DONE {
                n += 1;
            }
        }
        n
    }

    /// Number of chunks in FAILED state.
    pub fn failed_count(&self) -> u32 {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        let count = state.header.chunk_count;
        let mut n = 0u32;
        for i in 0..count {
            if state.chunks[i as usize].status.load(Ordering::Acquire) == STATUS_FAILED {
                n += 1;
            }
        }
        n
    }

    /// Snapshot of chunk `idx`'s state. Returns `None` if out of range.
    /// Useful for diagnostics and SUMMARY generation.
    pub fn chunk_state(&self, idx: u32) -> Option<ChunkStateSnapshot> {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        if idx >= state.header.chunk_count {
            return None;
        }
        let slot = &state.chunks[idx as usize];
        // Acquire on status first; the remaining Relaxed loads are ordered
        // after it in program order, so a snapshot that sees DONE/FAILED
        // also observes the finished_at/failed_files written before the
        // producer's Release store.
        Some(ChunkStateSnapshot {
            status: slot.status.load(Ordering::Acquire),
            claimer_id: slot.claimer_id.load(Ordering::Relaxed),
            module_id: slot.module_id,
            file_start: slot.file_start,
            file_count: slot.file_count,
            total_bytes: slot.total_bytes,
            started_at_ms: slot.started_at_ms.load(Ordering::Relaxed),
            finished_at_ms: slot.finished_at_ms.load(Ordering::Relaxed),
            failed_files: slot.failed_files.load(Ordering::Relaxed),
        })
    }

    /// Number of valid chunks in the queue.
    pub fn chunk_count(&self) -> u32 {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        state.header.chunk_count
    }

    /// Filesystem path backing this shm segment.
    pub fn path(&self) -> &str {
        &self.path
    }
}
