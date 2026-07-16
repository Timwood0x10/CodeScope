---
name: Pull Request
about: Submit changes to CodeScope
title: ""
labels: ""
assignees: ""
---

## Summary

**What does this PR change?** (1-2 sentences)

**Motivation:** Why is this change needed? (Link to related issue: `Closes #123`)

**How was it tested?** (Manual steps, existing tests, new tests)

---

## Self-Review Checklist (Author)

### Memory & Resources
- [ ] Raw pointers are null-checked before use
- [ ] `std::string::c_str()` does not outlive the source string
- [ ] File handles / sockets are closed on all code paths
- [ ] Ownership is clear (non-copyable types have `= delete` or comment)
- [ ] No raw `new`/`delete` outside FFI boundaries (use RAII)

### FFI Safety
- [ ] `unsafe` block inputs are validated (null, bounds, type)
- [ ] Returned `*mut c_char` has a single, documented owner responsible for freeing
- [ ] FFI layer does not construct raw SQL queries (delegates to `GraphStore` methods)
- [ ] Every `extern "C"` function wraps its body in `try/catch` (no exceptions cross FFI boundary)
- [ ] FFI string inputs are null-checked (`if (!param || !*param) return error_json`)

### SQLite
- [ ] Single connection handle is serialized (mutex / connection pool)
- [ ] `busy_timeout` is set
- [ ] Transaction boundaries are well-defined (no open txns leaked)

### JSON
- [ ] Output is never hand-joined — uses `jsonEscape()` or structured builders
- [ ] Malformed input does not crash the server (returns error instead)

### Cross-Platform
- [ ] New POSIX API calls have a Windows branch or `#error`
- [ ] File paths use `std::filesystem` or platform-agnostic API
- [ ] Shell commands avoid shell injection (no unchecked metacharacters)

### Performance
- [ ] Performance changes include before/after benchmark data
- [ ] No new full-table scans or N+1 queries without a size-limit justification

### Testing
- [ ] Core logic changes (call graph, semantic search, JSON, FFI boundary) include unit tests or regression tests
- [ ] New public functions have corresponding tests

### Documentation
- [ ] README updated if new tools/features added
- [ ] CHANGELOG.md entry added
- [ ] Code comments in English (per code_rules.md)

---

## Review Requirements

| Area | Required Approvals |
|------|:------------------:|
| Core modules (FFI / store / parser / graph builder / LSP) | **2 approvals** (including 1 module owner) |
| Security-related changes | **2 approvals** (including security review) |
| Performance changes | **1 approval + benchmark data** |
| Other changes | **1 approval** |

## Severity Quick Reference

- 🔴 **Blocker** — Must fix before merge
- 🟠 **Major** — Should fix (can track with issue)
- 🟡 **Minor** — Optional
