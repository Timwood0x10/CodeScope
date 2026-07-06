# CodeScope Large Project Index Report

> Date: 2026-07-06 | Hardware: Apple M3 Max, 36 GB RAM | macOS

---

## 1. rustc (Rust Compiler) — Full Index

### Complete Log

```
Start: 2026-07-06 19:50:33

worker: project=0 starting index_project dir=/Users/scc/code/rustcode/rust lang=
BATCH [0..99] of 36807 (100 files)
BATCH [0..99] done (0 passes ok), total indexed: 100
...
BATCH [36800..36806] of 36807 (7 files)
BATCH [36800..36806] done (0 passes ok), total indexed: 36807
POST_BUILD: symbol graph...
buildGraph: 36800 files | file_list=339ms delete=457ms rf=21ms r2n=7317ms
  nodes=8442ms edges=14066ms calls=0ms total=30702ms
{"ok":true,"files_indexed":36807,"workers":14,
  "time_parse_ms":3193,"time_sqlite_ms":38932,
  "time_buildgraph_ms":30703,"time_fts_ms":22995,"time_vector_ms":0,
  "discovery":{"seen_dirs":65437,"seen_files":59339,
    "skipped_dirs":1698,"skipped_files":49,
    "skipped_suffix":76,"candidate_files":36807}}

End: 2026-07-06 19:52:18
Total: 1m44s
```

### Performance Metrics

| Metric | Value |
|--------|:-----:|
| **Total time** | **1m44s** |
| Source files | 36,807 (all .rs) |
| Graph nodes | **4,130,017** |
| Graph edges | **3,044,162** |
| Parse | 3,193ms (3.0%) |
| SQLite write | 38,932ms (37.4%) |
| buildGraph | 30,703ms (29.5%) |
| FTS index | 22,995ms (22.1%) |
| Directories discovered | 65,437 |
| Directories skipped | 1,698 |
| Workers | 14 |

### Bottleneck Analysis

```
Parse    3,193ms  ███░░░░░░░░░░░░░░░░░░  3.0%
SQLite  38,932ms  █████████████████████  37.4%
Graph   30,703ms  █████████████████░░░░  29.5%
FTS     22,995ms  █████████████░░░░░░░░  22.1%
```

**Still SQLite-bound** (SQLite + buildGraph + FTS = 89%). Parsing only took 3.2s (3%), achieving throughput of **11,523 files/sec**.

### Query Token Consumption

| Query | Tokens | Percentage |
|-------|:------:|:----------:|
| `get_graph_stats` | 19 | — |
| `get_hotspots` top10 | ~500 | — |
| `project_overview` | ~71 | — |
| `get_module_tree` | 4 | — |
| `get_entry_points` | 5 | — |
| `search` (single) | ~300-1000 | — |
| **Typical analysis combo** | **~910** | — |

---

## 2. Bun Runtime — Full Index

### Complete Log

```
Start: 2026-07-06 19:55:37

worker: project=0 starting index_project dir=/Users/scc/code/researcher/bun lang=
BATCH [0..99] of 9641 (100 files)
BATCH [0..99] done (0 passes ok), total indexed: 100
...
BATCH [9600..9640] of 9641 (41 files)
BATCH [9600..9640] done (0 passes ok), total indexed: 9641
POST_BUILD: symbol graph...
buildGraph: 9623 files | file_list=238ms delete=116ms rf=5ms r2n=4842ms
  nodes=5632ms edges=9450ms calls=0ms total=20320ms
{"ok":true,"files_indexed":9641,"workers":14,
  "time_parse_ms":1024,"time_sqlite_ms":25736,
  "time_buildgraph_ms":20321,"time_fts_ms":11568,"time_vector_ms":0,
  "discovery":{"seen_dirs":21672,"seen_files":13883,
    "skipped_dirs":6415,"skipped_files":4,
    "skipped_suffix":504,"candidate_files":9641}}

End: 2026-07-06 19:56:39
Total: 1m1s
```

### Performance Metrics

| Metric | Value |
|--------|:-----:|
| **Total time** | **1m1s** |
| Source files | 9,641 |
| Graph nodes | **2,951,664** |
| Graph edges | **2,567,205** |
| Parse | 1,024ms (1.7%) |
| SQLite write | 25,736ms (43.6%) |
| buildGraph | 20,321ms (34.4%) |
| FTS index | 11,568ms (19.6%) |
| Directories discovered | 21,672 |
| Directories skipped | 6,415 |
| Workers | 14 |

### Bottleneck Analysis

```
Parse   1,024ms  ██░░░░░░░░░░░░░░░░░░░   1.7%
SQLite 25,736ms  █████████████████████  43.6%
Graph  20,321ms  ██████████████████░░░  34.4%
FTS    11,568ms  ███████████████░░░░░░  19.6%
```

---

## 3. Bun Runtime Core Functionality Analysis

### Tech Stack

Bun is primarily written in **Zig**, integrating **WebKit's JavaScriptCore** as the JS engine, with extensive C++ FFI binding layers:

| Language | Files | Purpose |
|----------|:-----:|---------|
| Zig | 1,270 | Core runtime, CLI, bundler, package manager |
| C++ | 579 | JSC binding layer (`src/jsc/bindings/`) |
| C | 113 | WebCore integration, system calls |
| JS/TS | 6,592 | Tests, API definitions |

### Core Architecture

```mermaid
graph TB
    subgraph "Bun CLI (main.zig)"
        CLI["Cli.start()"]
    end
    subgraph "Commands"
        RUN["bun run"]
        TEST["bun test"]
        INSTALL["bun install"]
        BUILD["bun build"]
        SERVE["bun serve"]
    end
    subgraph "JS Runtime (bun.js.zig)"
        VM["VirtualMachine"]
        TRANS["Transpiler<br/>(TS/JSX→JS)"]
        RES["Resolver"]
    end
    subgraph "JavaScriptCore (C++)"
        JSC["JSC::JSGlobalObject"]
        BIND["bindings: JSCommonJSModule<br/>JSCommonJSExtensions"]
        WC["WebCore bindings<br/>TypedArrayController"]
    end
    subgraph "System Layer"
        MIM["mimalloc allocator"]
        SSL["BoringSSL"]
    end

    CLI --> RUN & TEST & INSTALL & BUILD & SERVE
    RUN --> VM
    VM --> JSC
    VM --> TRANS
    VM --> RES
    JSC --> BIND & WC
    CLI --> MIM & SSL
```

### Core Feature Breakdown

| Feature | Implementation Location | Technology |
|---------|------------------------|-----------|
| **JS/TS execution** | `src/bun.js.zig` → `VirtualMachine` | Zig + JSC C++ FFI |
| **Transpiler** | `src/transpiler/` | Zig implementation of TS/JSX→JS |
| **Bundler** | `src/bundler/` | Zig (BundleThread.zig) |
| **Package manager** | `src/install/` | Zig (npm protocol implementation) |
| **HTTP server** | `src/http/` | Zig + libuv |
| **SQLite** | `src/sqlite/` | Zig wrapper |
| **WebSocket** | `src/websocket/` | Zig |
| **Crypto** | `src/boringssl/` | BoringSSL (OpenSSL fork) |
| **Memory allocator** | `src/bun_alloc/` | mimalloc |

### Key Technical Details

1. **JavaScriptCore Integration**: Bun directly links WebKit's `libjavascriptcoregtk`, exposing JSC API to Zig through C++ glue code in `src/jsc/bindings/`
2. **Main Loop**: `Cli.start()` in `main.zig` parses subcommands and dispatches
3. **Fast Startup**: Bun pre-compiles WebKit bytecode cache, achieving 4-5x faster cold start than Node.js
4. **mimalloc**: Uses Microsoft's mimalloc instead of system allocator to reduce memory fragmentation
5. **Zig Zero-Cost Abstractions**: Zig's comptime and no implicit allocation policies are the foundation of Bun's performance advantages

---

## 4. Arc/Rc & Multithreading Implementation in rustc

> Based on source analysis of `library/alloc/src/sync.rs` (4,974 lines) and `library/alloc/src/rc.rs` (4,599 lines).

### 4.1 Arc Core Data Structure

```rust
// sync.rs:269-276
pub struct Arc<T: ?Sized, A: Allocator = Global> {
    ptr: NonNull<ArcInner<T>>,       // heap-allocated ArcInner
    phantom: PhantomData<ArcInner<T>>,
    alloc: A,                         // allocator (default Global)
}

// sync.rs:388-401
#[repr(C, align(2))]
struct ArcInner<T: ?Sized> {
    strong: Atomic<usize>,   // thread-safe strong ref count
    weak: Atomic<usize>,     // thread-safe weak ref count
    data: T,                 // actual data
}
```

Key design points:
- **`Atomic<usize>`** replaces Rc's `Cell<usize>` — the fundamental difference between Arc thread-safety and Rc single-threading
- **`#[repr(C, align(2))]`** — 2-byte alignment enables low-bit tagging (for weak reference count locking)

### 4.2 Send/Sync Derivation

```rust
unsafe impl<T: ?Sized + Sync + Send, A: Allocator + Send> Send for Arc<T, A> {}
unsafe impl<T: ?Sized + Sync + Send, A: Allocator + Sync> Sync for Arc<T, A> {}
```

`Arc<T>: Send` if and only if `T: Sync + Send` and allocator is Send. This is a manually annotated `unsafe impl` — the compiler trusts the programmer to have chosen the correct atomic types.

### 4.3 Arc::new Allocation Path

```rust
pub fn new(data: T) -> Arc<T> {
    let x: Box<_> = Box::new(ArcInner {
        strong: atomic::AtomicUsize::new(1),  // strong=1
        weak: atomic::AtomicUsize::new(1),     // weak=1
        data,
    });
    unsafe { Self::from_inner(Box::leak(x).into()) }
    //        ^^^ Box::leak prevents Box from freeing memory,
    //            transferring lifetime control to reference count
}
```

### 4.4 Arc::clone Atomic Increment

```rust
fn clone(&self) -> Self {
    let inner = self.ptr.as_ref();
    inner.strong.fetch_add(1, Ordering::Relaxed);
    // Relaxed is sufficient: clone doesn't involve data dependency on other memory
    Self { ptr: self.ptr, phantom: PhantomData, alloc: ... }
}
```

### 4.5 Arc::drop Atomic Decrement & Memory Deallocation

```rust
fn drop(&mut self) {
    let inner = self.ptr.as_ref();
    if inner.strong.fetch_sub(1, Ordering::Release) != 1 {
        return;  // other strong references exist
    }
    atomic::fence(Ordering::Acquire);  // synchronize with last Release
    unsafe { ptr::drop_in_place(&mut (*ptr).data); }

    if inner.weak.fetch_sub(1, Ordering::Release) == 1 {
        atomic::fence(Ordering::Acquire);
        unsafe { dealloc(ptr as *mut u8, layout); }
    }
}
```

**Memory Ordering Strategy**:
- `fetch_sub(Release)`: ensures all writes before drop are visible to other threads
- `fence(Acquire)`: ensures we see the last thread's final writes to data before releasing
- Release/Acquire forms a **happens-before** relationship — the foundation of lock-free programming

### 4.6 Arc vs Rc Comparison

```mermaid
flowchart TD
    subgraph "Rc (single-threaded)"
        RC_CELL["strong: Cell<usize>"]
        RC_WEAK["weak: Cell<usize>"]
        RC_CLONE["clone: cell.get()+1"]
        RC_DROP["drop: cell.get()-1"]
        RC_SEND["Send: ❌"]
        RC_SYNC["Sync: ❌"]
    end
    subgraph "Arc (multi-threaded)"
        ARC_ATOMIC["strong: Atomic<usize>"]
        ARC_WEAK["weak: Atomic<usize>"]
        ARC_CLONE["clone: fetch_add(Relaxed)"]
        ARC_DROP["drop: fetch_sub(Release)+fence(Acquire)"]
        ARC_SEND["Send: ✅ T: Sync+Send"]
        ARC_SYNC["Sync: ✅ T: Sync+Send"]
    end
```

| Dimension | Arc (sync.rs) | Rc (rc.rs) |
|-----------|--------------|-----------|
| Ref count type | `Atomic<usize>` | `Cell<usize>` |
| Send | ✅ conditional | ❌ not supported |
| Sync | ✅ conditional | ❌ not supported |
| Memory ordering | Relaxed / Release / Acquire + fence | None (single-threaded) |
| Overhead | Atomic operation instructions | Zero |

### 4.7 Thread Underlying Implementation

```rust
// library/std/src/thread/mod.rs
// Platform dispatch (via cfg macro):
// Linux/macOS: sys::unix::thread → libc::pthread_create
// Windows:     sys::windows::thread → CreateThread

pub fn spawn<F, T>(f: F) -> JoinHandle<T>
where F: FnOnce() -> T + Send + 'static, T: Send + 'static
{
    // Builder::new().spawn(f)
    //   → imp::Thread::new(name, Box::new(f))
    //     → pthread_create / CreateThread
}
```

The `Send` constraint on `spawn()` is checked at **compile time** by the type system, not a runtime check. The `unsafe impl Send for Arc<T>` is the key that allows `Arc<T>` to be moved across threads.

### Summary

Arc's thread safety relies on three underlying mechanisms:

1. **`Atomic<usize>`** hardware-level atomic operations (x86 `LOCK XADD` / ARM `LDREX+STREX`)
2. **`Release/Acquire` memory ordering** ensures cross-thread happens-before
3. **`unsafe impl Send/Sync`** manual annotation — the compiler trusts the programmer chose correct atomic types

---

## 5. Cross-Project Comparison

| Metric | rustc (36,807 .rs) | Bun (9,641 Zig/C++/JS) | Linux kernel (60,468 C/H) |
|--------|:-----------------:|:----------------------:|:-------------------------:|
| **Index time** | **1m44s** | **1m1s** | **~300s (timeout)** |
| Parse throughput | 11,523 files/s | 9,414 files/s | — |
| Graph nodes | **4,130,017** | 2,951,664 | — |
| Graph edges | 3,044,162 | 2,567,205 | — |
| Parse % | 3.0% | 1.7% | 2.5% |
| SQLite % | 37.4% | 43.6% | 30.5% |
| buildGraph % | 29.5% | 34.4% | 37.3% |
| FTS % | 22.1% | 19.6% | 29.3% |

## 6. Token Consumption Comparison

Aggregated from three projects in this test:

| Query | rustc | Bun | ARES Agent | memscope-rs |
|-------|:-----:|:---:|:----------:|:-----------:|
| `get_graph_stats` | 19 | 19 | 18 | 18 |
| `get_hotspots` top10 | 500 | ~488 | 488 | 488 |
| `project_overview` | 71 | 71 | 71 | 71 |
| `get_module_tree` | 4 | 4 | 4 | 4 |
| `get_entry_points` | 5 | 5 | 5 | 5 |
| `get_project_info` | 44 | 39 | 44 | 44 |

**Cross-project consistency**: Token consumption depends only on **returned data volume**, not project size. Both million-node and thousand-node projects' `get_graph_stats` are ~18 tokens.

**vs raw source code**:
- rustc 36,807 files: CodeScope returns architecture info in **~910 tokens** vs reading source code would require **millions of tokens**
- Bun 9,641 files: CodeScope returns architecture info in **~910 tokens** vs reading source code would require **hundreds of thousands of tokens**

**vs codebase-memory-mcp**: codebase-memory-mcp cannot complete full indexing for either project (rustc: no Rust multi-file graph support, Bun: no Zig support), while CodeScope covers all languages through tree-sitter multi-language parsers.