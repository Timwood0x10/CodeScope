//! Cross-platform, file-backed shared memory.
//!
//! Both schedulers need a `#[repr(C)]` state block that the scheduler process
//! and its worker subprocesses all observe: the dynamic scheduler's
//! [`crate::scheduler::shm::SchedState`] and the chunked scheduler's
//! [`crate::scheduler::chunk_queue::ChunkQueueState`]. Both used to open-code
//! the same POSIX sequence (`open` + `ftruncate` + `mmap(MAP_SHARED)`) and so
//! both failed to compile on Windows, where the `libc` crate exports no mapping
//! API at all. This module owns that sequence once, behind two backends:
//!
//! * **POSIX** — `open`/`ftruncate`/`mmap(MAP_SHARED)`, torn down with
//!   `munmap`/`close`/`unlink`.
//! * **Windows** — a file-backed section object: `CreateFileW` (through
//!   [`std::fs`]) + `SetEndOfFile` + `CreateFileMappingW` + `MapViewOfFile`,
//!   torn down with `UnmapViewOfFile`/`CloseHandle`/`DeleteFileW`.
//!
//! The contract is identical on both platforms:
//!
//! * [`MappedFile::create`] sizes the file to exactly `len` and returns a
//!   writable view whose bytes are **all zero**.
//! * [`MappedFile::open`] attaches to a file the owner already created; the
//!   caller passes the same `len` it used for `create`.
//! * `Drop` unmaps and closes, and the owner additionally removes the file.
//!
//! Callers must keep the returned handle alive for as long as they read the
//! mapped memory, and must ensure every process that maps the same file agrees
//! on the layout (`#[repr(C)]` fields, atomics for anything mutated after
//! `create`).

/// Method tag appended to error strings, e.g. `SchedShm::create`, so messages
/// keep the `[module=scheduler, method=…]` shape callers' tests and log greps
/// expect. Kept as a parameter rather than a constant because the same mapping
/// code backs two differently named queues.
pub struct MappedFile {
    path: String,
    ptr: *mut u8,
    len: usize,
    owner: bool,
    handle: imp::Handle,
}

impl MappedFile {
    /// Create (or re-truncate) `path` to exactly `len` bytes and map it
    /// writable. The mapped bytes are guaranteed to be zero.
    pub fn create(path: &str, len: usize, method: &str) -> Result<Self, String> {
        let (ptr, handle) = imp::map(path, len, true, method)?;
        Ok(Self {
            path: path.to_string(),
            ptr,
            len,
            owner: true,
            handle,
        })
    }

    /// Attach to a file previously created with [`MappedFile::create`].
    pub fn open(path: &str, len: usize, method: &str) -> Result<Self, String> {
        let (ptr, handle) = imp::map(path, len, false, method)?;
        Ok(Self {
            path: path.to_string(),
            ptr,
            len,
            owner: false,
            handle,
        })
    }

    /// Base address of the mapping. Cast to the caller's `#[repr(C)]` type.
    pub fn ptr(&self) -> *mut u8 {
        self.ptr
    }

    /// Path backing the mapping.
    pub fn path(&self) -> &str {
        &self.path
    }

    /// True when this handle created the file (and so removes it on drop).
    pub fn is_owner(&self) -> bool {
        self.owner
    }
}

impl Drop for MappedFile {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            // SAFETY: ptr/len are exactly what this mapping returned, and Drop
            // runs once, so the view is unmapped exactly once.
            unsafe { imp::unmap(self.ptr, self.len) };
            self.ptr = std::ptr::null_mut();
        }
        // SAFETY: handle is owned by this MappedFile and released once; on the
        // Unix backend this closes the descriptor, on Windows the section.
        unsafe { imp::close(&mut self.handle) };
        if self.owner {
            imp::remove(&self.path);
        }
    }
}

// SAFETY: `MappedFile` owns a process-wide mapping of a shared file. Moving it
// between threads grants no exclusive access — every reader observes the same
// bytes, and all mutable fields in the mapped structs are atomics. The raw
// pointer and the backend handle are only ever passed to the OS calls that
// created them, all of which are thread-safe.
unsafe impl Send for MappedFile {}
// SAFETY: see above — reads through the raw pointer are of atomic fields, and
// the handle is touched only by `Drop`, which takes `&mut self`.
unsafe impl Sync for MappedFile {}

#[cfg(unix)]
mod imp {
    use std::ffi::{CString, c_void};
    use std::io;
    use std::os::unix::io::RawFd;

    // NOTE: libc's `close` is intentionally not imported — this module defines
    // its own `close` for the mapping handle, so the libc one is called with an
    // explicit path below.
    use libc::{
        MAP_SHARED, O_CREAT, O_RDWR, PROT_READ, PROT_WRITE, S_IRUSR, S_IWUSR, ftruncate, mmap,
        munmap, open, unlink,
    };

    /// Open descriptor of the backing file. `-1` means "already closed", which
    /// makes the release path idempotent.
    pub(super) struct Handle(RawFd);

    pub(super) fn map(
        path: &str,
        len: usize,
        create: bool,
        method: &str,
    ) -> Result<(*mut u8, Handle), String> {
        let c_path = CString::new(path).map_err(|e| {
            format!("path contains NUL byte: {e} [module=scheduler, method={method}]")
        })?;

        // SAFETY: c_path is a valid NUL-terminated CString. O_CREAT|O_RDWR
        // creates the file when the caller owns it; mode 0600 restricts it to
        // the owner. The mode is cast to c_int because open(2) is variadic and
        // macOS types S_IRUSR as u16.
        let fd = unsafe {
            if create {
                open(
                    c_path.as_ptr(),
                    O_CREAT | O_RDWR,
                    (S_IRUSR | S_IWUSR) as libc::c_int,
                )
            } else {
                open(c_path.as_ptr(), O_RDWR)
            }
        };
        if fd < 0 {
            return Err(format!(
                "open failed: {} [module=scheduler, method={method}, path={path}]",
                io::Error::last_os_error(),
            ));
        }

        if create {
            // ftruncate both sizes the file and — when extending — zero-fills
            // the new bytes, which is what lets the callers treat a fresh
            // mapping as all-zero. The struct sizes here are a few hundred
            // bytes to ~16 KB, well inside off_t range.
            // SAFETY: fd is a valid descriptor owned by this frame.
            if unsafe { ftruncate(fd, len as i64) } != 0 {
                let err = io::Error::last_os_error();
                // SAFETY: fd is valid and owned here; release it on error.
                unsafe { libc::close(fd) };
                return Err(format!(
                    "ftruncate failed: {err} [module=scheduler, method={method}]"
                ));
            }
        }

        // SAFETY: fd is valid and the file is at least `len` bytes (just
        // truncated by us, or created by the owner). MAP_SHARED makes writes
        // visible to every other process mapping the same file, which is the
        // whole point: PROT_READ|PROT_WRITE allows the atomic updates.
        let ptr = unsafe {
            mmap(
                std::ptr::null_mut(),
                len,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                0,
            )
        };
        if ptr == libc::MAP_FAILED {
            let err = io::Error::last_os_error();
            // SAFETY: fd is valid and owned here; release it on error.
            unsafe { libc::close(fd) };
            return Err(format!(
                "mmap failed: {err} [module=scheduler, method={method}]"
            ));
        }

        Ok((ptr as *mut u8, Handle(fd)))
    }

    pub(super) unsafe fn unmap(ptr: *mut u8, len: usize) {
        // SAFETY: caller guarantees ptr/len came from `mmap` above and that the
        // view is unmapped exactly once.
        unsafe { munmap(ptr as *mut c_void, len) };
    }

    pub(super) unsafe fn close(handle: &mut Handle) {
        if handle.0 >= 0 {
            // SAFETY: the descriptor is owned by the caller and closed once;
            // the -1 sentinel makes a second call a no-op.
            unsafe { libc::close(handle.0) };
            handle.0 = -1;
        }
    }

    pub(super) fn remove(path: &str) {
        if let Ok(c_path) = CString::new(path) {
            // SAFETY: c_path is a valid NUL-terminated CString. Unlinking while
            // another process still maps the file is harmless: existing
            // mappings stay valid until they are unmapped (POSIX semantics),
            // and the inode is freed with the last mapping.
            unsafe { unlink(c_path.as_ptr()) };
        }
    }
}

#[cfg(windows)]
mod imp {
    use std::fs::{File, OpenOptions};
    use std::io::Write;
    use std::os::windows::io::AsRawHandle;

    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
    use windows_sys::Win32::System::Memory::{
        CreateFileMappingW, FILE_MAP_ALL_ACCESS, MapViewOfFile, PAGE_READWRITE, UnmapViewOfFile,
    };

    /// Owns the backing file (which keeps the section object valid) plus the
    /// section handle itself. `mapping.is_null()` means "already released", so
    /// the release path is idempotent; dropping `file` closes the last handle.
    pub(super) struct Handle {
        file: Option<File>,
        mapping: HANDLE,
    }

    pub(super) fn map(
        path: &str,
        len: usize,
        create: bool,
        method: &str,
    ) -> Result<(*mut u8, Handle), String> {
        let mut options = OpenOptions::new();
        options.read(true).write(true);
        if create {
            options.create(true);
        }
        let file = options.open(path).map_err(|e| {
            format!("open failed: {e} [module=scheduler, method={method}, path={path}]")
        })?;

        if create {
            // SetEndOfFile (behind `set_len`) sizes the file but leaves the
            // bytes of an *extended* region undefined on Windows, unlike POSIX
            // ftruncate. Write the zeros explicitly so `create` offers the same
            // guarantee on both platforms: a fresh mapping reads as all-zero,
            // which is what the callers' "reserved/zeroed fields" comments
            // (and their magic/version writes) rely on.
            file.set_len(len as u64).map_err(|e| {
                format!("ftruncate failed: {e} [module=scheduler, method={method}]")
            })?;
            let zeros = vec![0u8; len];
            let mut writer = &file;
            writer.write_all(&zeros).map_err(|e| {
                format!("zero-fill failed: {e} [module=scheduler, method={method}]")
            })?;
            writer.flush().map_err(|e| {
                format!("zero-fill flush failed: {e} [module=scheduler, method={method}]")
            })?;
        }

        // The section size is passed as two u32 halves. Callers map a single
        // struct, so anything above 4 GiB means a layout mistake — refuse it
        // rather than silently mapping a truncated (and therefore
        // out-of-bounds) view.
        let size_low = u32::try_from(len).map_err(|_| {
            format!(
                "mapping too large for a section object: {len} bytes \
                 [module=scheduler, method={method}]"
            )
        })?;

        // SAFETY: the file handle is valid for as long as `file` is alive and
        // is only borrowed for the duration of the call; a null name gives the
        // section no name in the object namespace (workers reach it through the
        // shared file path, not through a named section).
        let mapping = unsafe {
            CreateFileMappingW(
                file.as_raw_handle() as HANDLE,
                std::ptr::null(),
                PAGE_READWRITE,
                0,
                size_low,
                std::ptr::null(),
            )
        };
        if mapping.is_null() {
            return Err(format!(
                "mmap failed: {} [module=scheduler, method={method}]",
                std::io::Error::last_os_error(),
            ));
        }

        // SAFETY: `mapping` is a live section handle object; FILE_MAP_ALL_ACCESS
        // matches the PAGE_READWRITE protection it was created with.
        let view = unsafe { MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, len) };
        if view.Value.is_null() {
            let err = std::io::Error::last_os_error();
            // SAFETY: mapping was created above and is owned by this frame.
            unsafe { CloseHandle(mapping) };
            return Err(format!(
                "mmap failed: {err} [module=scheduler, method={method}]"
            ));
        }

        Ok((
            view.Value as *mut u8,
            Handle {
                file: Some(file),
                mapping,
            },
        ))
    }

    pub(super) unsafe fn unmap(ptr: *mut u8, _len: usize) {
        // SAFETY: caller guarantees ptr came from `MapViewOfFile` above and that
        // the view is unmapped exactly once.
        unsafe {
            UnmapViewOfFile(
                windows_sys::Win32::System::Memory::MEMORY_MAPPED_VIEW_ADDRESS {
                    Value: ptr as *mut core::ffi::c_void,
                },
            )
        };
    }

    pub(super) unsafe fn close(handle: &mut Handle) {
        if !handle.mapping.is_null() {
            // SAFETY: the section handle is owned here and released once; the
            // null sentinel makes a second call a no-op.
            unsafe { CloseHandle(handle.mapping) };
            handle.mapping = std::ptr::null_mut();
        }
        // Closing the file handle releases the file itself. Order matters: the
        // view and the section must be gone first (Windows refuses to delete a
        // file that still has an open handle or a live section).
        handle.file = None;
    }

    pub(super) fn remove(path: &str) {
        // Best effort, and deliberately after both handles were released: if a
        // worker has not exited yet the delete fails with a sharing violation
        // and the file stays in the temp directory. That is the Windows
        // counterpart of the POSIX path's "stale segment" case (a killed run
        // leaves the file behind too), and the names are unique per PID, so the
        // next run never collides with it.
        let _ = std::fs::remove_file(path);
    }
}
