// discover.rs — quick project scan.
//
// Split out of tools/mod.rs (see plan/rules/code_rules.md 1000-line rule).
// Walks the directory tree and counts candidate source files per top-level
// directory, using the same filtering rules as the C++ engine's
// FilterPolicy. No parsing, no SQLite — just a fast filesystem walk, which
// is why it carries no dependency on the handler table or the FFI layer.

use serde_json::json;

/// ── Discover: quick project scan ────────────────────────────────
/// Walks the directory tree, counts candidate source files per top-level
/// directory (same filtering rules as the C++ engine's FilterPolicy).
/// No parsing, no SQLite — just a fast filesystem walk.
/// Output: JSON with per-directory file counts + total.
pub fn discover(dir_path: &str) -> String {
    use std::collections::BTreeMap;
    use std::path::Path;

    // Built-in skip dirs matching FilterPolicy's normal_skip_dirs_
    // Applied to EVERY path component (any depth).
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

    // Top-only skip dirs matching FilterPolicy's top_only_skip_dirs_
    // Source-bearing dirs that are rarely the focus of analysis
    // (test/, docs/, bench/, examples/, ...). Matched ONLY against the
    // first path component (top-level dirs), so Java packages like
    // org/springframework/samples/petclinic are NOT falsely skipped.
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

    fn is_skip_prefix(name: &str) -> bool {
        name.starts_with('.') // hidden dirs/files
    }

    // Countable source extensions (matching FilterPolicy::isSourceFile)
    fn is_source_file(name: &str) -> bool {
        let dot = name.rfind('.');
        if dot.is_none() {
            return false;
        }
        let ext = &name[dot.unwrap()..].to_lowercase();
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
            // ".d.ts" is deliberately NOT listed: the extension this matches
            // on is everything after the LAST '.', so "foo.d.ts" already
            // yields ".ts" and a ".d.ts" pattern could never match anything
            // the ".ts" entry does not. Declaration files are picked up by
            // that entry; if they should be EXCLUDED instead, that needs an
            // exclusion, not an allow-list entry that never fires.
                | ".wasm"
                | ".zig"
                | ".mojo"
        )
    }

    let root = Path::new(dir_path);
    if !root.is_dir() {
        return json!({
            "ok": false,
            "error": format!("directory not found: {}", dir_path)
        })
        .to_string();
    }

    let mut modules: BTreeMap<String, u64> = BTreeMap::new();
    let mut total_files: u64 = 0;
    let mut total_dirs: u64 = 0;
    let mut skipped_dirs: u64 = 0;
    let mut skipped_files: u64 = 0;

    // Walk top-level entries first
    if let Ok(entries) = std::fs::read_dir(root) {
        for entry in entries.flatten() {
            let name = entry.file_name();
            let name_str = name.to_string_lossy().to_string();
            let path = entry.path();

            if path.is_dir() {
                let base = name_str.clone();
                if is_skip_dir(&base) || is_top_only_skip_dir(&base) || is_skip_prefix(&base) {
                    skipped_dirs += 1;
                    continue;
                }
                total_dirs += 1;

                // Recursively count source files in this directory
                let mut count: u64 = 0;
                let walk = walkdir::WalkDir::new(&path).into_iter().filter_entry(|e| {
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
                        } else {
                            skipped_files += 1;
                        }
                    }
                }
                if count > 0 {
                    modules.insert(base, count);
                    total_files += count;
                }
            }
        }
    }

    json!({
        "ok": true,
        "project_path": dir_path,
        "total_dirs": total_dirs,
        "total_files": total_files,
        "skipped_dirs": skipped_dirs,
        "skipped_files": skipped_files,
        "modules": modules
    })
    .to_string()
}
