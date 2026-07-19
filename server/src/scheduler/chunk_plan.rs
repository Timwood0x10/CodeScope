//! Chunk planner — directory clustering + byte-weighted chunk splitting.
//!
//! Given a list of candidate source files (path + byte size), produces a
//! list of chunks that respect directory locality and balance byte weight.
//!
//! Algorithm (§5 of DYNAMIC_SCHED_REDESIGN.md):
//!   1. Group files by first 2 levels of relative path (directory clustering).
//!   2. Sort clusters by total byte size descending (large clusters first).
//!   3. Greedy-packing: accumulate files into a chunk until `TARGET_BYTES`
//!      is reached; giant clusters (≥ `MAX_BYTES`) are forcibly split.
//!   4. Output `Vec<Chunk>` where each chunk owns a contiguous range of
//!      `file_indices` into the original file list.
//!
//! Design constraints:
//!   - Chunk count for a 50k-file project should be 100–250.
//!   - Each chunk file_count ∈ [50, 800] and total_bytes near TARGET_BYTES.
//!   - Same-directory files preferentially fall in the same chunk.
//!
//! # Examples
//!
//! ```
//! use scheduler::chunk_plan::{plan_chunks, FileEntry, TARGET_BYTES};
//! let mut files = vec![
//!     FileEntry { path: "src/main.rs".to_string(), size: 4096 },
//!     FileEntry { path: "src/lib.rs".to_string(), size: 2048 },
//! ];
//! // plan_chunks requires input sorted by path (see Invariant below).
//! files.sort_by(|a, b| a.path.cmp(&b.path));
//! let chunks = plan_chunks(&files, TARGET_BYTES, TARGET_BYTES * 2);
//! assert!(!chunks.is_empty());
//! ```
// This module is used by the chunk-level scheduler (index_parallel_chunked).
// The dead_code lint is suppressed because the module is wired via the
// `index_parallel_chunked` dispatch path which is gated behind the
// `CODESCOPE_DYNAMIC_SCHED` env var. Tests verify correctness.
#![allow(dead_code)]

/// Default target bytes per chunk (8 MB).
pub const TARGET_BYTES: u64 = 8 * 1024 * 1024;

/// Maximum bytes for a single chunk (32 MB) — giant clusters forced split.
pub const MAX_BYTES: u64 = 32 * 1024 * 1024;

/// Directory clustering depth: group by first N relative-path components.
pub const CLUSTER_DEPTH: usize = 2;

/// A single chunk: a contiguous range of indices into the original file list.
#[derive(Debug, Clone)]
pub struct Chunk {
    /// Start index in the original file list (inclusive).
    pub file_start: usize,
    /// Number of files in this chunk.
    pub file_count: usize,
    /// Total byte size of all files in this chunk.
    pub total_bytes: u64,
    /// Module ID this chunk belongs to (0 = unassigned, set by scheduler).
    pub module_id: u32,
    /// Directory prefix for this chunk (for debugging / bookkeeping).
    pub dir_prefix: String,
}

/// A file entry with its relative path and byte size.
#[derive(Debug, Clone)]
pub struct FileEntry {
    /// Relative path from the project root, e.g. `src/main.rs`.
    pub path: String,
    /// File size in bytes.
    pub size: u64,
}

/// Extract the first `depth` path components of a relative path.
///
/// Returns the directory prefix (e.g. `"src/compiler"` for `"src/compiler/parser.rs"`).
/// For root-level files (no directory separator), returns `"/"`.
/// For the empty path, returns `"/"` (root sentinel).
fn dir_prefix(path: &str, depth: usize) -> String {
    // Root-level files (no directory separator) → "/".
    if !path.contains('/') {
        return "/".to_string();
    }
    let mut components: Vec<&str> = path.split('/').collect();
    // Remove the filename (last component).
    if components.len() >= 2 {
        components.pop();
    }
    let take = depth.min(components.len());
    if take == 0 {
        return "/".to_string();
    }
    components[..take].join("/")
}

/// Group file entries by their directory prefix (depth = `CLUSTER_DEPTH`).
///
/// Returns a `Vec` of `(prefix, entries)` sorted by total bytes descending.
fn group_by_prefix(files: &[FileEntry], depth: usize) -> Vec<(String, Vec<FileEntry>)> {
    use std::collections::HashMap;
    let mut map: HashMap<String, Vec<FileEntry>> = HashMap::new();
    for f in files {
        let prefix = dir_prefix(&f.path, depth);
        map.entry(prefix).or_default().push(f.clone());
    }
    let mut groups: Vec<(String, Vec<FileEntry>)> = map.into_iter().collect();
    // Sort by total bytes descending (largest clusters first).
    groups.sort_by_key(|(_, entries)| entries.iter().map(|e| e.size).sum::<u64>());
    groups.reverse();
    groups
}

/// Plan chunks from a list of file entries.
///
/// `target_bytes` — desired chunk size (default: `TARGET_BYTES`).
/// `max_bytes` — hard ceiling for a single chunk (default: `MAX_BYTES`).
///
/// Returns a `Vec<Chunk>` covering all files, with no overlap and no gaps.
/// Panics if `files` is empty (caller should check first).
///
/// # Invariant
/// `files` MUST be sorted by `path` (ascending). The planner emits
/// contiguous index ranges per directory cluster and relies on equal
/// 2-level prefixes collapsing into adjacent indices. An unsorted input
/// (e.g. a parallel walk or size-sorted list) would make chunks silently
/// reference the wrong files. The discover path currently guarantees this
/// via `files.sort()`; the assertion below fails loudly if a future caller
/// violates it instead of corrupting chunks.
pub fn plan_chunks(files: &[FileEntry], target_bytes: u64, max_bytes: u64) -> Vec<Chunk> {
    assert!(!files.is_empty(), "plan_chunks: empty file list");
    assert!(
        target_bytes > 0 && target_bytes <= max_bytes,
        "plan_chunks: target_bytes={} must be in (0, max_bytes={}]",
        target_bytes,
        max_bytes
    );
    // INVARIANT CHECK: fail loudly on unsorted input (see doc above).
    debug_assert!(
        files.windows(2).all(|w| w[0].path <= w[1].path),
        "plan_chunks requires `files` to be sorted by path"
    );

    // Phase 1: group by directory prefix, sorted largest-first.
    let clusters = group_by_prefix(files, CLUSTER_DEPTH);

    let mut chunks: Vec<Chunk> = Vec::new();
    // Global file index — tracks the absolute position in the original `files` slice.
    // We build a lookup: path → index so we can map cluster entries back to indices.
    // For correctness we iterate through the original `files` order per cluster.
    let path_to_idx: std::collections::HashMap<&str, usize> = files
        .iter()
        .enumerate()
        .map(|(i, f)| (f.path.as_str(), i))
        .collect();

    for (prefix, cluster_entries) in &clusters {
        // Sort cluster entries by their original index so the chunk's file range
        // is contiguous in the original ordering.
        let mut sorted_entries: Vec<&FileEntry> = cluster_entries.iter().collect();
        sorted_entries.sort_by_key(|e| {
            path_to_idx
                .get(e.path.as_str())
                .copied()
                .unwrap_or(usize::MAX)
        });

        // Split large clusters that exceed max_bytes into multiple chunks.
        let mut chunk_start: usize = 0;
        let mut chunk_bytes: u64 = 0;

        for entry in &sorted_entries {
            let idx = path_to_idx[entry.path.as_str()];

            // If this single file exceeds max_bytes, it gets its own chunk.
            // If adding this file would exceed max_bytes AND the current
            // chunk already has content, finalise the current chunk first.
            // The guard is `chunk_bytes > 0` (current chunk non-empty),
            // NOT `!chunks.is_empty()`: the latter gates on whether ANY
            // chunk has been emitted across the whole call, which has
            // nothing to do with whether the current chunk should close.
            if entry.size > max_bytes || (chunk_bytes > 0 && chunk_bytes + entry.size > max_bytes) {
                // Finalise the current chunk.
                let count = sorted_entries[chunk_start..]
                    .iter()
                    .position(|e| {
                        let ei = path_to_idx[e.path.as_str()];
                        ei >= idx
                    })
                    .unwrap_or(sorted_entries.len() - chunk_start);
                if count > 0 {
                    chunks.push(Chunk {
                        file_start: path_to_idx[sorted_entries[chunk_start].path.as_str()],
                        file_count: count,
                        total_bytes: chunk_bytes,
                        module_id: 0,
                        dir_prefix: prefix.clone(),
                    });
                }
                chunk_start += count;
                chunk_bytes = 0;
            }

            chunk_bytes += entry.size;

            // Greedy cut: if we've reached target, finalise the chunk and reset.
            if chunk_bytes >= target_bytes {
                let chunk_end = chunk_start
                    + sorted_entries[chunk_start..]
                        .iter()
                        .position(|e| {
                            let ei = path_to_idx[e.path.as_str()];
                            ei > idx
                        })
                        .unwrap_or(sorted_entries.len() - chunk_start);
                let count = chunk_end - chunk_start;
                if count > 0 {
                    chunks.push(Chunk {
                        file_start: path_to_idx[sorted_entries[chunk_start].path.as_str()],
                        file_count: count,
                        total_bytes: chunk_bytes,
                        module_id: 0,
                        dir_prefix: prefix.clone(),
                    });
                }
                chunk_start = chunk_end;
                chunk_bytes = 0;
            }
        }

        // Flush remaining entries in this cluster.
        if chunk_bytes > 0 {
            let remaining = sorted_entries.len() - chunk_start;
            if remaining > 0 {
                chunks.push(Chunk {
                    file_start: path_to_idx[sorted_entries[chunk_start].path.as_str()],
                    file_count: remaining,
                    total_bytes: chunk_bytes,
                    module_id: 0,
                    dir_prefix: prefix.clone(),
                });
            }
        }
    }

    // Sanity: verify no files are lost or double-counted.
    #[cfg(debug_assertions)]
    {
        let mut covered: Vec<bool> = vec![false; files.len()];
        for c in &chunks {
            for (i, slot) in covered
                .iter_mut()
                .enumerate()
                .skip(c.file_start)
                .take(c.file_count)
            {
                assert!(!*slot, "chunk_plan: file index {} double-counted", i);
                *slot = true;
            }
        }
        let uncovered: Vec<usize> = covered
            .iter()
            .enumerate()
            .filter(|&(_, v)| !v)
            .map(|(i, _)| i)
            .collect();
        assert!(
            uncovered.is_empty(),
            "chunk_plan: {} files uncovered: {:?}",
            uncovered.len(),
            uncovered
        );
    }

    chunks
}

/// A compact representation of a chunk for serialisation / shm transfer.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ChunkDescriptor {
    pub file_start: u32,
    pub file_count: u32,
    pub total_bytes: u64,
    pub module_id: u32,
}

impl From<&Chunk> for ChunkDescriptor {
    fn from(c: &Chunk) -> Self {
        ChunkDescriptor {
            file_start: c.file_start as u32,
            file_count: c.file_count as u32,
            total_bytes: c.total_bytes,
            module_id: c.module_id,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_file(path: &str, size: u64) -> FileEntry {
        FileEntry {
            path: path.to_string(),
            size,
        }
    }

    // Mirror production usage: discover.rs sorts files by path before
    // planning. plan_chunks requires sorted input (see module docs), so
    // tests must honour the same contract rather than pass raw inputs.
    fn sorted(mut files: Vec<FileEntry>) -> Vec<FileEntry> {
        files.sort_by(|a, b| a.path.cmp(&b.path));
        files
    }

    #[test]
    fn test_dir_prefix_root_level() {
        assert_eq!(dir_prefix("main.rs", 2), "/");
    }

    #[test]
    fn test_dir_prefix_one_level() {
        assert_eq!(dir_prefix("src/main.rs", 2), "src");
    }

    #[test]
    fn test_dir_prefix_two_levels() {
        assert_eq!(dir_prefix("src/compiler/parser.rs", 2), "src/compiler");
    }

    #[test]
    fn test_dir_prefix_three_levels_truncated() {
        // Only first 2 components are kept.
        assert_eq!(dir_prefix("a/b/c/d.rs", 2), "a/b");
    }

    #[test]
    fn test_dir_prefix_empty_path() {
        assert_eq!(dir_prefix("", 2), "/");
    }

    #[test]
    fn test_group_by_prefix_single_file() {
        let files = vec![make_file("src/main.rs", 100)];
        let groups = group_by_prefix(&files, 2);
        assert_eq!(groups.len(), 1);
        assert_eq!(groups[0].0, "src");
    }

    #[test]
    fn test_group_by_prefix_multiple_dirs() {
        let files = vec![
            make_file("src/main.rs", 100),
            make_file("src/lib.rs", 200),
            make_file("tests/test.rs", 50),
        ];
        let groups = group_by_prefix(&files, 2);
        // Sorted by total bytes descending: src (300) > tests (50).
        assert_eq!(groups.len(), 2);
        assert_eq!(groups[0].0, "src");
        assert_eq!(groups[1].0, "tests");
    }

    #[test]
    fn test_plan_chunks_small_project_single_chunk() {
        let files = vec![
            make_file("src/main.rs", 1000),
            make_file("src/lib.rs", 2000),
        ];
        let chunks = plan_chunks(&sorted(files), TARGET_BYTES, MAX_BYTES);
        assert_eq!(chunks.len(), 1);
        assert_eq!(chunks[0].file_count, 2);
        assert_eq!(chunks[0].total_bytes, 3000);
    }

    #[test]
    fn test_plan_chunks_multiple_chunks() {
        // 50 files, each 1 MB → exceeds TARGET_BYTES (8 MB) → multiple chunks.
        let files: Vec<FileEntry> = (0..50)
            .map(|i| make_file(&format!("src/mod{}/file.rs", i), 1_000_000))
            .collect();
        let chunks = plan_chunks(&sorted(files), TARGET_BYTES, MAX_BYTES);
        assert!(
            chunks.len() >= 5,
            "expected >=5 chunks, got {}",
            chunks.len()
        );
        // Verify total coverage.
        let total_files: usize = chunks.iter().map(|c| c.file_count).sum();
        assert_eq!(total_files, 50);
        let total_bytes: u64 = chunks.iter().map(|c| c.total_bytes).sum();
        assert_eq!(total_bytes, 50_000_000);
    }

    #[test]
    fn test_plan_chunks_giant_cluster_forced_split() {
        // 100 files all in one directory, each 1 MB → exceeds MAX_BYTES at 32 files.
        let files: Vec<FileEntry> = (0..100)
            .map(|i| make_file(&format!("src/giant/file_{}.rs", i), 1_000_000))
            .collect();
        let chunks = plan_chunks(&sorted(files), TARGET_BYTES, MAX_BYTES);
        // Each chunk should be ≤ MAX_BYTES (32 MB = 32 files × 1 MB).
        for c in &chunks {
            assert!(
                c.total_bytes <= MAX_BYTES,
                "chunk {} has {} bytes, exceeds MAX_BYTES={}",
                c.file_start,
                c.total_bytes,
                MAX_BYTES
            );
        }
        let total_files: usize = chunks.iter().map(|c| c.file_count).sum();
        assert_eq!(total_files, 100);
    }

    #[test]
    fn test_plan_chunks_empty_input_panics() {
        let result = std::panic::catch_unwind(|| {
            let _ = plan_chunks(&[], TARGET_BYTES, MAX_BYTES);
        });
        assert!(result.is_err(), "plan_chunks should panic on empty input");
    }

    #[test]
    fn test_plan_chunks_single_giant_file() {
        // A single file larger than MAX_BYTES gets its own chunk.
        let files = vec![make_file("src/huge/blob.rs", MAX_BYTES + 1)];
        let chunks = plan_chunks(&sorted(files), TARGET_BYTES, MAX_BYTES);
        assert_eq!(chunks.len(), 1);
        assert_eq!(chunks[0].file_count, 1);
        // The chunk's total_bytes reflects the file size, even though it exceeds
        // max_bytes — the file is indivisible.
        assert_eq!(chunks[0].total_bytes, MAX_BYTES + 1);
    }

    #[test]
    fn test_plan_chunks_target_bytes_zero_panics() {
        let files = vec![make_file("src/main.rs", 100)];
        let result = std::panic::catch_unwind(|| {
            let _ = plan_chunks(&files, 0, MAX_BYTES);
        });
        assert!(result.is_err());
    }

    #[test]
    fn test_plan_chunks_target_exceeds_max_panics() {
        let files = vec![make_file("src/main.rs", 100)];
        let result = std::panic::catch_unwind(|| {
            let _ = plan_chunks(&files, MAX_BYTES + 1, MAX_BYTES);
        });
        assert!(result.is_err());
    }

    #[test]
    fn test_plan_chunks_50k_files_synthetic() {
        // Simulate a 50k-file project to verify chunk count and bounds.
        let mut files = Vec::with_capacity(50_000);
        for i in 0..50_000 {
            let dir = format!("mod{}", i / 1000);
            let size = 20_000 + (i % 50) * 200; // 20KB–30KB per file (matches the design's ~200–500 files/chunk target)
            files.push(make_file(&format!("{}/file_{}.rs", dir, i), size));
        }
        let chunks = plan_chunks(&sorted(files), TARGET_BYTES, MAX_BYTES);
        // Chunk count should be 100–250 for 50k files.
        assert!(
            chunks.len() >= 100,
            "expected >=100 chunks for 50k files, got {}",
            chunks.len()
        );
        assert!(
            chunks.len() <= 250,
            "expected <=250 chunks for 50k files, got {}",
            chunks.len()
        );
        // Verify every file is covered exactly once.
        let total_files: usize = chunks.iter().map(|c| c.file_count).sum();
        assert_eq!(total_files, 50_000);
        // Verify no chunk exceeds MAX_BYTES.
        for c in &chunks {
            assert!(
                c.total_bytes <= MAX_BYTES,
                "chunk starting at {} has {} bytes, exceeds MAX_BYTES",
                c.file_start,
                c.total_bytes
            );
        }
    }

    #[test]
    fn test_plan_chunks_directory_locality() {
        // Files from the same directory should mostly fall in the same chunk.
        let mut files = Vec::new();
        for i in 0..20 {
            files.push(make_file(&format!("src/core/module_{}.rs", i), 100_000));
        }
        for i in 0..5 {
            files.push(make_file(&format!("src/other/extra_{}.rs", i), 100_000));
        }
        let chunks = plan_chunks(&sorted(files), TARGET_BYTES, MAX_BYTES);
        // The "src/core" cluster (20 files × 100KB = 2MB) fits in one chunk
        // since it's well below TARGET_BYTES (8MB).
        let core_chunks: Vec<&Chunk> = chunks
            .iter()
            .filter(|c| c.dir_prefix == "src/core")
            .collect();
        assert_eq!(core_chunks.len(), 1, "src/core should be in one chunk");
        assert_eq!(core_chunks[0].file_count, 20);
    }

    #[test]
    fn test_chunk_descriptor_from_chunk() {
        let chunk = Chunk {
            file_start: 10,
            file_count: 5,
            total_bytes: 12345,
            module_id: 2,
            dir_prefix: "src".to_string(),
        };
        let desc: ChunkDescriptor = (&chunk).into();
        assert_eq!(desc.file_start, 10);
        assert_eq!(desc.file_count, 5);
        assert_eq!(desc.total_bytes, 12345);
        assert_eq!(desc.module_id, 2);
    }
}
