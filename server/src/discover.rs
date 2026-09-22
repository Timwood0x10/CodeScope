//! Module-discovery helpers for the built-in parallel scheduler.
//!
//! These functions provide a single source of truth for "which top-level
//! modules exist" and "which files belong to a module" so the scheduler
//! (`server/src/scheduler/`) does not need to call the slow `discover`
//! tool (which is designed for project-overview UI display, not for
//! dispatching workers).
//!
//! See `builtin-scheduler-design.md` §4.2 for the rationale. The skip
//! rules mirror the C++ `FilterPolicy::shouldSkipEntry` so the file list
//! we emit matches what the worker will actually index.

use serde_json::{Value, json};
use std::path::Path;
use walkdir::WalkDir;

/// Whether the indexer would skip this path.
///
/// Delegates to the engine's `FilterPolicy` — the single authority — instead
/// of keeping a copy of the skip rules. The copy is what let two defects
/// through in one review: a `.gitignore`d top-level directory was still
/// reported as a module (the copy read no ignore file at all, so the scheduler
/// dispatched a worker that indexed zero files), and the extension list below
/// advertised `.zig` while the engine has no Zig support. `root` is the
/// project root `rel_path` is relative to, which is the shape the indexer
/// passes, so project-anchored rules can match.
fn skipped_by_engine(root: &str, rel_path: &str, is_dir: bool) -> bool {
    crate::ffi::path_is_skipped(root, rel_path, is_dir)
}

/// Path of `entry_path` relative to `root`, the form both the indexer and
/// `skipped_by_engine` expect.
fn rel_to(root: &Path, entry_path: &Path) -> String {
    entry_path
        .strip_prefix(root)
        .unwrap_or(entry_path)
        .to_string_lossy()
        .to_string()
}

/// Countable source files, decided by the engine.
///
/// Delegates to `FilterPolicy::detectLanguage`, so the server cannot count a
/// language the indexer cannot parse. The former hand-written list contained
/// `.zig`, `.rb`, `.kt`, `.cs`, `.vue` and others with no engine support; on a
/// 373-file Zig project the count said "source files" and the index contained
/// zero entities for them.
fn is_source_file(name: &str) -> bool {
    crate::ffi::is_indexable_source(name)
}

/// Module name for the files that sit DIRECTLY in the target directory.
///
/// They belong to no top-level directory, so a walk that only looks at
/// subdirectories drops them: a flat project (all sources in one directory)
/// reported `"modules":[]` and indexed nothing at all, and a project mixing
/// root files with subdirectories silently lost the root files. The scheduler
/// roots this module's worker at the project directory and excludes the
/// subdirectories, so the root files are indexed exactly once.
pub const ROOT_MODULE_NAME: &str = ".";

/// Absolute paths of the source files that sit directly in `dir_path`,
/// sorted. Mirrors the shape of `discover_files` for one directory level and
/// is the companion to ROOT_MODULE_NAME.
pub fn root_source_files(dir_path: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let entries = match std::fs::read_dir(Path::new(dir_path)) {
        Ok(e) => e,
        Err(_) => return out,
    };
    for entry in entries.flatten() {
        if entry.path().is_dir() {
            continue;
        }
        let fname = entry.file_name().to_string_lossy().to_string();
        if skipped_by_engine(dir_path, &fname, false) || !is_source_file(&fname) {
            continue;
        }
        out.push(entry.path().to_string_lossy().to_string());
    }
    out.sort();
    out
}

/// Discover top-level modules and their source-file counts.
///
/// Walks the project root, counts source files per top-level directory,
/// and returns a JSON object suitable for the scheduler's proportional
/// CPU allocation:
///
/// ```json
/// {"ok":true,"project_path":"...","total_files":N,"total_modules":N,
///  "modules":[{"name":"engine","files":120},{"name":"server","files":10}]}
/// ```
///
/// Skip rules mirror `FilterPolicy` so the count matches what the worker
/// will actually parse (modulo nested `test/`/`docs/` dirs which the
/// worker additionally skips — see `builtin-scheduler-design.md` §1.2).
/// The count is therefore an upper bound; the worker reports the true
/// `discovery.candidate_files` in its stdout JSON, which the scheduler
/// uses for the final summary (so reported numbers are always accurate).
pub fn discover_modules(dir_path: &str) -> String {
    let root = Path::new(dir_path);
    if !root.is_dir() {
        return json!({
            "ok": false,
            "error": format!("directory not found: {} [module=discover, method=discover_modules]", dir_path)
        })
        .to_string();
    }

    let mut modules: Vec<(String, u64, u64)> = Vec::new();
    let mut total_files: u64 = 0;

    let entries = match std::fs::read_dir(root) {
        Ok(e) => e,
        Err(e) => {
            return json!({
                "ok": false,
                "error": format!("read_dir failed: {} [module=discover, method=discover_modules]", e)
            })
            .to_string();
        }
    };

    for entry in entries.flatten() {
        let name = entry.file_name();
        let name_str = name.to_string_lossy().to_string();
        let path = entry.path();

        if !path.is_dir() {
            continue;
        }
        if skipped_by_engine(dir_path, &name_str, true) {
            continue;
        }

        // Recursively count source files in this top-level module.
        let mut count: u64 = 0;
        // Total source bytes in this module. Parse cost scales with file
        // SIZE (line count), not file count — rustc's compiler/ files are
        // far larger than library/ files, so a pure file-count weight
        // under-allocates workers to the slowest module. The scheduler
        // uses this as the worker-allocation weight.
        let mut bytes: u64 = 0;
        let walk = WalkDir::new(&path).into_iter().filter_entry(|e| {
            if e.depth() == 0 {
                return true;
            }
            if e.file_type().is_dir() {
                !skipped_by_engine(dir_path, &rel_to(Path::new(dir_path), e.path()), true)
            } else {
                true
            }
        });
        for entry in walk.flatten() {
            if entry.file_type().is_file() {
                let fname = entry.file_name().to_string_lossy();
                if is_source_file(&fname)
                    && !skipped_by_engine(
                        dir_path,
                        &rel_to(Path::new(dir_path), entry.path()),
                        false,
                    )
                {
                    count += 1;
                    if let Ok(meta) = entry.metadata() {
                        bytes += meta.len();
                    }
                }
            }
        }
        if count > 0 {
            modules.push((name_str, count, bytes));
            total_files += count;
        }
    }

    // Files directly in the target directory. The loop above only ever
    // descends into subdirectories, so these were counted nowhere: a flat
    // project produced an empty module list and indexed nothing, and a project
    // with both root files and subdirectories lost the root files. They become
    // the root module, whose worker restricts itself to this level.
    let root_files = root_source_files(dir_path);
    if !root_files.is_empty() {
        let root_bytes: u64 = root_files
            .iter()
            .filter_map(|p| std::fs::metadata(p).ok())
            .map(|m| m.len())
            .sum();
        total_files += root_files.len() as u64;
        modules.push((
            ROOT_MODULE_NAME.to_string(),
            root_files.len() as u64,
            root_bytes,
        ));
    }

    // Sort by file count descending so the scheduler dispatches the
    // largest modules first (better CPU utilisation when `--parallel`
    // is smaller than the module count).
    modules.sort_by_key(|b| std::cmp::Reverse(b.1));

    let modules_json: Vec<Value> = modules
        .iter()
        .map(|(n, c, b)| json!({"name": n, "files": c, "bytes": b}))
        .collect();

    json!({
        "ok": true,
        "project_path": dir_path,
        "total_files": total_files,
        "total_modules": modules.len(),
        "modules": modules_json
    })
    .to_string()
}

/// Discover all candidate source files under a directory.
///
/// Used by the scheduler's quarantine binary search to obtain a file
/// list that approximates what the worker would index. The worker
/// re-filters via C++ `FilterPolicy::shouldSkipEntry`, so any over-
/// inclusion here is silently dropped by the worker — the only risk
/// is under-inclusion, which would prevent quarantine from finding a
/// crashing file. We therefore mirror the worker's skip rules here.
///
/// Output:
/// ```json
/// {"ok":true,"total":N,"files":["abs/path1","abs/path2",...]}
/// ```
pub fn discover_files(dir_path: &str) -> String {
    let root = Path::new(dir_path);
    if !root.is_dir() {
        return json!({
            "ok": false,
            "error": format!("directory not found: {} [module=discover, method=discover_files]", dir_path)
        })
        .to_string();
    }

    let walk = WalkDir::new(root).into_iter().filter_entry(|e| {
        if e.depth() == 0 {
            return true;
        }
        if e.file_type().is_dir() {
            // The engine decides, so the depth-3 `top_only_skip_dirs_` rule is
            // applied exactly as the worker applies it rather than being
            // approximated at depth 1 as it used to be here.
            return !skipped_by_engine(dir_path, &rel_to(root, e.path()), true);
        }
        true
    });

    let mut files: Vec<String> = Vec::new();
    for entry in walk.flatten() {
        if !entry.file_type().is_file() {
            continue;
        }
        let fname = entry.file_name().to_string_lossy();
        if !is_source_file(&fname)
            || skipped_by_engine(dir_path, &rel_to(root, entry.path()), false)
        {
            continue;
        }
        // Use absolute path so the worker can resolve regardless of cwd.
        let abs = entry.path().to_string_lossy().to_string();
        files.push(abs);
    }

    files.sort();

    json!({
        "ok": true,
        "total": files.len(),
        "files": files
    })
    .to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn make_tmpdir(name: &str) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(format!("codescope_test_{}", name));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }

    #[test]
    fn test_skipped_by_engine_matches_known_vendors() {
        // These assertions used to run against a Rust copy of the C++ skip
        // lists. They now run against the engine's FilterPolicy, i.e. the
        // implementation the indexer itself uses — which is the point of the
        // delegation: there is no second list left to drift.
        let root = make_tmpdir("skip_vendors");
        let root = root.to_str().unwrap();
        for vendor in ["node_modules", "target", "vendor", "third_party"] {
            assert!(skipped_by_engine(root, vendor, true), "{vendor} must skip");
        }
        assert!(!skipped_by_engine(root, "src", true));
        assert!(!skipped_by_engine(root, "engine", true));
        // The depth-gated names are applied at the depth the indexer applies
        // them (first three components), not approximated at depth 1.
        assert!(skipped_by_engine(root, "test", true));
        assert!(skipped_by_engine(root, "docs", true));
        assert!(!skipped_by_engine(root, "src", true));
        assert!(!skipped_by_engine(root, "compiler", true));
    }

    #[test]
    fn test_is_source_file_recognises_extensions() {
        assert!(is_source_file("foo.rs"));
        assert!(is_source_file("bar.cpp"));
        // "foo.d.ts" resolves through the ".ts" entry: the extension is
        // everything after the LAST dot. The ".d.ts" allow-list entry that
        // used to sit in the list could never fire and has been removed —
        // this assertion is what keeps the behaviour identical.
        assert!(is_source_file("types.d.ts"));
        assert!(is_source_file("baz.go"));
        assert!(is_source_file("a.py"));
        assert!(!is_source_file("readme.md"));
        assert!(!is_source_file("Makefile"));
        assert!(!is_source_file("noext"));
        // Counted only if the ENGINE can parse it. The list used to be
        // hand-written and counted `.zig` (362 files in OmniScope) although
        // Zig is deliberately unsupported — no mapping, no translator, and
        // none planned — so the count advertised files the index could never
        // contain. Ruby, which the same list contains, IS recognized: the
        // point is to follow the engine, not to shorten a list.
        assert!(!is_source_file("main.zig"), "no Zig support in the engine");
        assert!(
            is_source_file("script.rb"),
            "the engine does recognize Ruby"
        );
    }

    #[test]
    fn test_gitignored_top_level_dir_is_not_a_module() {
        // Drift that the delegation removes: the module list was decided by a
        // Rust copy of the skip rules that read no ignore file, so a
        // `.gitignore`d top-level directory was reported as a module and the
        // scheduler dispatched a worker for it that indexed zero files.
        let dir = make_tmpdir("discover_modules_gitignore");
        fs::write(dir.join(".gitignore"), "**/build-*/\n").unwrap();
        fs::create_dir_all(dir.join("build-x/src")).unwrap();
        fs::write(dir.join("build-x/src/main.rs"), "fn main() {}").unwrap();
        fs::create_dir_all(dir.join("src")).unwrap();
        fs::write(dir.join("src/lib.rs"), "pub fn f() {}").unwrap();

        let v: Value = serde_json::from_str(&discover_modules(dir.to_str().unwrap())).unwrap();
        assert_eq!(v["total_modules"], 1, "only src/ is a module: {v}");
        assert_eq!(v["modules"][0]["name"], "src");

        let files: Value = serde_json::from_str(&discover_files(dir.to_str().unwrap())).unwrap();
        let list = files["files"].as_array().unwrap();
        assert_eq!(list.len(), 1, "only src/lib.rs is a candidate: {list:?}");
    }

    #[test]
    fn test_discover_modules_counts_root_files() {
        // Regression (#22): files directly in the target directory were counted
        // nowhere. A flat project reported no modules at all and indexed
        // nothing; a project mixing root files with subdirectories lost the
        // root files.
        let flat = make_tmpdir("discover_modules_flat");
        fs::write(flat.join("a.go"), "package main").unwrap();
        let v: Value = serde_json::from_str(&discover_modules(flat.to_str().unwrap())).unwrap();
        assert_eq!(v["total_modules"], 1, "a flat project has one module");
        assert_eq!(v["modules"][0]["name"], ROOT_MODULE_NAME);
        assert_eq!(v["modules"][0]["files"], 1);
        assert_eq!(v["total_files"], 1);

        let mixed = make_tmpdir("discover_modules_mixed");
        fs::write(mixed.join("top.go"), "package main").unwrap();
        fs::create_dir_all(mixed.join("sub")).unwrap();
        fs::write(mixed.join("sub/b.go"), "package sub").unwrap();
        let v: Value = serde_json::from_str(&discover_modules(mixed.to_str().unwrap())).unwrap();
        assert_eq!(v["total_modules"], 2, "root files + sub/ = two modules");
        assert_eq!(
            v["total_files"], 2,
            "the root file must be counted as well as sub/b.go"
        );
        let names: Vec<&str> = v["modules"]
            .as_array()
            .unwrap()
            .iter()
            .map(|m| m["name"].as_str().unwrap())
            .collect();
        assert!(names.contains(&ROOT_MODULE_NAME));
        assert!(names.contains(&"sub"));
    }

    #[test]
    fn test_root_source_files_are_direct_children_only() {
        let dir = make_tmpdir("root_source_files");
        fs::write(dir.join("a.go"), "package main").unwrap();
        fs::write(dir.join("notes.md"), "not source").unwrap();
        fs::create_dir_all(dir.join("sub")).unwrap();
        fs::write(dir.join("sub/b.go"), "package sub").unwrap();
        let files = root_source_files(dir.to_str().unwrap());
        assert_eq!(files.len(), 1);
        assert!(files[0].ends_with("a.go"));
    }

    #[test]
    fn test_discover_modules_returns_array() {
        let dir = make_tmpdir("discover_modules");
        // Create src/ with one .rs file
        fs::create_dir_all(dir.join("src")).unwrap();
        fs::write(dir.join("src/main.rs"), "fn main() {}").unwrap();
        // Create docs/ (should be skipped — top-only skip)
        fs::create_dir_all(dir.join("docs")).unwrap();
        fs::write(dir.join("docs/guide.rs"), "// doc").unwrap();
        // Create node_modules/ (should be skipped — any-depth skip)
        fs::create_dir_all(dir.join("node_modules/pkg")).unwrap();
        fs::write(dir.join("node_modules/pkg/lib.js"), "// lib").unwrap();

        let out = discover_modules(dir.to_str().unwrap());
        let v: Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["ok"], true);
        assert_eq!(v["total_modules"], 1);
        assert_eq!(v["modules"][0]["name"], "src");
        assert_eq!(v["modules"][0]["files"], 1);
    }

    #[test]
    fn test_discover_modules_missing_dir_returns_error() {
        let out = discover_modules("/nonexistent/path/that/does/not/exist");
        let v: Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["ok"], false);
        assert!(v["error"].as_str().unwrap().contains("directory not found"));
    }

    #[test]
    fn test_discover_files_returns_abs_paths() {
        let dir = make_tmpdir("discover_files");
        fs::create_dir_all(dir.join("a")).unwrap();
        fs::write(dir.join("a/f1.rs"), "// f1").unwrap();
        fs::write(dir.join("a/f2.cpp"), "// f2").unwrap();
        // Non-source file should be excluded
        fs::write(dir.join("a/readme.md"), "doc").unwrap();

        let out = discover_files(dir.to_str().unwrap());
        let v: Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["ok"], true);
        assert_eq!(v["total"], 2);
        let files = v["files"].as_array().unwrap();
        assert_eq!(files.len(), 2);
        // Each path must be absolute
        for f in files {
            let s = f.as_str().unwrap();
            assert!(s.starts_with('/') || s.starts_with(&*std::env::temp_dir().to_string_lossy()));
        }
    }
}
