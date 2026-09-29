// docs_lint.rs — documentation-liveness guards for the tool layer.
//
// Split out of tools/mod.rs to stay under the 1000-line rule (see
// plan/rules/code_rules.md); the tests it holds only need `std`, so they live
// here on their own.
//
// The guards exist because CodeScope's own README once promised things the code
// did not do. `CODESCOPE_VERBOSE` was documented in README §9 but never read by
// any production code, so setting it did nothing
// (CODE_REVIEW_2026-09-27.md D3-3). These tests make that drift fail the build.

#[cfg(test)]
mod tests {
    /// Append the contents of every source file under `dir` to `out`.
    fn collect_sources(dir: &std::path::Path, out: &mut String) {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                collect_sources(&path, out);
                continue;
            }
            let is_source = matches!(
                path.extension().and_then(|e| e.to_str()),
                Some("rs" | "cpp" | "h" | "hpp")
            );
            if !is_source {
                continue;
            }
            if let Ok(text) = std::fs::read_to_string(&path) {
                out.push_str(&text);
                out.push('\n');
            }
        }
    }

    /// True when `sources` contains a statement that READS `var` — a
    /// `getenv("VAR")` / `env::var("VAR")` call, or a `getenv(<const>)` where
    /// `<const>` is the constant holding the variable name
    /// (`kExcludePathsEnv = "CODESCOPE_EXCLUDE_PATHS"`).
    ///
    /// A set-only or comment-only mention deliberately does not count: that is
    /// exactly the `CODESCOPE_VERBOSE` failure mode this guards against (the
    /// variable was passed to workers but never read by production code).
    fn has_read_site(sources: &str, var: &str) -> bool {
        let quoted = format!("\"{var}\"");
        for line in sources.lines() {
            if !line.contains(&quoted) {
                continue;
            }
            if line.contains("getenv") || line.contains("env::var") {
                return true;
            }
            // `static constexpr const char *kFooEnv = "CODESCOPE_FOO";`
            // The name is only indirect: look for a getenv() of that constant.
            // The constant is the last identifier before the `=`.
            let const_name = line
                .split(&quoted)
                .next()
                .unwrap_or("")
                .rsplit([' ', '*', '(', ',', '=', '\t', ';'])
                .find(|token| {
                    !token.is_empty() && token.chars().all(|c| c.is_alphanumeric() || c == '_')
                });
            if let Some(name) = const_name
                && sources.contains(&format!("getenv({name})"))
            {
                return true;
            }
        }
        false
    }

    /// Regression (CODE_REVIEW_2026-09-27.md D3-3): `CODESCOPE_VERBOSE` was
    /// documented in README §9 but never read by any production code — setting
    /// it did nothing. Every variable the §9 table promises must have a real
    /// READER in `server/src` or `engine/src`, so a dead entry cannot creep
    /// back in. A set-only or comment-only mention deliberately does not count.
    #[test]
    fn test_documented_env_vars_are_read_by_production_code() {
        // Resolve every path from the compile-time crate directory so the test
        // does not depend on the working directory it is launched from.
        let crate_dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
        let readme_path = crate_dir.join("../README.md");
        let readme = std::fs::read_to_string(&readme_path).unwrap_or_else(|e| {
            panic!("cannot read {}: {e}", readme_path.display());
        });
        let section = readme
            .split("## 9. Environment Variables")
            .nth(1)
            .expect("README must have a §9 Environment Variables section");
        let section = section.split("\n## ").next().unwrap_or(section);

        let mut vars: Vec<String> = Vec::new();
        for line in section.lines() {
            let Some(rest) = line.strip_prefix("| `") else {
                continue;
            };
            let Some(name) = rest.split('`').next() else {
                continue;
            };
            if name.starts_with("CODESCOPE_") {
                vars.push(name.to_string());
            }
        }
        assert!(
            vars.len() >= 9,
            "expected the §9 table rows, parsed only {vars:?}"
        );

        let mut sources = String::new();
        collect_sources(&crate_dir.join("src"), &mut sources);
        collect_sources(&crate_dir.join("../engine/src"), &mut sources);

        for var in &vars {
            assert!(
                has_read_site(&sources, var),
                "README §9 documents `{var}` but no production code READS it \
                 (a set-only or comment-only reference does not count)"
            );
        }
    }

    /// The liveness check must reject a variable that is only ever SET or
    /// mentioned in a comment — the `CODESCOPE_VERBOSE` shape.
    #[test]
    fn test_env_var_liveness_rejects_set_only_variables() {
        let sources = "\
            cmd.env(\"CODESCOPE_DEAD_VAR\", \"0\");\n\
            // CODESCOPE_DEAD_VAR is documented but never read\n\
            let _ = std::env::var(\"CODESCOPE_LIVE_VAR\").ok();\n\
            static constexpr const char *kIndirectEnv = \"CODESCOPE_INDIRECT\";\n\
            const char *p = getenv(kIndirectEnv);\n";
        assert!(
            !has_read_site(sources, "CODESCOPE_DEAD_VAR"),
            "a set-only variable must not count as read"
        );
        assert!(has_read_site(sources, "CODESCOPE_LIVE_VAR"));
        assert!(
            has_read_site(sources, "CODESCOPE_INDIRECT"),
            "an indirect getenv(const) read must count"
        );
    }
}
