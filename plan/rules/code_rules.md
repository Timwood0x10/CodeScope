**Here is a clean, professional English Coding Standards document** for your project.

---

**CodeScope Coding Standards**  
**C++ & Rust**  
**Version 1.0**  


所有公共函数和方法必须有完整的注释，包括参数、返回值、异常等。
所有的错误禁止静默处理，必须显示处理，而且有完整的错误追踪链，能定位到什么模块的，什么方法。
禁止用git commit


FFI： 规范

绝对不要“单条符号调用一次 FFI”：如果 C++ 扫出一个符号就调用一次 Rust 导出的 Insert 接口，几千次 FFI 的上下文切换开销会直接毁掉 ms 级的目标。

块级传递（Chunking）或回调（Callback）：

方案 A：C++ 扫描完一个文件，将该文件所有的 symbols 压入一个连续的 C-Style 结构体数组（POD struct array），一次性通过指针传给 Rust，由 Rust 侧的 rusqlite 开启事务进行 std::vec 批量插入。

方案 B：C++ 侧直接静态链接 sqlite3（或者通过相同的 libsqlite 动态链接），直接由 C++ 写入事实表。Rust 侧只负责传入 db_path 和任务指令，并负责读取展示。从你的 Mermaid 架构图来看，方案 B（C++ Core 直接写 FACTS 表）是最符合你当前设计的，也能压榨出极限的 I/O 性能。



### 1. General Principles

- **File Size Limit**: No source file may exceed **1000 lines** (including blank lines and comments). Split files when necessary.
- **Comments**: All comments must be written in **English**. Use clear, concise language. Include purpose, parameters, return values, and any important invariants for complex functions.
- **Code Style**:
  - **Rust**: Follow official `rustfmt` + `cargo clippy --all-targets -- -D warnings`.
  - **C++**: Follow Google C++ Style Guide + `clang-format`.
- **Language Version**: Rust 2024 Edition, C++23.

### 2. Memory Management (Strict Rules)

**Rust**:
- Prefer safe abstractions: `Box`, `Vec`, `String`, `Rc/Arc`, `Cow`, etc.
- Avoid `unsafe` unless absolutely necessary (mainly FFI boundaries).
- Always use `reserve()` for `Vec`/`String` when size is predictable.
- All `unsafe` blocks must have a detailed safety comment explaining invariants.

**C++**:
- **Never** use raw `new`/`delete` outside FFI boundaries.
- Use RAII: `std::unique_ptr`, `std::shared_ptr`, `std::vector`, `std::string`.
- All resources must be managed by RAII types.

**General**:
- Prefer moving ownership over copying.
- Avoid global/static mutable state unless strictly necessary.

### 3. FFI Boundaries (Most Critical)

All cross-language boundaries (Rust ↔ C++) must follow these strict rules:

- Use `extern "C"` and `#[no_mangle]` for FFI functions.
- **Ownership Transfer Rules**:
  - Rust-allocated memory passed to C++ must be explicitly freed by C++ using a provided `free_xxx()` function.
  - C++-allocated memory passed to Rust must be wrapped in `Box` or explicitly transferred.
- **Data Types**:
  - Use `CString` / `CStr` for strings.
  - Use opaque pointers (`*mut c_void` or boxed structs) for complex objects.
  - Never pass raw Rust structs by value across FFI.
- **Error Handling**:
  - FFI functions should return `int` error codes (0 = success).
  - Use `#[repr(C)]` for error structs if needed.
- **Safety Documentation**:
  - Every FFI function must have a comment block explaining memory ownership, lifetime, and thread safety.

**Example (Rust side)**:
```rust
/// # Safety
/// The caller must ensure `ptr` is valid and call `free_xxx` when done.
#[no_mangle]
pub unsafe extern "C" fn create_xxx() -> *mut c_void { ... }
```

### 4. Testing Requirements

- Every public function / module must have corresponding tests.
- **Boundary & Edge Cases** are **mandatory**:
  - Null / empty inputs
  - Maximum / minimum values
  - Error conditions
  - Concurrency / race conditions (for thread-safe code)
  - FFI boundary cases (ownership transfer, double-free prevention)
- Use property-based testing where appropriate (`proptest` in Rust).
- Integration tests must use real dependencies (e.g., real PostgreSQL for storage modules).

### 5. Additional Rules

- **No magic numbers** — use named constants.
- **Error handling**: Prefer `Result<T, E>` in Rust and `std::expected` / error codes in C++.
- **Dependencies**: Keep them minimal and well-documented.
- **Performance**: Profile critical paths. Use `cargo flamegraph` / `perf` regularly.

---

**Enforcement**:
- Use CI to check file length, Clippy, clang-format, and test coverage.
- All new code must be reviewed against this standard.
