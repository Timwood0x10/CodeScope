//! Shared-memory infrastructure for dynamic CPU scheduling.
//!
//! `SchedState` is a `#[repr(C)]` struct stored in a POSIX shared-memory
//! file (`/tmp/codescope_sched_<pid>.shm`) mapped `MAP_SHARED` so the
//! scheduler process and worker subprocesses observe the same atomic
//! counters. Workers claim cores from a shared pool at startup and
//! release them on completion; the scheduler polls `generation` to
//! dispatch queued workers when cores become available.
//!
//! All mutable fields are `AtomicU32` and use `Ordering::Relaxed` —
//! we only need cross-core visibility, not strict acquire/release
//! ordering (the scheduler re-reads state in a poll loop anyway).
//!
//! Owner (scheduler) creates the file via [`SchedShm::create`] and is
//! responsible for unlinking it on shutdown (handled in `Drop`). Workers
//! attach via [`SchedShm::open`] and only munmap on drop.

// Worker-side helpers (open/register_worker/mark_done/set_worker_cores/
// can_grab/poll_interval_ms/path) and the status constants they reference
// are kept for the engine's monitor-thread implementation (see
// engine/src/engine_index_project.cpp) — the engine reads the shm directly
// via mmap, so these Rust helpers remain unused by the scheduler itself.
#![allow(dead_code)]

use std::ffi::CString;
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicU32, Ordering};

use libc::{
    MAP_SHARED, O_CREAT, O_RDWR, PROT_READ, PROT_WRITE, S_IRUSR, S_IWUSR, c_void, close, ftruncate,
    mmap, munmap, open, unlink,
};
#[cfg(windows)]
const MAP_FAILED: *mut libc::c_void = !0 as *mut libc::c_void;

/// Magic number stored in [`SchedState::magic`] — ASCII "SCHD" in
/// little-endian. Workers check this on attach to verify the scheduler
/// initialised the segment before they opened it.
const MAGIC: u32 = 0x53434844;

/// Layout version stored in [`SchedState::version`]. Bumped if the
/// `SchedState` struct layout changes in a backward-incompatible way.
const VERSION: u32 = 1;

/// Maximum number of worker slots tracked in `worker_status`/`worker_cores`.
const MAX_WORKERS: usize = 64;

/// Worker status value: slot is free, never registered.
const STATUS_IDLE: u32 = 0;

/// Worker status value: worker is running and may hold cores.
const STATUS_RUNNING: u32 = 1;

/// Worker status value: worker finished, cores returned to pool.
const STATUS_DONE: u32 = 2;

/// Aggressive poll interval (ms) — used when `aggressive == 1`.
const POLL_AGGRESSIVE_MS: u64 = 50;

/// Normal poll interval (ms) — used when `aggressive == 0`.
const POLL_NORMAL_MS: u64 = 100;

/// Cross-process scheduling state, laid out for POSIX `mmap`.
///
/// All mutable fields are `AtomicU32` so cross-process writes are
/// data-race-free. The non-atomic fields (`magic`, `version`,
/// `worker_count`, `reserved`) are written once by the scheduler
/// during [`SchedShm::create`] and treated as read-only afterwards.
#[repr(C)]
pub struct SchedState {
    /// Sanity check — must be [`MAGIC`] (= 0x53434844).
    pub magic: u32,
    /// Layout version — currently [`VERSION`] (= 1).
    pub version: u32,
    /// Total CPU cores on the system (informational; never changes).
    pub total_cores: AtomicU32,
    /// Cores currently available in the shared pool.
    pub available_cores: AtomicU32,
    /// Number of workers in RUNNING state.
    pub active_workers: AtomicU32,
    /// Bumped on every state change so polling workers can detect updates
    /// without re-reading every field each iteration.
    pub generation: AtomicU32,
    /// Memory ceiling in MB. [`SchedShm::can_grab`] returns false when
    /// `current_mem_mb` exceeds this.
    pub mem_limit_mb: AtomicU32,
    /// Current total memory usage in MB (scheduler updates this).
    pub current_mem_mb: AtomicU32,
    /// 1 = aggressive mode (50ms poll), 0 = normal (100ms).
    pub aggressive: AtomicU32,
    /// Expected total worker count (informational; managed by caller).
    pub worker_count: u32,
    /// Reserved for future fields — keeps the layout cache-line aligned.
    pub reserved: [u32; 8],
    /// Per-worker status: 0=idle, 1=running, 2=done.
    pub worker_status: [AtomicU32; 64],
    /// Per-worker current core count.
    pub worker_cores: [AtomicU32; 64],
}

/// RAII wrapper around a `mmap`'d [`SchedState`].
///
/// The owner (scheduler) creates the file via [`SchedShm::create`] and
/// unlinks it on `Drop`. Workers attach via [`SchedShm::open`] and
/// only `munmap` on drop — they do not unlink.
pub struct SchedShm {
    path: String,
    fd: RawFd,
    ptr: *mut SchedState,
    is_owner: bool,
}

// Manual Debug impl — needed because `*mut SchedState` is a raw pointer
// and does not auto-derive Debug. Tests use `Result<SchedShm, _>::unwrap_err()`
// which requires `SchedShm: Debug`. We only emit the path + fd so we don't
// leak the mmap address in logs.
impl std::fmt::Debug for SchedShm {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SchedShm")
            .field("path", &self.path)
            .field("fd", &self.fd)
            .field("is_owner", &self.is_owner)
            .finish_non_exhaustive()
    }
}

impl SchedShm {
    /// Scheduler creates a new shared-memory file and initialises [`SchedState`].
    ///
    /// `path` — file path, typically `/tmp/codescope_sched_<pid>.shm`.
    /// `total_cores` — total CPU budget workers can claim from.
    /// `mem_limit_mb` — RSS ceiling; [`SchedShm::can_grab`] returns false
    /// when `current_mem_mb` exceeds this.
    ///
    /// Returns `Err` with a tagged message if `open`, `ftruncate`, or
    /// `mmap` fails. On success the file is sized to `size_of::<SchedState>()`
    /// and all fields are initialised before returning.
    pub fn create(path: &str, total_cores: u32, mem_limit_mb: u32) -> Result<Self, String> {
        let c_path = CString::new(path).map_err(|e| {
            format!(
                "path contains NUL byte: {} [module=scheduler, method=SchedShm::create]",
                e
            )
        })?;

        // SAFETY: c_path is a valid NUL-terminated CString; O_CREAT|O_RDWR
        // creates the file if absent, mode 0600 restricts read/write to owner.
        // Cast mode bits to c_int — on macOS S_IRUSR|S_IWUSR are u16, but
        // libc::open's variadic mode arg is c_int (see open(2)).
        let fd = unsafe {
            open(
                c_path.as_ptr(),
                O_CREAT | O_RDWR,
                (S_IRUSR | S_IWUSR) as libc::c_int,
            )
        };
        if fd < 0 {
            return Err(format!(
                "open failed: {} [module=scheduler, method=SchedShm::create, path={}]",
                std::io::Error::last_os_error(),
                path,
            ));
        }

        let size = std::mem::size_of::<SchedState>();
        // SAFETY: fd is a valid open descriptor. ftruncate sizes the file
        // to exactly `size` bytes; cast to i64 is safe because the struct
        // is ~584 bytes, well within off_t range.
        if unsafe { ftruncate(fd, size as i64) } != 0 {
            let err = std::io::Error::last_os_error();
            // SAFETY: fd is a valid open descriptor; close it to leak no fd.
            unsafe { close(fd) };
            return Err(format!(
                "ftruncate failed: {} [module=scheduler, method=SchedShm::create]",
                err
            ));
        }

        // SAFETY: fd is valid and points to a file we just sized to `size`.
        // MAP_SHARED makes writes visible to other processes that map the
        // same file. PROT_READ|PROT_WRITE allows atomic counter updates.
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
            // SAFETY: fd is a valid open descriptor; close it to leak no fd.
            unsafe { close(fd) };
            return Err(format!(
                "mmap failed: {} [module=scheduler, method=SchedShm::create]",
                err
            ));
        }

        // SAFETY: ptr is a valid, properly-aligned pointer to memory
        // returned by mmap and sized to size_of::<SchedState>(). The
        // file was just created/truncated, so the bytes are zero. No
        // other process has opened this file yet (caller's responsibility
        // to call create() before spawning workers), so exclusive mutable
        // access here is safe.
        let state = unsafe { &mut *(ptr as *mut SchedState) };
        state.magic = MAGIC;
        state.version = VERSION;
        state.total_cores.store(total_cores, Ordering::Relaxed);
        state.available_cores.store(total_cores, Ordering::Relaxed);
        state.active_workers.store(0, Ordering::Relaxed);
        state.generation.store(0, Ordering::Relaxed);
        state.mem_limit_mb.store(mem_limit_mb, Ordering::Relaxed);
        state.current_mem_mb.store(0, Ordering::Relaxed);
        state.aggressive.store(0, Ordering::Relaxed);
        state.worker_count = 0;
        state.reserved = [0u32; 8];
        for slot in state.worker_status.iter_mut() {
            slot.store(STATUS_IDLE, Ordering::Relaxed);
        }
        for slot in state.worker_cores.iter_mut() {
            slot.store(0, Ordering::Relaxed);
        }

        Ok(Self {
            path: path.to_string(),
            fd,
            ptr: ptr as *mut SchedState,
            is_owner: true,
        })
    }

    /// Set the aggressive polling flag (1=aggressive 50ms, 0=normal 100ms).
    /// Scheduler calls this after create() to honor CODESCOPE_AGGRESSIVE.
    /// Bumps `generation` so polling workers notice the change.
    pub fn set_aggressive(&self, on: bool) {
        // SAFETY: self.ptr is valid for the lifetime of self. We only
        // access `aggressive` and `generation` (both AtomicU32) through
        // a shared reference, which is safe because atomics support
        // concurrent mutable access.
        let state = unsafe { &*self.ptr };
        state
            .aggressive
            .store(if on { 1 } else { 0 }, Ordering::Relaxed);
        state.generation.fetch_add(1, Ordering::Relaxed);
    }

    /// Worker opens an existing shared-memory file created by the scheduler.
    ///
    /// Verifies the magic number to ensure the scheduler initialised the
    /// segment before the worker attached. Returns `Err` with a tagged
    /// message if `open`, `mmap`, or the magic check fails.
    pub fn open(path: &str) -> Result<Self, String> {
        let c_path = CString::new(path).map_err(|e| {
            format!(
                "path contains NUL byte: {} [module=scheduler, method=SchedShm::open]",
                e
            )
        })?;

        // SAFETY: c_path is valid NUL-terminated; O_RDWR (no O_CREAT) —
        // workers don't create the file, the scheduler must have done so.
        let fd = unsafe { open(c_path.as_ptr(), O_RDWR) };
        if fd < 0 {
            return Err(format!(
                "open failed: {} [module=scheduler, method=SchedShm::open, path={}]",
                std::io::Error::last_os_error(),
                path,
            ));
        }

        let size = std::mem::size_of::<SchedState>();
        // SAFETY: fd is valid; the file was created and sized by
        // SchedShm::create, so the mapping fits the file exactly.
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
            // SAFETY: fd is a valid open descriptor; close it to leak no fd.
            unsafe { close(fd) };
            return Err(format!(
                "mmap failed: {} [module=scheduler, method=SchedShm::open]",
                err
            ));
        }

        // SAFETY: ptr is valid SchedState-aligned memory returned by mmap.
        // We only read `magic` here (a non-atomic u32); the scheduler
        // writes it once in create() before any worker opens the file,
        // so the read is race-free.
        let state = unsafe { &*(ptr as *const SchedState) };
        // Cache the magic into a local BEFORE any munmap — reading through
        // `state` after `munmap(ptr, size)` would be use-after-unmap.
        let observed_magic = state.magic;
        if observed_magic != MAGIC {
            // SAFETY: ptr was returned by mmap with `size` bytes; unmap it.
            unsafe { munmap(ptr, size) };
            // SAFETY: fd is a valid open descriptor; close it to leak no fd.
            unsafe { close(fd) };
            return Err(format!(
                "magic mismatch: 0x{:08x} (expected 0x{:08x}) \
                 [module=scheduler, method=SchedShm::open]",
                observed_magic, MAGIC,
            ));
        }

        Ok(Self {
            path: path.to_string(),
            fd,
            ptr: ptr as *mut SchedState,
            is_owner: false,
        })
    }

    /// Atomically claim up to `max_want` cores from the shared pool.
    ///
    /// CAS loop on `available_cores`: loads the current value, computes
    /// `min(loaded, max_want)` as the take, retries on contention. Returns
    /// the number actually claimed (0 if the pool is empty or `max_want`
    /// is 0). Bumps `generation` on success so polling workers notice.
    pub fn claim_cores(&self, max_want: u32) -> u32 {
        if max_want == 0 {
            return 0;
        }
        // SAFETY: self.ptr is valid for the lifetime of self; reads via
        // shared reference are safe because all mutable fields are atomic.
        let state = unsafe { &*self.ptr };
        loop {
            let avail = state.available_cores.load(Ordering::Relaxed);
            if avail == 0 {
                return 0;
            }
            let take = std::cmp::min(avail, max_want);
            let new_avail = avail - take;
            match state.available_cores.compare_exchange(
                avail,
                new_avail,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => {
                    state.generation.fetch_add(1, Ordering::Relaxed);
                    return take;
                }
                Err(_) => continue,
            }
        }
    }

    /// Return `count` cores to the shared pool.
    ///
    /// CAS loop on `available_cores`: loads, saturating-adds `count`, retries
    /// on contention. Bumps `generation` on success. No-op if `count` is 0.
    pub fn release_cores(&self, count: u32) {
        if count == 0 {
            return;
        }
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        loop {
            let avail = state.available_cores.load(Ordering::Relaxed);
            let new_avail = avail.saturating_add(count);
            match state.available_cores.compare_exchange(
                avail,
                new_avail,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => {
                    state.generation.fetch_add(1, Ordering::Relaxed);
                    return;
                }
                Err(_) => continue,
            }
        }
    }

    /// Register a new worker by claiming the first IDLE slot.
    ///
    /// Scans `worker_status[0..64]` and CAS-es the first IDLE entry to
    /// RUNNING. Returns the slot index (0..63) or `None` if all 64 slots
    /// are non-IDLE. Bumps `generation` on success. The CAS makes
    /// concurrent registrations from sibling worker processes safe.
    pub fn register_worker(&self) -> Option<u32> {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        for i in 0..MAX_WORKERS {
            match state.worker_status[i].compare_exchange(
                STATUS_IDLE,
                STATUS_RUNNING,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => {
                    state.generation.fetch_add(1, Ordering::Relaxed);
                    return Some(i as u32);
                }
                Err(_) => continue,
            }
        }
        None
    }

    /// Mark worker `worker_id` as completed (status → DONE).
    ///
    /// Stores `STATUS_DONE` into `worker_status[worker_id]` and bumps
    /// `generation`. No-op if `worker_id` is out of range (>= 64) —
    /// the caller's bookkeeping stays consistent without panicking.
    pub fn mark_done(&self, worker_id: u32) {
        if worker_id as usize >= MAX_WORKERS {
            return;
        }
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        state.worker_status[worker_id as usize].store(STATUS_DONE, Ordering::Relaxed);
        state.generation.fetch_add(1, Ordering::Relaxed);
    }

    /// Update the core count held by worker `worker_id`.
    ///
    /// Stores `cores` into `worker_cores[worker_id]` and bumps
    /// `generation`. No-op if `worker_id` is out of range (>= 64).
    pub fn set_worker_cores(&self, worker_id: u32, cores: u32) {
        if worker_id as usize >= MAX_WORKERS {
            return;
        }
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        state.worker_cores[worker_id as usize].store(cores, Ordering::Relaxed);
        state.generation.fetch_add(1, Ordering::Relaxed);
    }

    /// Returns true if at least one core is available in the pool.
    pub fn has_available(&self) -> bool {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        state.available_cores.load(Ordering::Relaxed) > 0
    }

    /// Scheduler updates the current total memory usage (in MB).
    ///
    /// Workers read this via [`SchedShm::can_grab`] to decide whether
    /// they may grab more cores. Bumps `generation`.
    pub fn update_mem_usage(&self, total_mb: u32) {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        state.current_mem_mb.store(total_mb, Ordering::Relaxed);
        state.generation.fetch_add(1, Ordering::Relaxed);
    }

    /// Returns true if memory is under the limit AND cores are available.
    ///
    /// Workers call this before attempting to grab more cores: if memory
    /// pressure is too high or the pool is empty, they keep their current
    /// allocation rather than risk an OOM cascade.
    pub fn can_grab(&self) -> bool {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        let cur = state.current_mem_mb.load(Ordering::Relaxed);
        let lim = state.mem_limit_mb.load(Ordering::Relaxed);
        let avail = state.available_cores.load(Ordering::Relaxed);
        cur <= lim && avail > 0
    }

    /// Returns the poll interval in milliseconds (50ms aggressive, 100ms normal).
    ///
    /// The scheduler uses this to choose its `thread::sleep` duration
    /// when polling for worker completion or core availability.
    pub fn poll_interval_ms(&self) -> u64 {
        // SAFETY: self.ptr is valid for the lifetime of self.
        let state = unsafe { &*self.ptr };
        if state.aggressive.load(Ordering::Relaxed) == 1 {
            POLL_AGGRESSIVE_MS
        } else {
            POLL_NORMAL_MS
        }
    }

    /// Returns the filesystem path backing this shared-memory segment.
    ///
    /// The scheduler uses this to pass `CODESCOPE_SCHED_SHM=<path>` to
    /// worker subprocesses so they can attach via [`SchedShm::open`].
    pub fn path(&self) -> &str {
        &self.path
    }
}

impl Drop for SchedShm {
    fn drop(&mut self) {
        let size = std::mem::size_of::<SchedState>();
        if !self.ptr.is_null() {
            // SAFETY: self.ptr was returned by mmap with `size` bytes and
            // has not been unmapped yet (Drop runs once per instance).
            unsafe {
                munmap(self.ptr as *mut c_void, size);
            }
        }
        if self.fd >= 0 {
            // SAFETY: self.fd is a valid open descriptor (or already closed,
            // in which case close returns EBADF — harmless and ignored).
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

// SAFETY: SchedShm is Send because:
// - `ptr` points to mmap'd memory (MAP_SHARED) that is process-global;
//   moving the Rust handle across threads does not affect the underlying
//   memory or its visibility to other processes.
// - `fd` (RawFd = i32), `path` (String), and `is_owner` (bool) are all
//   Send by default.
// Moving SchedShm to another thread grants no exclusive access to the
// shared state — the thread still observes the same atomic-updated
// SchedState that all processes/threads see.
unsafe impl Send for SchedShm {}

// SAFETY: SchedShm is Sync because:
// - All mutable operations on the mmap'd SchedState go through
//   `AtomicU32` (`available_cores`, `generation`, `worker_status`,
//   `worker_cores`, `current_mem_mb`, `aggressive`, `active_workers`,
//   `mem_limit_mb`, `total_cores`). `AtomicU32` is `Sync`, so multiple
//   threads calling `claim_cores`/`release_cores`/`register_worker`/
//   `mark_done`/etc. concurrently on the same `&SchedShm` is safe —
//   the atomics serialise the actual state changes.
// - The non-atomic fields (`magic`, `version`, `worker_count`,
//   `reserved`) are written once in `create()` and treated as
//   read-only afterwards. `create()` completes before the
//   `SchedShm` is shared with worker threads (caller's responsibility),
//   so there are no concurrent reads during the writes.
// - `fd` and `is_owner` are only accessed in `Drop`, which takes
//   `&mut self` (exclusive access) — no concurrent access is possible.
unsafe impl Sync for SchedShm {}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicU64;

    /// Monotonic counter appended to test paths so parallel test
    /// processes never collide on the same /tmp file.
    static TEST_COUNTER: AtomicU64 = AtomicU64::new(0);

    /// Build a unique /tmp path for one test invocation.
    fn unique_path() -> String {
        let pid = std::process::id();
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0);
        let counter = TEST_COUNTER.fetch_add(1, Ordering::Relaxed);
        format!(
            "/tmp/codescope_sched_test_{}_{}_{}.shm",
            pid, nanos, counter
        )
    }

    /// Read `available_cores` directly from the shared state. Used by
    /// tests to assert post-condition values that aren't exposed via
    /// a public getter.
    fn available(s: &SchedShm) -> u32 {
        // SAFETY: s.ptr is valid for the lifetime of SchedShm; reading
        // an AtomicU32 through a shared reference is always safe.
        let state = unsafe { &*s.ptr };
        state.available_cores.load(Ordering::Relaxed)
    }

    /// Read `worker_status[id]` directly from the shared state.
    fn status(s: &SchedShm, id: u32) -> u32 {
        // SAFETY: s.ptr is valid for the lifetime of SchedShm; reading
        // an AtomicU32 through a shared reference is always safe.
        let state = unsafe { &*s.ptr };
        state.worker_status[id as usize].load(Ordering::Relaxed)
    }

    #[test]
    fn test_create_open_claim_release() {
        let path = unique_path();
        let sched = SchedShm::create(&path, 8, 4096).expect("create");
        let worker = SchedShm::open(&path).expect("open");

        // 8 cores available initially; both scheduler and worker see
        // the same value because mmap is MAP_SHARED.
        assert_eq!(available(&worker), 8);
        assert_eq!(available(&sched), 8);

        // claim 5 → 5 taken, 3 left.
        let got = worker.claim_cores(5);
        assert_eq!(got, 5);
        assert_eq!(available(&worker), 3);

        // claim 10 → only 3 left to claim (clamped at pool size).
        let got = worker.claim_cores(10);
        assert_eq!(got, 3);
        assert_eq!(available(&worker), 0);

        // claim 1 → pool empty, returns 0.
        let got = worker.claim_cores(1);
        assert_eq!(got, 0);
        assert_eq!(available(&worker), 0);

        // release 3 → pool has 3 again.
        worker.release_cores(3);
        assert_eq!(available(&worker), 3);

        // claim 1 → ok, 2 left.
        let got = worker.claim_cores(1);
        assert_eq!(got, 1);
        assert_eq!(available(&worker), 2);

        // Scheduler process sees the same final state as the worker.
        assert_eq!(available(&sched), 2);

        // claim_cores(0) is a no-op short-circuit, returns 0.
        assert_eq!(worker.claim_cores(0), 0);
        // release_cores(0) is a no-op short-circuit.
        worker.release_cores(0);
        assert_eq!(available(&worker), 2);
    }

    #[test]
    fn test_worker_register_mark_done() {
        let path = unique_path();
        let sched = SchedShm::create(&path, 8, 4096).expect("create");

        // First two registrations grab slots 0 and 1.
        let id0 = sched.register_worker().expect("register 0");
        assert_eq!(id0, 0);
        assert_eq!(status(&sched, 0), STATUS_RUNNING);

        let id1 = sched.register_worker().expect("register 1");
        assert_eq!(id1, 1);
        assert_eq!(status(&sched, 1), STATUS_RUNNING);

        // mark_done(0) sets worker_status[0] = DONE.
        sched.mark_done(0);
        assert_eq!(status(&sched, 0), STATUS_DONE);
        assert_eq!(status(&sched, 1), STATUS_RUNNING);

        // Slot 0 is now DONE (not IDLE), so the next register skips it
        // and grabs slot 2. This verifies the CAS scan only claims
        // IDLE slots — DONE slots stay DONE until the scheduler resets.
        let id2 = sched.register_worker().expect("register 2");
        assert_eq!(id2, 2);

        // mark_done on an out-of-range id is a silent no-op (no panic).
        sched.mark_done(999);
    }

    #[test]
    fn test_mem_limit() {
        let path = unique_path();
        let sched = SchedShm::create(&path, 8, 4096).expect("create");

        // Initially: mem=0, limit=4096, avail=8 → can grab.
        assert!(sched.can_grab());

        // Over limit: current_mem_mb (5000) > mem_limit_mb (4096).
        sched.update_mem_usage(5000);
        // SAFETY: sched.ptr is valid for the lifetime of sched; reading
        // AtomicU32 fields through a shared reference is always safe.
        let state = unsafe { &*sched.ptr };
        assert_eq!(state.current_mem_mb.load(Ordering::Relaxed), 5000);
        assert_eq!(state.mem_limit_mb.load(Ordering::Relaxed), 4096);
        assert!(!sched.can_grab()); // memory over limit

        // Even with cores available, mem over limit blocks grabbing.
        assert!(sched.has_available());

        // Drop memory back under limit → can grab again.
        sched.update_mem_usage(1000);
        assert!(sched.can_grab());

        // poll_interval_ms defaults to normal (100ms) since aggressive=0.
        assert_eq!(sched.poll_interval_ms(), POLL_NORMAL_MS);
    }

    #[test]
    fn test_open_rejects_wrong_magic() {
        // Create a file with garbage bytes (no MAGIC header) and verify
        // open() rejects it instead of returning a bogus SchedShm.
        let path = unique_path();
        std::fs::write(&path, b"not a sched state").expect("write");
        let result = SchedShm::open(&path);
        assert!(result.is_err(), "open should fail on bad magic");
        let err = result.unwrap_err();
        assert!(
            err.contains("magic mismatch"),
            "error should mention magic mismatch: {}",
            err
        );
        assert!(
            err.contains("module=scheduler"),
            "error should be tagged with module: {}",
            err
        );
        // Clean up the garbage file (no Drop ran since open failed).
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn test_set_worker_cores_and_path_accessor() {
        let path = unique_path();
        let sched = SchedShm::create(&path, 4, 2048).expect("create");

        // path() returns the path passed to create().
        assert_eq!(sched.path(), path);

        let id = sched.register_worker().expect("register");
        sched.set_worker_cores(id, 3);
        // SAFETY: sched.ptr is valid for the lifetime of sched; reading
        // an AtomicU32 through a shared reference is always safe.
        let state = unsafe { &*sched.ptr };
        assert_eq!(state.worker_cores[id as usize].load(Ordering::Relaxed), 3);

        // Out-of-range worker_id is a silent no-op.
        sched.set_worker_cores(999, 7);
    }
}
