# CodeScope Architecture (3): Worker Isolation — Why Indexing Won't Crash Your MCP Server

> The original version didn't have Worker subprocesses. The Server called engine functions directly.
> Then one day, indexing a large project, RSS hit 3.5 GB and the MCP Server OOM'd. Claude Desktop showed "tool call timeout" with no error message.
> It took me three days to find the cause — not a memory leak, but thread stacks that were too large.
>
> From that day on I realized: **in a long-running Server process doing heavy computation, either isolate the computation or be prepared to be dragged down by it.**

## Series Index

| # | Title | One-liner |
|:--:|------|-----------|
| 1 | [Introduction](codescope-architecture-01-intro.md) | Why rewrite a code understanding tool |
| 2 | [Progressive Readiness](codescope-architecture-02-progressive-readiness.md) | Millisecond-level code understanding |
| **3** | **Worker Isolation** (this article) | Why indexing won't crash your MCP Server |

## I. The Problem: Heavy Computation Can't Run in the Server Process

An MCP Server is a long-running process. Its job is to:
1. Receive JSON-RPC requests
2. Route to the correct tool
3. Return JSON-RPC response

It should not do heavy computation. But source code indexing IS heavy computation — reading hundreds of files, parsing ASTs, building graphs, writing to DB.

If all this happens in the Server process, problems arise:

### 1.1 Memory Pollution

C++ engine uses arena allocators and various local buffers. After indexing a project, this memory isn't 100% returned to the OS — C++'s free list retains some fragments. The Server process's RSS only grows.

### 1.2 Thread Stack Issues

The original version allocated **256MB stacks** per worker thread. 14 threads × 256MB = 3.5GB. This isn't physical memory — it's virtual address space reservation — but it still stresses the OS.

```
14 threads × 256MB stack = 3.5 GB virtual address space
        ↓ after fix
14 threads × 8MB stack = 112 MB virtual address space
        ↓ 256MB → 8MB is a 32x reduction
```

I discovered this bug in an embarrassing way — I started by looking for memory leaks with valgrind, found nothing, then used `/usr/bin/time -l` to check peak RSS, and realized it was thread stacks.

### 1.3 Long Hangs

Indexing Linux Kernel takes minutes. If indexing runs in the main process, the MCP Server can't respond to any other requests during those minutes — including `get_index_progress` polling. The AI sees "tool unresponsive."

---

## II. Solution: Worker Subprocess

The solution is straightforward: **put indexing in a subprocess. When it finishes, the subprocess exits and RSS is fully returned to the OS.**

### 2.1 Architecture

```
┌─────────────────────────────────────────────┐
│ Rust MCP Server (long-running, ~12MB RSS)    │
│                                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │ Tool     │  │ FFI      │  │ Tokio    │  │
│  │ Dispatch │  │ Bridge   │  │ Scheduler│  │
│  └──────────┘  └──────────┘  └──────────┘  │
│         │            ▲            │          │
│         ▼            │            ▼          │
│  ┌─────────────────────────────────────────┐ │
│  │         Subprocess Manager               │ │
│  │  spawn + pipe stdout + timeout + reap    │ │
│  └─────────────────────────────────────────┘ │
│         │                                    │
│         ▼  spawn (RSS 0 → 3GB → 0)          │
│  ┌─────────────────────────────────────────┐ │
│  │  C++ Worker (short-lived, a few min)    │ │
│  │  scan → parse → build → write → exit    │ │
│  └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

### 2.2 Process Lifecycle

1. Server spawns Worker subprocess via `fork() + execvp()`
2. Worker receives project path via command-line argument
3. Worker scans, parses, builds graph, writes to DB
4. Worker writes JSON result to stdout
5. Server reads stdout via pipe
6. Worker exits, RSS fully returned to OS (confirmed by `/usr/bin/time -l`)

### 2.3 Timeout Protection

```cpp
// server/src/ffi/worker.rs
let child = Command::new(worker_path)
    .arg(project_path)
    .stdout(Stdio::piped())
    .stderr(Stdio::piped())
    .spawn()?;

// 300s timeout + 3 retries
let result = tokio::time::timeout(
    Duration::from_secs(300),
    child.wait_with_output()
).await;
```

If the Worker doesn't complete within 300 seconds, it's killed. The Server retries up to 3 times. If all retries fail, the error is returned to AI.

---

## III. Cost: Serialization Overhead

Worker isolation isn't free. The cost is serialization.

### 3.1 What Needs to Be Serialized

Arguments passed via command line:
- `project_id` (uint64): 8 bytes
- `project_path` (string): N bytes
- `db_path` (string): N bytes

Result returned via stdout JSON:
- `symbols` count, `files` count, `errors` array, etc.

### 3.2 What Doesn't Need Serialization

The Worker and Server share the SQLite database file. The Worker writes, the Server reads — no data copy needed.

### 3.3 Measured Overhead

Process spawn overhead is ~2-5ms on Linux/macOS (measured with 100 iterations). This is negligible compared to indexing time (seconds to minutes).

---

## IV. Trade-offs Summary

| Aspect | Thread Model | Worker Subprocess |
|--------|:------------:|:-----------------:|
| Memory isolation | Poor — RSS never returns | **Perfect** — 100% returned on exit |
| Crash isolation | Poor — one thread brings down all | **Perfect** — Worker crash ≠ Server crash |
| Spawn overhead | None | **~2-5ms** |
| Data sharing | Direct memory access | Via SQLite file |
| Complexity | Lower | Higher (IPC, timeout, retry) |

For an MCP Server that needs to stay responsive for hours or days, Worker isolation is clearly the right choice.
