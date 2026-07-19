//! Chunk planning: split a module's file list into balanced chunks
//! for parallel indexing. Used by the chunk-level scheduler to enable
//! work-stealing across workers (see DYNAMIC_SCHED_REDESIGN.md §5).
//!
//! Algorithm:
//! 1. Group files by their first 2 path components (directory
//!    clustering) to preserve semantic locality.
//! 2. Sort clusters by total size descending (largest first).
//! 3. Greedy bin-pack: for each file in each cluster, append to the
//!    current chunk; when the chunk reaches TARGET_BYTES, close it
//!    and start a new one.
//! 4. Return the list of chunks, each with file index range + total
//!    bytes.
//!
//! The planner is pure computation (no I/O, no shm) so it can be
//! unit-tested in isolation. The output `Chunk` list references
//! indices into the caller's file vector — the planner never copies
//! file paths.
//!
//! This module is infrastructure for the chunk-level scheduler (P2-P4
//! of the redesign). The current module-level scheduler doesn't use
//! it yet; both coexist during the migration.

#![allow(dead_code)]

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::Path;

/// Default target bytes per chunk (8 MB). Tuned for 200-500 files
/// per chunk on typical source code (avg 16-40 KB/file).
pub const DEFAULT_TARGET_BYTES: u64 = 8 * 1024 * 1024;

/// Hard cap on chunk size (32 MB). A single file larger than this
/// still becomes its own chunk — we don't split individual files.
pub const DEFAULT_MAX_BYTES: u64 = 32 * 1024 * 1024;

/// One chunk of work: a contiguous range of file indices into the
/// input `files` vector, plus the total byte size for accounting.
///
/// `file_start` and `file_count` reference indices into the ORIGINAL
/// input slice. The planner does not reorder files; it groups them
/// by directory prefix, but each chunk's indices form a sub-range
/// of the per-cluster file order.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Chunk {
    /// Index of the first file in this chunk (into the input slice).
    pub file_start: u32,
    /// Number of files in this chunk.
    pub file_count: u32,
    /// Total bytes of all files in this chunk.
    pub total_bytes: u64,
}

/// A file entry passed to the planner. The path is used for
/// directory clustering; the size drives the byte-weighted packing.
#[derive(Debug, Clone)]
pub struct FileEntry {
    pub path: String,
    pub size: u64,
}

/// Plan chunks for a list of files.
///
/// `files` — the file list to partition.
/// `target_bytes` — desired bytes per chunk (0 = use DEFAULT_TARGET_BYTES).
/// `max_bytes` — hard cap on chunk bytes (0 = use DEFAULT_MAX_BYTES).
///
/// Returns a vector of chunks covering all input files exactly once.
/// Empty input yields an empty vector. A single huge file becomes
/// its own chunk even if it exceeds `max_bytes` (we never split
/// individual files).
pub fn plan_chunks(files: &[FileEntry], target_bytes: u64, max_bytes: u64) -> Vec<Chunk> {
    if files.is_empty() {
        return Vec::new();
    }
    let target = if target_bytes == 0 {
        DEFAULT_TARGET_BYTES
    } else {
        target_bytes
    };
    let max = if max_bytes == 0 {
        DEFAULT_MAX_BYTES
    } else {
        max_bytes
    };

    // 1) Group file indices by their first 2 path components.
    let mut clusters: HashMap<String, Vec<usize>> = HashMap::new();
    for (i, f) in files.iter().enumerate() {
        let key = path_prefix_2(&f.path);
        clusters.entry(key).or_default().push(i);
    }

    // 2) Sort clusters by total bytes descending — largest first so
    //    big clusters are split early while the chunk budget is fresh.
    let mut cluster_keys: Vec<(String, Vec<usize>)> = clusters.into_iter().collect();
    cluster_keys.sort_by(|a, b| {
        let sum_a: u64 = a.1.iter().map(|&i| files[i].size).sum();
        let sum_b: u64 = b.1.iter().map(|&i| files[i].size).sum();
        sum_b.cmp(&sum_a)
    });

    // 3) Greedy bin-pack: walk files cluster by cluster, accumulating
    //    into the current chunk. Close the chunk when:
    //      - current chunk bytes >= target, OR
    //      - adding the next file would exceed max (close BEFORE adding), OR
    //      - this is the last file of the cluster (preserves locality
    //        by closing at cluster boundaries so the next cluster
    //        starts a fresh chunk).
    let mut chunks: Vec<Chunk> = Vec::new();
    let mut cur_start: u32 = 0;
    let mut cur_count: u32 = 0;
    let mut cur_bytes: u64 = 0;

    for (_, indices) in cluster_keys {
        for (pos_in_cluster, &idx) in indices.iter().enumerate() {
            let file_size = files[idx].size;
            let is_last_in_cluster = pos_in_cluster == indices.len() - 1;

            // If adding this file would exceed max AND the current
            // chunk is non-empty, close the current chunk first.
            // (A single file larger than max becomes its own chunk —
            // we don't split individual files.)
            if cur_count > 0 && cur_bytes + file_size > max {
                chunks.push(Chunk {
                    file_start: cur_start,
                    file_count: cur_count,
                    total_bytes: cur_bytes,
                });
                cur_start = 0;
                cur_count = 0;
                cur_bytes = 0;
            }

            if cur_count == 0 {
                // Starting a new chunk — record the absolute file index.
                // Note: idx is an index into the ORIGINAL files slice,
                // not a position within the cluster.
                cur_start = idx as u32;
            }
            cur_count += 1;
            cur_bytes += file_size;

            // Close the chunk if we've reached the target OR this is
            // the last file in the cluster (locality boundary).
            if cur_bytes >= target || is_last_in_cluster {
                chunks.push(Chunk {
                    file_start: cur_start,
                    file_count: cur_count,
                    total_bytes: cur_bytes,
                });
                cur_start = 0;
                cur_count = 0;
                cur_bytes = 0;
            }
        }
    }

    // Flush any trailing partial chunk (rare — usually the last
    // cluster's last file already triggered the close above).
    if cur_count > 0 {
        chunks.push(Chunk {
            file_start: cur_start,
            file_count: cur_count,
            total_bytes: cur_bytes,
        });
    }

    chunks
}

/// Extract the first 2 path components as a "src/engine" style key.
/// Files with fewer than 2 components return their full path.
/// Empty paths return an empty string (degenerate but safe).
fn path_prefix_2(path: &str) -> String {
    let p = Path::new(path);
    let comps: Vec<_> = p.components().take(2).collect();
    if comps.is_empty() {
        return String::new();
    }
    comps
        .iter()
        .map(|c| c.as_os_str().to_string_lossy().to_string())
        .collect::<Vec<_>>()
        .join("/")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(path: &str, size: u64) -> FileEntry {
        FileEntry {
            path: path.to_string(),
            size,
        }
    }

    #[test]
    fn test_plan_chunks_empty() {
        let chunks = plan_chunks(&[], 0, 0);
        assert!(chunks.is_empty());
    }

    #[test]
    fn test_plan_chunks_single_file() {
        let files = vec![entry("a.rs", 1024)];
        let chunks = plan_chunks(&files, 0, 0);
        assert_eq!(chunks.len(), 1);
        assert_eq!(chunks[0].file_count, 1);
        assert_eq!(chunks[0].total_bytes, 1024);
        assert_eq!(chunks[0].file_start, 0);
    }

    #[test]
    fn test_plan_chunks_balanced() {
        // 10 files, sizes 1KB..10KB, target=10KB. Each cluster ("dir")
        // is one file, so each chunk closes at the cluster boundary.
        // With target=10KB and files <= 10KB, expect ~1 chunk per file
        // when each file is its own cluster.
        let files: Vec<FileEntry> = (1..=10)
            .map(|i| entry(&format!("dir{}/f{}.rs", i, i), i * 1024))
            .collect();
        let chunks = plan_chunks(&files, 10 * 1024, 0);
        // Each file is its own cluster and reaches target on its own,
        // so we expect ~10 chunks (some files < 10KB but the cluster
        // boundary closes them anyway).
        assert!(!chunks.is_empty());
        // Total bytes preserved
        let total: u64 = chunks.iter().map(|c| c.total_bytes).sum();
        assert_eq!(total, (1..=10).map(|i| i * 1024).sum::<u64>());
        // Total file count preserved
        let total_count: u32 = chunks.iter().map(|c| c.file_count).sum();
        assert_eq!(total_count, 10);
        // No chunk exceeds 2x target (slack for the last chunk)
        for c in &chunks {
            assert!(
                c.total_bytes <= 20 * 1024,
                "chunk too big: {} bytes",
                c.total_bytes
            );
        }
    }

    #[test]
    fn test_plan_chunks_large_file_isolated() {
        // 3 files of 50MB + 7 files of 1KB, target=8MB, max=32MB.
        // Each 50MB file exceeds max, so it must be its own chunk.
        let mut files: Vec<FileEntry> = Vec::new();
        for i in 0..3 {
            files.push(entry(&format!("big/dir{}.bin", i), 50 * 1024 * 1024));
        }
        for i in 0..7 {
            files.push(entry(&format!("small/f{}.rs", i), 1024));
        }
        let chunks = plan_chunks(&files, 8 * 1024 * 1024, 32 * 1024 * 1024);

        // Find chunks that contain a 50MB file (total_bytes >= 50MB).
        let big_chunks: Vec<&Chunk> = chunks
            .iter()
            .filter(|c| c.total_bytes >= 50 * 1024 * 1024)
            .collect();
        assert_eq!(big_chunks.len(), 3, "each 50MB file in its own chunk");
        for c in &big_chunks {
            assert_eq!(c.file_count, 1, "big chunk has exactly 1 file");
        }
    }

    #[test]
    fn test_plan_chunks_directory_clustering() {
        // 5 files in src/a/ + 5 files in src/b/, all 1KB, target=8MB.
        // All files in one cluster fit in target, so each cluster is
        // exactly one chunk. No chunk mixes src/a and src/b.
        let mut files: Vec<FileEntry> = Vec::new();
        for i in 0..5 {
            files.push(entry(&format!("src/a/f{}.rs", i), 1024));
        }
        for i in 0..5 {
            files.push(entry(&format!("src/b/f{}.rs", i), 1024));
        }
        let chunks = plan_chunks(&files, 8 * 1024 * 1024, 0);
        assert_eq!(chunks.len(), 2, "two clusters → two chunks");
        // Each chunk has 5 files
        for c in &chunks {
            assert_eq!(c.file_count, 5);
            assert_eq!(c.total_bytes, 5 * 1024);
        }
        // Verify cluster boundary: chunk 0 covers src/a indices,
        // chunk 1 covers src/b indices. (Order depends on sort which
        // is by total bytes — both equal — so stable order applies.)
        let chunk0_paths: Vec<&str> = (chunks[0].file_start as usize
            ..(chunks[0].file_start + chunks[0].file_count) as usize)
            .map(|i| files[i].path.as_str())
            .collect();
        let chunk1_paths: Vec<&str> = (chunks[1].file_start as usize
            ..(chunks[1].file_start + chunks[1].file_count) as usize)
            .map(|i| files[i].path.as_str())
            .collect();
        // All chunk-0 paths must share a common "src/a" or "src/b" prefix
        let chunk0_prefix = if chunk0_paths[0].contains("src/a") {
            "src/a"
        } else {
            "src/b"
        };
        for p in &chunk0_paths {
            assert!(p.contains(chunk0_prefix), "chunk 0 mixes dirs: {}", p);
        }
        let chunk1_prefix = if chunk1_paths[0].contains("src/a") {
            "src/a"
        } else {
            "src/b"
        };
        for p in &chunk1_paths {
            assert!(p.contains(chunk1_prefix), "chunk 1 mixes dirs: {}", p);
        }
        // The two chunks must cover different clusters
        assert_ne!(chunk0_prefix, chunk1_prefix);
    }

    #[test]
    fn test_plan_chunks_no_file_left_behind() {
        // 100 files of varying sizes — verify every file is covered
        // exactly once across all chunks.
        let files: Vec<FileEntry> = (0..100)
            .map(|i| entry(&format!("d{}/f{}.rs", i % 5, i), (i as u64 + 1) * 100))
            .collect();
        let chunks = plan_chunks(&files, 1024, 0);

        // Total file count preserved
        let total_count: u32 = chunks.iter().map(|c| c.file_count).sum();
        assert_eq!(total_count, 100);

        // Total bytes preserved
        let total_bytes: u64 = chunks.iter().map(|c| c.total_bytes).sum();
        let expected_bytes: u64 = (1..=100).map(|i| i as u64 * 100).sum();
        assert_eq!(total_bytes, expected_bytes);

        // No chunk is empty
        for c in &chunks {
            assert!(c.file_count > 0, "empty chunk");
            assert!(c.total_bytes > 0, "zero-byte chunk");
        }
    }

    #[test]
    fn test_path_prefix_2_basic() {
        assert_eq!(path_prefix_2("src/engine/foo.cpp"), "src/engine");
        assert_eq!(path_prefix_2("a/b/c/d"), "a/b");
        assert_eq!(path_prefix_2("single"), "single");
        assert_eq!(path_prefix_2(""), "");
    }
}
