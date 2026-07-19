//! Shared-memory chunk queue for lock-free work-stealing (see
//! DYNAMIC_SCHED_REDESIGN.md §4, §6).
//!
//! The scheduler creates the queue via [`ChunkQueue::create`], writes
//! the planned chunks via [`ChunkQueue::write_chunk`], then spawns
//! workers that each call [`ChunkQueue::claim_next`] in a CAS loop to
//! pull the next pending chunk. State transitions:
//!
//! ```text
//!        claim_next(worker i)              parse 完成
//! PENDING ──────────────► CLAIMED(i) ─────────────────► DONE
//!   ▲                       │  full model failed                       │
//!   │                       └─────────────────────────────────► FAILED
//!   │ watchdog timeout（reset_stale）
//!   └──────── CLAIMED(i) && (now - started_at) > TIMEOUT
//! ```
//!
//! All mutable fields are atomic; no lock-free ring pointer is needed
//! because workers linear-scan for the first `PENDING` slot and CAS it
//! to `CLAIMED` — chunk count (~100-250) makes the scan cost negligible
//! (§4.2).
//!
//! This module is infrastructure for the chunk-level scheduler (P2-P4
//! of the redesign). The current module-level scheduler still uses
//! `shm.rs`; both can coexist during the migration.

#![allow(dead_code)]

use std::ffi::CString;
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use libc::{
    MAP_SHARED, O_CREAT, O_RDWR, PROT_READ, PROT_WRITE, S_IRUSR, S_IWUSR, c_void, close, ftruncate,
    mmap, munmap, open, unlink,
};

/// Magic number stored in [`ChunkQueueHeader::magic`] — ASCII "CUNK"
/// (Chunk Queue) in little-endian. Workers verify this on attach.
const MAGIC: u32 = 0x43554e4b;

/// Layout version stored in [`ChunkQueueHeader::version`]. Bumped if
/// the on-disk layout changes in a backward-incompatible way. A stale
/// shm (older version) is rejected by [`ChunkQueue::open`] so the
/// scheduler can rebuild the queue (§8.2).
const VERSION: u32 = 1;

/// Maximum chunk slots in the queue. Sized for the largest expected
/// project (50k files × ~200 files/chunk ≈ 250 chunks, plus 10%
/// headroom). Going beyond 256 would suggest `TARGET_BYTES` is too
/// small and the planner should be re-tuned instead.
pub const MAX_CHUNKS: usize = 256;

/// Chunk status: not yet claimed by any worker.
const STATUS_PENDING: u32 = 0;
/// Chunk status: a worker has claimed it and is parsing.
const STATUS_CLAIMED: u32 = 1;
/// Chunk status: all files in this chunk were processed (success or
/// individual file failures — see `failed_files`).
const STATUS_DONE: u32 = 2;
/// Chunk status: every file in this chunk failed (≥ FAIL_RATE_LIMIT).
/// Triggers quarantine for the chunk's file set.
const STATUS_FAILED: u32 = 3;

/// Per-chunk state, laid out `#[repr(C)]` for cross-process mmap.
///
/// `status`, `claimer_id`, `started_at_ms`, `finished_at_ms`, and
/// `failed_files` are atomic — they're mutated by workers after the
/// scheduler writes the initial `PENDING` state. The remaining fields
/// (`module_id`, `file_start`, `file_count`, `total_bytes`) are
/// immutable after `write_chunk` and read-only from the worker side.
#[repr(C)]
pub struct ChunkState {
    /// 0=PENDING, 1=CLAIMED, 2=DONE, 3=FAILED.
    pub status: AtomicU32,
    /// Worker index that owns this chunk when `status == CLAIMED`.
    /// 0xFFFFFFFF (`u32::MAX`) means "unclaimed".
    pub claimer_id: AtomicU32,
    /// Module index this chunk belongs to (for quarantine exclusion).
    pub module_id: u32,
    /// Start index into the scheduler's per-module file vector.
    pub file_start: u32,
    /// Number of files in this chunk.
    pub file_count: u32,
    /// Total bytes of all files in this chunk (load accounting).
    pub total_bytes: u64,
    /// Milliseconds since UNIX_EPOCH when the chunk was claimed.
    /// Used by `reset_stale` to detect crashed workers (§8).
    pub started_at_ms: AtomicU64,
    /// Milliseconds since UNIX_EPOCH when the chunk reached DONE/FAILED.
    pub finished_at_ms: AtomicU64,
    /// Number of files in this chunk that failed parse/translate
    /// (fail-fast bookkeeping, §7.3).
    pub failed_files: AtomicU32,
}

impl ChunkState {
    /// Initialise a slot to the PENDING state with no claimer and
    /// immutable metadata set. Called by the scheduler's `write_chunk`.
    ///
    /// `total_bytes` is stored as a `u64` but the planner input is
    /// already bounded by `DEFAULT_MAX_BYTES` × `MAX_CHUNKS`, so it
    /// never overflows.
    fn init(&self, module_id: u32, file_start: u32, file_count: u32, total_bytes: u64) {
        self.status.store(STATUS_PENDING, Ordering::Relaxed);
        self.claimer_id.store(u32::MAX, Ordering::Relaxed);
        // SAFETY: these are plain u32/u64 writes; the scheduler calls
        // init() before any worker opens the shm, so exclusive access
        // is guaranteed by the caller's happens-before relationship.
        // We write through the AtomicU32/U64 storage so subsequent
        // worker reads via `load` are well-defined.
        // NOTE: module_id/file_start/file_count/total_bytes are NOT
        // atomic — they're written once here and never mutated. Workers
        // must only read them after observing `status != PENDING` via
        // an atomic load (acquire ordering not required because we use
        // Relaxed everywhere and rely on the CAS in claim_next for
        // visibility).
        unsafe {
            let p = self as *const Self as *mut Self;
            (*p).module_id = module_id;
            (*p).file_start = file_start;
            (*p).file_count = file_count;
            (*p).total_bytes = total_bytes;
        }
        self.started_at_ms.store(0, Ordering::Relaxed);
        self.finished_at_ms.store(0, Ordering::Relaxed);
        self.failed_files.store(0, Ordering::Relaxed);
    }
}

/// Shm header — magic + version + chunk_count. Workers check
/// magic/version on attach; chunk_count tells `claim_next` how far to
/// scan (avoids touching uninitialised slots past the planned range).
#[repr(C)]
pub struct ChunkQueueHeader {
    /// Must be [`MAGIC`] (= 0x43554e4b).
    pub magic: u32,
    /// Must be [`VERSION`] (= 1).
    pub version: u32,
    /// Number of valid chunks in `chunks[0..chunk_count]`. Set by the
    /// scheduler after all `write_chunk` calls; workers read it once
    /// on attach.
    pub chunk_count: u32,
    /// Reserved for future fields — keeps the header cache-aligned so
    /// `chunks[0]` starts on a clean line.
    pub reserved: [u32; 13],
}

/// Full shm layout: header + chunk array.
#[repr(C)]
pub struct ChunkQueueState {
    pub header: ChunkQueueHeader,
    pub chunks: [ChunkState; MAX_CHUNKS],
}

/// RAII wrapper around a `mmap`'d [`ChunkQueueState`].
///
/// Owner (scheduler) creates via [`ChunkQueue::create`] and unlinks on
/// `Drop`. Workers attach via [`ChunkQueue::open`] and only `munmap` on
/// drop — they do not unlink.
pub struct ChunkQueue {
    path: String,
    fd: RawFd,
    ptr: *mut ChunkQueueState,
    is_owner: bool,
}

// Manual Debug impl — `*mut ChunkQueueState` doesn't auto-derive Debug.
impl std::fmt::Debug for ChunkQueue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ChunkQueue")
            .field("path", &self.path)
            .field("fd", &self.fd)
            .field("is_owner", &self.is_owner)
            .finish_non_exhaustive()
    }
}

/// Read-only snapshot of a chunk's state. Returned by
/// [`ChunkQueue::chunk_state`] so callers don't need to touch raw
/// atomics or hold the shm lock.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ChunkStateSnapshot {
    pub status: u32,
    pub claimer_id: u32,
    pub module_id: u32,
    pub file_start: u32,
    pub file_count: u32,
    pub total_bytes: u64,
    pub started_at_ms: u64,
    pub finished_at_ms: u64,
    pub failed_files: u32,
}

impl ChunkQueue {
    /// Scheduler creates a new shm file and zeroes the chunk array.
    ///
    /// `chunk_count` is clamped to [`MAX_CHUNKS`]. The scheduler calls
    /// [`ChunkQueue::write_chunk`] afterwards to populate each slot's
    /// `module_id`/`file_start`/`file_count`/`total_bytes`; until then
    /// every slot is `PENDING` with `file_count == 0` (workers must
    /// not consume the queue before all writes are done — caller's
    /// responsibility).
    pub fn create(path: &str, chunk_count: u32) -> Result<Self, String> {
        let c_path = CString::new(path).map_err(|e| {
            format!(
                "path contains NUL byte: {} [module=scheduler, method=ChunkQueue::create]",
                e
            )
        })?;

        // SAFETY: c_path is a valid NUL-terminated CString; O_CREAT|O_RDWR
        // creates the file if absent; mode 0600 restricts to owner.
        let fd = unsafe {
            open(
                c_path.as_ptr(),
                O_CREAT | O_RDWR,
                (S_IRUSR | S_IWUSR) as libc::c_int,
            )
        };
        if fd < 0 {
            return Err(format!(
                "open failed: {} [module=scheduler, method=ChunkQueue::create, path={}]",
                std::io::Error::last_os_error(),
                path,
            ));
        }

        let size = std::mem::size_of::<ChunkQueueState>();
        // SAFETY: fd is valid; ftruncate sizes the file to exactly
        // `size` bytes. Cast to i64 is safe because the struct is
        // roughly 256 * 64 = ~16KB.
        if unsafe { ftruncate(fd, size as i64) } != 0 {
            let err = std::io::Error::last_os_error();
            // SAFETY: fd is valid; close it to leak no fd.
            unsafe { close(fd) };
            return Err(format!(
                "ftruncate failed: {} [module=scheduler, method=ChunkQueue::create]",
                err
            ));
        }

        // SAFETY: fd is valid and points to a file we just sized.
        // MAP_SHARED makes writes visible to other processes that map
        // the same file.
        let ptr = unsafe {
            mmap(
                std::ptr::null_mut(),
                size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                0,
            )
        };
        if ptr == libc::MAP_FAILED {
            let err = std::io::Error::last_os_error();
            // SAFETY: fd is valid; close it to leak no fd.
            unsafe { close(fd) };
            return Err(format!(
                "mmap failed: {} [module=scheduler, method=ChunkQueue::create]",
                err
            ));
        }

        let clamped = std::cmp::min(chunk_count, MAX_CHUNKS as u32);

        // SAFETY: ptr is a valid, properly-aligned pointer to memory
        // returned by mmap and sized to size_of::<ChunkQueueState>().
        // The file was just created/truncated so the bytes are zero.
        // No worker has opened the file yet (caller's responsibility),
        // so exclusive mutable access here is safe.
        let state = unsafe { &mut *(ptr as *mut ChunkQueueState) };
        state.header.magic = MAGIC;
        state.header.version = VERSION;
        state.header.chunk_count = clamped;
        state.header.reserved = [0u32; 13];
        // ChunkState fields are already zero from ftruncate — but
        // `status` must be explicitly initialised because zero IS
        // STATUS_PENDING (so it's fine) and `claimer_id` zero would
        // look like worker 0 claimed it, so we set it to u32::MAX.
        for slot in state.chunks.iter_mut() {
            slot.status.store(STATUS_PENDING, Ordering::Relaxed);
            slot.claimer_id.store(u32::MAX, Ordering::Relaxed);
            slot.started_at_ms.store(0, Ordering::Relaxed);
            slot.finished_at_ms.store(0, Ordering::Relaxed);
            slot.failed_files.store(0, Ordering::Relaxed);
            // module_id/file_start/file_count/total_bytes stay 0
            // until write_chunk populates them.
        }

        Ok(Self {
            path: path.to_string(),
            fd,
            ptr: ptr as *mut ChunkQueueState,
            is_owner: true,
        })
    }

    /// Worker opens an existing shm created by the scheduler.
    ///
    /// Verifies magic + version. Returns `Err` with a tagged message
    /// if `open`, `mmap`, or the magic/version check fails.
    pub fn open(path: &str) -> Result<Self, String> {
        let c_path = CString::new(path).map_err(|e| {
            format!(
                "path contains NUL byte: {} [module=scheduler, method=ChunkQueue::open]",
                e
            )
        })?;

        // SAFETY: c_path is valid NUL-terminated; O_RDWR (no O_CREAT).
        let fd = unsafe { open(c_path.as_ptr(), O_RDWR) };
        if fd < 0 {
            return Err(format!(
                "open failed: {} [module=scheduler, method=ChunkQueue::open, path={}]",
                std::io::Error::last_os_error(),
                path,
            ));
        }

        let size = std::mem::size_of::<ChunkQueueState>();
        // SAFETY: fd is valid; the file was created and sized by
        // ChunkQueue::create, so the mapping fits the file exactly.
        let ptr = unsafe {
            mmap(
                std::ptr::null_mut(),
                size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                0,
            )
        };
        if ptr == libc::MAP_FAILED {
            let err = std::io::Error::last_os_error();
            // SAFETY: fd is valid; close it to leak no fd.
            unsafe { close(fd) };
            return Err(format!(
                "mmap failed: {} [module=scheduler, method=ChunkQueue::open]",
                err
            ));
        }

        // SAFETY: ptr is valid ChunkQueueState-aligned memory returned
        // by mmap. We only read header.magic and header.version here
        // (non-atomic u32); the scheduler writes them once in create()
        // before any worker opens the file, so the read is race-free.
        let state = unsafe { &*(ptr as *const ChunkQueueState) };
        // Cache the values BEFORE any munmap — reading through `state`
        // after `munmap(ptr, size)` would be use-after-unmap.
        let observed_magic = state.header.magic;
        let observed_version = state.header.version;
        if observed_magic != MAGIC {
            // SAFETY: ptr was returned by mmap with `size` bytes.
            unsafe { munmap(ptr, size) };
            // SAFETY: fd is valid; close it to leak no fd.
            unsafe { close(fd) };
            return Err(format!(
                "magic mismatch: 0x{:08x} (expected 0x{:08x}) \
                 [module=scheduler, method=ChunkQueue::open]",
                observed_magic, MAGIC,
            ));
        }
        if observed_version != VERSION {
            // SAFETY: ptr was returned by mmap with `size` bytes.
            unsafe { munmap(ptr, size) };
            // SAFETY: fd is valid; close it to leak no fd.
            unsafe { close(fd) };
            return Err(format!(
                "version mismatch: {} (expected {}) \
                 [module=scheduler, method=ChunkQueue::open]",
                observed_version, VERSION,
            ));
        }

        Ok(Self {
            path: path.to_string(),
            fd,
            ptr: ptr as *mut ChunkQueueState,
            is_owner: false,
        })
    }

    /// Scheduler writes a chunk's immutable metadata into slot `idx`.
    ///
    /// Initialises `status = PENDING` and zeros the timing/claimer
    /// fields. Must be called BEFORE any worker calls `claim_next`.
    /// Returns `Err` if `idx >= chunk_count`.
    pub fn write_chunk(
        &self,
        idx: u32,
        module_id: u32,
        file_start: u32,
        file_count: u32,
        total_bytes: u64,
    ) -> Result<(), String> {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        let count = state.header.chunk_count;
        if idx >= count {
            return Err(format!(
                "chunk idx {} out of range (chunk_count={}) \
                 [module=scheduler, method=ChunkQueue::write_chunk]",
                idx, count,
            ));
        }
        // SAFETY: chunks[idx] is within bounds (checked above). The
        // scheduler is the sole writer at this point (workers haven't
        // been spawned yet), so exclusive access is safe.
        let slot = &state.chunks[idx as usize];
        slot.init(module_id, file_start, file_count, total_bytes);
        Ok(())
    }

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
            match slot.status.compare_exchange(
                STATUS_PENDING,
                STATUS_CLAIMED,
                Ordering::Relaxed,
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
        slot.finished_at_ms.store(now_ms(), Ordering::Relaxed);
        slot.status.store(STATUS_DONE, Ordering::Relaxed);
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
        slot.finished_at_ms.store(now_ms(), Ordering::Relaxed);
        slot.status.store(STATUS_FAILED, Ordering::Relaxed);
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
        slot.failed_files.fetch_add(1, Ordering::Relaxed);
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
        let started = slot.started_at_ms.load(Ordering::Relaxed);
        if started == 0 {
            return false; // never claimed
        }
        let elapsed = now_ms().saturating_sub(started);
        if elapsed < timeout_ms {
            return false;
        }
        // CAS CLAIMED → PENDING so we don't clobber a worker that just
        // finished (race between watchdog and worker completion).
        match slot.status.compare_exchange(
            STATUS_CLAIMED,
            STATUS_PENDING,
            Ordering::Relaxed,
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

    /// Returns true if every chunk is in DONE or FAILED state.
    /// Used by the scheduler's main loop to detect completion.
    pub fn is_complete(&self) -> bool {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        let count = state.header.chunk_count;
        for i in 0..count {
            let s = state.chunks[i as usize].status.load(Ordering::Relaxed);
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
            if state.chunks[i as usize].status.load(Ordering::Relaxed) == STATUS_PENDING {
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
            if state.chunks[i as usize].status.load(Ordering::Relaxed) == STATUS_DONE {
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
            if state.chunks[i as usize].status.load(Ordering::Relaxed) == STATUS_FAILED {
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
        Some(ChunkStateSnapshot {
            status: slot.status.load(Ordering::Relaxed),
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

impl Drop for ChunkQueue {
    fn drop(&mut self) {
        let size = std::mem::size_of::<ChunkQueueState>();
        if !self.ptr.is_null() {
            // SAFETY: self.ptr was returned by mmap with `size` bytes and
            // has not been unmapped yet (Drop runs once per instance).
            unsafe {
                munmap(self.ptr as *mut c_void, size);
            }
        }
        if self.fd >= 0 {
            // SAFETY: self.fd is a valid open descriptor (or already
            // closed, in which case close returns EBADF — harmless).
            unsafe {
                close(self.fd);
            }
        }
        // Only the scheduler (owner) unlinks the file. Workers just
        // munmap+close — the inode persists until the scheduler unlinks,
        // and any in-flight mmap references stay valid (POSIX semantics).
        if self.is_owner
            && let Ok(c_path) = CString::new(self.path.clone())
        {
            // SAFETY: c_path is a valid NUL-terminated CString. unlink
            // removes the directory entry; existing mmap references
            // remain valid until munmap (POSIX shared memory semantics).
            unsafe {
                unlink(c_path.as_ptr());
            }
        }
    }
}

// SAFETY: ChunkQueue is Send for the same reasons as SchedShm:
// - `ptr` points to mmap'd memory (MAP_SHARED) that is process-global;
//   moving the Rust handle across threads does not affect the underlying
//   memory or its visibility to other processes.
// - `fd` (RawFd = i32), `path` (String), and `is_owner` (bool) are all
//   Send by default.
unsafe impl Send for ChunkQueue {}

// SAFETY: ChunkQueue is Sync for the same reasons as SchedShm:
// - All mutable operations on the mmap'd ChunkQueueState go through
//   `AtomicU32`/`AtomicU64` (`status`, `claimer_id`, `started_at_ms`,
//   `finished_at_ms`, `failed_files`). Both atomics are `Sync`, so
//   multiple threads calling `claim_next`/`mark_done`/etc. concurrently
//   on the same `&ChunkQueue` is safe — the atomics serialise the
//   actual state changes.
// - The non-atomic fields (`magic`, `version`, `chunk_count`,
//   `reserved`, `module_id`, `file_start`, `file_count`, `total_bytes`)
//   are written once in `create()`/`write_chunk()` and treated as
//   read-only afterwards. `write_chunk` completes before workers are
//   spawned (caller's responsibility), so there are no concurrent
//   reads during the writes.
// - `fd` and `is_owner` are only accessed in `Drop`, which takes
//   `&mut self` (exclusive access) — no concurrent access is possible.
unsafe impl Sync for ChunkQueue {}

/// Current time in milliseconds since UNIX_EPOCH. Returns 0 if the
/// system clock is before epoch (only possible on misconfigured hosts).
fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::sync::atomic::AtomicU64;

    /// Monotonic counter appended to test paths so parallel test
    /// processes never collide on the same /tmp file.
    static TEST_COUNTER: AtomicU64 = AtomicU64::new(0);

    fn unique_path() -> String {
        let pid = std::process::id();
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0);
        let counter = TEST_COUNTER.fetch_add(1, Ordering::Relaxed);
        format!(
            "/tmp/codescope_chunkq_test_{}_{}_{}.shm",
            pid, nanos, counter
        )
    }

    #[test]
    fn test_create_open_chunk_count() {
        let path = unique_path();
        let q = ChunkQueue::create(&path, 10).expect("create");
        assert_eq!(q.chunk_count(), 10);
        assert_eq!(q.path(), path);

        // Worker opens and sees the same chunk_count.
        let w = ChunkQueue::open(&path).expect("open");
        assert_eq!(w.chunk_count(), 10);

        // Initially all 10 chunks are PENDING.
        assert_eq!(w.pending_count(), 10);
        assert_eq!(w.done_count(), 0);
        assert_eq!(w.failed_count(), 0);
        assert!(!w.is_complete());
    }

    #[test]
    fn test_create_clamps_to_max_chunks() {
        let path = unique_path();
        let q = ChunkQueue::create(&path, MAX_CHUNKS as u32 * 2).expect("create");
        assert_eq!(q.chunk_count(), MAX_CHUNKS as u32);
    }

    #[test]
    fn test_open_rejects_wrong_magic() {
        let path = unique_path();
        std::fs::write(&path, b"not a chunk queue").expect("write");
        let result = ChunkQueue::open(&path);
        assert!(result.is_err());
        let err = result.unwrap_err();
        assert!(err.contains("magic mismatch"), "err: {}", err);
        assert!(err.contains("module=scheduler"));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn test_write_chunk_and_snapshot() {
        let path = unique_path();
        let q = ChunkQueue::create(&path, 3).expect("create");

        q.write_chunk(0, 1, 0, 100, 1_000_000).expect("write 0");
        q.write_chunk(1, 1, 100, 200, 2_500_000).expect("write 1");
        q.write_chunk(2, 2, 0, 50, 800_000).expect("write 2");

        let snap0 = q.chunk_state(0).expect("snap 0");
        assert_eq!(snap0.status, STATUS_PENDING);
        assert_eq!(snap0.module_id, 1);
        assert_eq!(snap0.file_start, 0);
        assert_eq!(snap0.file_count, 100);
        assert_eq!(snap0.total_bytes, 1_000_000);
        assert_eq!(snap0.claimer_id, u32::MAX);
        assert_eq!(snap0.started_at_ms, 0);

        // Out-of-range write should Err, not panic.
        let err = q.write_chunk(3, 1, 0, 1, 1).unwrap_err();
        assert!(err.contains("out of range"));
    }

    #[test]
    fn test_claim_next_serialises_correctly() {
        let path = unique_path();
        let q = ChunkQueue::create(&path, 5).expect("create");
        for i in 0..5u32 {
            q.write_chunk(i, 1, i * 10, 10, 100_000).expect("write");
        }

        // Single worker claims sequentially.
        let w = ChunkQueue::open(&path).expect("open");
        let mut claimed = Vec::new();
        while let Some(idx) = w.claim_next(7) {
            claimed.push(idx);
        }
        claimed.sort();
        assert_eq!(claimed, vec![0, 1, 2, 3, 4], "each chunk claimed once");

        // claimer_id on each slot should be 7.
        for i in 0..5u32 {
            let s = w.chunk_state(i).expect("snap");
            assert_eq!(s.status, STATUS_CLAIMED);
            assert_eq!(s.claimer_id, 7);
            assert!(s.started_at_ms > 0, "started_at_ms must be set");
        }

        // No more PENDING chunks.
        assert_eq!(w.pending_count(), 0);
        assert!(w.claim_next(7).is_none());
    }

    #[test]
    fn test_concurrent_claim_no_duplicates() {
        // Spawn 4 threads, each loops claim_next until None. Across all
        // threads, every chunk index must be claimed exactly once —
        // CAS guarantees no duplicates.
        let path = unique_path();
        let n_chunks = 100u32;
        let q = Arc::new(ChunkQueue::create(&path, n_chunks).expect("create"));
        for i in 0..n_chunks {
            q.write_chunk(i, 1, i, 10, 100_000).expect("write");
        }

        let n_threads = 4;
        let mut handles = Vec::new();
        let claimed_per_thread: Vec<Arc<std::sync::Mutex<Vec<u32>>>> = (0..n_threads)
            .map(|_| Arc::new(std::sync::Mutex::new(Vec::new())))
            .collect();

        for (t, claimed_slot) in claimed_per_thread.iter().enumerate() {
            let q = Arc::clone(&q);
            let claimed = Arc::clone(claimed_slot);
            handles.push(std::thread::spawn(move || {
                while let Some(idx) = q.claim_next(t as u32) {
                    claimed.lock().unwrap().push(idx);
                }
            }));
        }
        for h in handles {
            h.join().unwrap();
        }

        // Merge all claimed indices and verify they cover 0..n_chunks
        // exactly once.
        let mut all: Vec<u32> = Vec::new();
        for c in &claimed_per_thread {
            all.extend(c.lock().unwrap().iter().copied());
        }
        all.sort();
        let expected: Vec<u32> = (0..n_chunks).collect();
        assert_eq!(all, expected, "every chunk claimed exactly once");

        // No PENDING left; all CLAIMED.
        assert_eq!(q.pending_count(), 0);
    }

    #[test]
    fn test_mark_done_failed_and_completion() {
        let path = unique_path();
        let q = ChunkQueue::create(&path, 3).expect("create");
        for i in 0..3u32 {
            q.write_chunk(i, 1, i * 10, 10, 100_000).expect("write");
        }

        let idx0 = q.claim_next(0).expect("claim 0");
        let idx1 = q.claim_next(1).expect("claim 1");
        let idx2 = q.claim_next(2).expect("claim 2");
        assert_eq!(idx0, 0);
        assert_eq!(idx1, 1);
        assert_eq!(idx2, 2);

        q.mark_done(idx0);
        q.mark_done(idx1);
        q.mark_failed(idx2);

        assert_eq!(q.done_count(), 2);
        assert_eq!(q.failed_count(), 1);
        assert_eq!(q.pending_count(), 0);
        assert!(q.is_complete(), "DONE + FAILED counts as complete");

        // finished_at_ms should be set on done/failed chunks.
        let s0 = q.chunk_state(0).expect("snap 0");
        assert!(s0.finished_at_ms > 0);
        assert_eq!(s0.status, STATUS_DONE);
        let s2 = q.chunk_state(2).expect("snap 2");
        assert!(s2.finished_at_ms > 0);
        assert_eq!(s2.status, STATUS_FAILED);
    }

    #[test]
    fn test_inc_failed_files() {
        let path = unique_path();
        let q = ChunkQueue::create(&path, 1).expect("create");
        q.write_chunk(0, 1, 0, 100, 1_000_000).expect("write");

        let _ = q.claim_next(0).expect("claim");
        q.inc_failed_files(0);
        q.inc_failed_files(0);
        q.inc_failed_files(0);

        let s = q.chunk_state(0).expect("snap");
        assert_eq!(s.failed_files, 3);

        // Out-of-range inc is a silent no-op.
        q.inc_failed_files(999);
    }

    #[test]
    fn test_reset_stale_recovers_crashed_worker() {
        let path = unique_path();
        let q = ChunkQueue::create(&path, 1).expect("create");
        q.write_chunk(0, 1, 0, 10, 100_000).expect("write");

        // Worker 5 claims the chunk but never finishes (simulated crash).
        let _ = q.claim_next(5).expect("claim");
        let s = q.chunk_state(0).expect("snap");
        assert_eq!(s.status, STATUS_CLAIMED);
        assert_eq!(s.claimer_id, 5);

        // To make the test deterministic, manually set started_at_ms to
        // a tiny value (1ms since epoch = 1970) so `elapsed` is huge
        // and reset_stale always fires. Without this the test would
        // depend on real wall-clock time advancing past `timeout_ms`.
        // SAFETY: q.ptr is valid; reading/writing AtomicU64 through a
        // shared reference is safe.
        unsafe {
            let state = &*q.ptr;
            state.chunks[0].started_at_ms.store(1, Ordering::Relaxed);
        }
        let reset = q.reset_stale(0, 1);
        // elapsed = now - 1 ≫ 1ms (since 1ms since epoch is 1970).
        assert!(reset, "stale chunk must be reset");

        let s = q.chunk_state(0).expect("snap");
        assert_eq!(s.status, STATUS_PENDING, "back to PENDING");
        assert_eq!(s.claimer_id, u32::MAX, "claimer cleared");

        // A different worker can now claim it.
        let idx = q.claim_next(9).expect("re-claim");
        assert_eq!(idx, 0);
        let s = q.chunk_state(0).expect("snap");
        assert_eq!(s.claimer_id, 9);
    }

    #[test]
    fn test_reset_stale_does_not_clobber_finished() {
        let path = unique_path();
        let q = ChunkQueue::create(&path, 1).expect("create");
        q.write_chunk(0, 1, 0, 10, 100_000).expect("write");
        let _ = q.claim_next(0).expect("claim");
        q.mark_done(0);

        // Even with a forced-stale started_at_ms, reset_stale should
        // fail because the CAS only matches CLAIMED.
        unsafe {
            let state = &*q.ptr;
            state.chunks[0].started_at_ms.store(1, Ordering::Relaxed);
        }
        let reset = q.reset_stale(0, 1);
        assert!(!reset, "must not reset a DONE chunk");
        let s = q.chunk_state(0).expect("snap");
        assert_eq!(s.status, STATUS_DONE);
    }

    #[test]
    fn test_open_rejects_wrong_version() {
        // Create a valid queue, then corrupt the version field and
        // verify open() rejects it. Keep `q` alive so the file isn't
        // unlinked before the open() attempt.
        let path = unique_path();
        let q = ChunkQueue::create(&path, 2).expect("create");
        // SAFETY: q.ptr is valid; we hold the only reference (no workers
        // spawned), so exclusive mutable access is safe.
        unsafe {
            let state = &mut *q.ptr;
            state.header.version = 999;
        }
        let result = ChunkQueue::open(&path);
        assert!(result.is_err());
        let err = result.unwrap_err();
        assert!(err.contains("version mismatch"), "err: {}", err);
        assert!(err.contains("module=scheduler"));
    }

    #[test]
    fn test_out_of_range_ops_are_safe() {
        let path = unique_path();
        let q = ChunkQueue::create(&path, 1).expect("create");

        // None of these should panic.
        q.mark_done(999);
        q.mark_failed(999);
        q.inc_failed_files(999);
        assert!(q.chunk_state(999).is_none());
        assert!(!q.reset_stale(999, 0));
    }
}
