# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in CodeScope, please report it
**privately** — do **not** open a public GitHub issue.

- **Preferred:** open a private security advisory on GitHub:
  https://github.com/Timwood0x10/CodeScope/security/advisories/new
- **Alternatively:** email the maintainer directly.

Please include:

- A description of the issue and its potential impact.
- Steps to reproduce (minimal example or proof of concept).
- Affected version / commit.
- Any suggested fix.

You should receive an initial response within **72 hours**. Please do not disclose
the issue publicly until a fix has been released (see timeline below).

## Supported Versions

Only the **latest release** of CodeScope receives security fixes. Users should
always run the newest release.

| Version          | Supported |
|------------------|-----------|
| Latest release   | ✅        |
| Older releases   | ❌        |

## Scope

CodeScope runs locally and indexes source code on the user's machine.
Security-relevant issues include, but are not limited to:

- **Path traversal** during indexing or queries — escaping the project root and
  accessing files outside the indexed directory.
- **SQL injection** in graph/query builders. All queries must use parameterized
  statements; hand-joined SQL is never acceptable.
- **FFI memory corruption** — buffer overflows, use-after-free, double-free, or
  exceptions crossing the `extern "C"` boundary between the Rust server and the
  C++ engine.
- **Malformed-input crashes** — a crafted source file or JSON argument that crashes
  the server instead of returning a structured error.
- **Command injection** through shell-out calls or unchecked file paths.

Out of scope: bugs with no security impact, denial-of-service via extremely large
inputs on a component explicitly designed to process them, and issues in third-party
dependencies (report those upstream).

## Disclosure Timeline

We follow a coordinated **90-day disclosure** timeline:

1. **Day 0:** You report the issue privately; we acknowledge within 72 hours.
2. **Day 1–30:** We investigate and develop a fix, keeping you updated.
3. **Day 30–60:** The fix is validated and a patched release is prepared.
4. **Day 60–90:** The patched release is published; a public advisory is drafted.
5. **Day 90:** Public disclosure (or earlier, once a fix is widely available), with
   credit to the reporter unless they prefer to remain anonymous.

If a fix is delayed, we will keep you informed and agree on an extended timeline.
