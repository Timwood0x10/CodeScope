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

/// Built-in skip dirs matching `FilterPolicy::normal_skip_dirs_`.
/// Applied to EVERY path component (any depth) so nested `target/` and
/// `node_modules/` directories are pruned during the walk.
fn is_skip_dir(name: &str) -> bool {
    let lower = name.to_lowercase();
    matches!(
        lower.as_str(),
        ".git"
            | ".svn"
            | ".hg"
            | ".bzr"
            | "node_modules"
            | ".venv"
            | "venv"
            | "env"
            | ".env"
            | "__pycache__"
            | ".pytest_cache"
            | ".mypy_cache"
            | ".cache"
            | ".codescope"
            | "target"
            | "build"
            | "dist"
            | ".next"
            | ".turbo"
            | ".direnv"
            | "vendor"
            | "third_party"
            | "third-party"
            | ".bundle"
            | ".gem"
            | "go_pkg"
            | "pkg"
    )
}

/// Top-only skip dirs matching `FilterPolicy::top_only_skip_dirs_`.
/// Source-bearing dirs that are rarely the focus of analysis
/// (test/, docs/, bench/, examples/, ...). Matched ONLY against the
/// first path component (top-level dirs), so Java packages like
/// `org/springframework/samples/petclinic` are NOT falsely skipped.
fn is_top_only_skip_dir(name: &str) -> bool {
    let lower = name.to_lowercase();
    matches!(
        lower.as_str(),
        "test"
            | "tests"
            | "docs"
            | "doc"
            | "documentation"
            | "examples"
            | "example"
            | "samples"
            | "sample"
            | "scripts"
            | "hack"
            | "migrations"
            | "seeds"
            | "e2e"
            | "integration"
            | "locale"
            | "locales"
            | "i18n"
            | "l10n"
            | "assets"
            | "static"
            | "public"
            | "media"
            | "external"
            | "vendored"
            | "bench"
            | "benchmarks"
    )
}

/// Skip hidden dirs/files (those starting with `.`).
fn is_skip_prefix(name: &str) -> bool {
    name.starts_with('.')
}

/// Countable source extensions matching `FilterPolicy::isSourceFile`.
/// Kept in sync with the C++ list so the scheduler's file count agrees
/// with what the worker will actually parse.
fn is_source_file(name: &str) -> bool {
    let dot = match name.rfind('.') {
        Some(d) => d,
        None => return false,
    };
    let ext = name[dot..].to_lowercase();
    matches!(
        ext.as_str(),
        ".c" | ".h"
            | ".cpp"
            | ".hpp"
            | ".cc"
            | ".cxx"
            | ".hh"
            | ".hxx"
            | ".rs"
            | ".go"
            | ".py"
            | ".java"
            | ".kt"
            | ".kts"
            | ".js"
            | ".jsx"
            | ".ts"
            | ".tsx"
            | ".swift"
            | ".rb"
            | ".php"
            | ".cs"
            | ".fs"
            | ".scala"
            | ".clj"
            | ".cljs"
            | ".ex"
            | ".exs"
            | ".erl"
            | ".hrl"
            | ".vue"
            | ".svelte"
            | ".mjs"
            | ".cjs"
            | ".mts"
            | ".cts"
            | ".d.ts"
            | ".wasm"
            | ".zig"
            | ".mojo"
    )
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

    let mut modules: Vec<(String, u64)> = Vec::new();
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
        if is_skip_dir(&name_str) || is_top_only_skip_dir(&name_str) || is_skip_prefix(&name_str) {
            continue;
        }

        // Recursively count source files in this top-level module.
        let mut count: u64 = 0;
        let walk = WalkDir::new(&path).into_iter().filter_entry(|e| {
            let fname = e.file_name().to_string_lossy();
            if e.depth() == 0 {
                return true;
            }
            if e.file_type().is_dir() {
                !is_skip_dir(&fname) && !is_skip_prefix(&fname)
            } else {
                true
            }
        });
        for entry in walk.flatten() {
            if entry.file_type().is_file() {
                let fname = entry.file_name().to_string_lossy();
                if is_source_file(&fname) {
                    count += 1;
                }
            }
        }
        if count > 0 {
            modules.push((name_str, count));
            total_files += count;
        }
    }

    // Sort by file count descending so the scheduler dispatches the
    // largest modules first (better CPU utilisation when `--parallel`
    // is smaller than the module count).
    modules.sort_by_key(|b| std::cmp::Reverse(b.1));

    let modules_json: Vec<Value> = modules
        .iter()
        .map(|(n, c)| json!({"name": n, "files": c}))
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
        let fname = e.file_name().to_string_lossy();
        if e.depth() == 0 {
            return true;
        }
        if e.file_type().is_dir() {
            // Apply both any-depth skip dirs and top-only skip dirs at
            // depth 1. The C++ FilterPolicy checks `top_only_skip_dirs_`
            // against the first 3 path components — we approximate by
            // checking depth==1 here. Deeper nested test/docs dirs are
            // also skipped to match worker behaviour (see design §1.2).
            if is_skip_dir(&fname) || is_skip_prefix(&fname) {
                return false;
            }
            if e.depth() == 1 && is_top_only_skip_dir(&fname) {
                return false;
            }
            true
        } else {
            true
        }
    });

    let mut files: Vec<String> = Vec::new();
    for entry in walk.flatten() {
        if !entry.file_type().is_file() {
            continue;
        }
        let fname = entry.file_name().to_string_lossy();
        if !is_source_file(&fname) {
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
    fn test_is_skip_dir_matches_known_vendors() {
        assert!(is_skip_dir("node_modules"));
        assert!(is_skip_dir("target"));
        assert!(is_skip_dir("vendor"));
        assert!(is_skip_dir("third_party"));
        assert!(!is_skip_dir("src"));
        assert!(!is_skip_dir("engine"));
    }

    #[test]
    fn test_is_top_only_skip_dir_matches_test_docs() {
        assert!(is_top_only_skip_dir("test"));
        assert!(is_top_only_skip_dir("docs"));
        assert!(is_top_only_skip_dir("bench"));
        assert!(!is_top_only_skip_dir("src"));
        assert!(!is_top_only_skip_dir("compiler"));
    }

    #[test]
    fn test_is_source_file_recognises_extensions() {
        assert!(is_source_file("foo.rs"));
        assert!(is_source_file("bar.cpp"));
        assert!(is_source_file("baz.go"));
        assert!(is_source_file("a.py"));
        assert!(!is_source_file("readme.md"));
        assert!(!is_source_file("Makefile"));
        assert!(!is_source_file("noext"));
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
