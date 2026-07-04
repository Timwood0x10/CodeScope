# Token Savings Report / Token 节省报告

> CodeScope vs. reading raw source files — measured across real analysis scenarios.
> CodeScope 对比直接读取源文件——基于真实分析场景的实测数据。

---

## English

### Why Token Savings Matter

When an LLM needs to understand code, it has two options:
1. **Read raw source files** — thousands of lines of code, mostly irrelevant context
2. **Query CodeScope** — get exactly the structured knowledge needed (symbols, graphs, metrics)

CodeScope reduces token consumption by ~98.9% on average.

### Measured Savings by Scenario

| Scenario | CodeScope (tokens) | Raw Source (tokens) | Savings |
|----------|-------------------|---------------------|---------|
| Find function definition | ~21 | ~2,265 | **99.1%** |
| Trace callers of a function | ~18 | ~2,000 | **99.1%** |
| Project architecture overview | ~32 | ~1,875 | **98.3%** |
| Function complexity analysis | ~43 | ~4,733 | **99.1%** |
| Symbol search by name | ~23 | ~958 | **97.6%** |
| USB driver subsystem overview | ~250 | ~24,000 | **99.0%** |
| Linux kernel scheduler analysis | ~180 | ~15,000 | **98.8%** |
| Parent-child process (COW) trace | ~85 | ~8,500 | **99.0%** |
| **Average** | **~81** | **~7,416** | **98.9%** |

### Real-World Benchmarks

| Analysis | CodeScope Output | Raw Source | Saved |
|----------|----------------|------------|-------|
| Fast Scan `drivers/usb/` (37,286 symbols) | ~2,000 tokens | ~3.2 MB code | ~99.9% |
| Fast Scan `kernel/sched/` (4,913 symbols) | ~800 tokens | ~1.1 MB code | ~99.9% |
| Fast Scan `mm/` (16,111 symbols) | ~1,200 tokens | ~1.8 MB code | ~99.9% |
| `find_symbol("usb_register")` | ~30 tokens | ~400 KB headers | ~99.9% |
| `trace_path(copy_process, dup_mm)` | ~50 tokens | ~8,500 lines fork.c | ~99.4% |
| `build_context("USB init")` | ~500 tokens | ~3.2 MB code | ~99.98% |

### Why It Works

CodeScope pre-indexes the codebase into a structured knowledge base (SQLite with 11 tables). When an LLM asks a question, CodeScope returns only the **relevant facts** — not the entire source file. The architecture is designed for this:

```
Source Code → CodeScope (Facts + Index + Graph) → LLM gets only relevant context
                                                                 ↓
                                                    ~98.9% fewer tokens
```

---

## 中文

### 为什么 Token 节省很重要

当 LLM 需要理解代码时，有两个选择：
1. **读取原始源文件**——数千行代码，大部分是无关上下文
2. **查询 CodeScope**——精确获取需要的结构化知识（符号、图、指标）

CodeScope 平均减少 **~98.9%** 的 token 消耗。

### 各场景实测节省

| 场景 | CodeScope (tokens) | 原始源码 (tokens) | 节省 |
|------|-------------------|------------------|------|
| 查找函数定义 | ~21 | ~2,265 | **99.1%** |
| 追踪函数调用者 | ~18 | ~2,000 | **99.1%** |
| 项目架构概览 | ~32 | ~1,875 | **98.3%** |
| 函数复杂度分析 | ~43 | ~4,733 | **99.1%** |
| 符号名称搜索 | ~23 | ~958 | **97.6%** |
| USB 驱动子系统概览 | ~250 | ~24,000 | **99.0%** |
| Linux 内核调度器分析 | ~180 | ~15,000 | **98.8%** |
| 父子进程（写时复制）追踪 | ~85 | ~8,500 | **99.0%** |
| **平均** | **~81** | **~7,416** | **98.9%** |

### 真实基准测试

| 分析 | CodeScope 输出 | 原始源码 | 节省 |
|------|---------------|---------|------|
| Fast Scan `drivers/usb/`（37,286 符号） | ~2,000 tokens | ~3.2 MB 代码 | ~99.9% |
| Fast Scan `kernel/sched/`（4,913 符号） | ~800 tokens | ~1.1 MB 代码 | ~99.9% |
| Fast Scan `mm/`（16,111 符号） | ~1,200 tokens | ~1.8 MB 代码 | ~99.9% |
| `find_symbol("usb_register")` | ~30 tokens | ~400 KB 头文件 | ~99.9% |
| `trace_path(copy_process, dup_mm)` | ~50 tokens | ~8,500 行 fork.c | ~99.4% |
| `build_context("USB init")` | ~500 tokens | ~3.2 MB 代码 | ~99.98% |

### 原理

CodeScope 将代码库预索引为结构化知识库（SQLite，11 张表）。当 LLM 提问时，CodeScope 只返回**相关事实**——而不是整个源文件。整个架构为此设计：

```
源代码 → CodeScope（事实 + 索引 + 图）→ LLM 只获取相关上下文
                                                       ↓
                                          ~98.9% 更少的 token
```

---

## Methodology / 方法论

All measurements taken from real CodeScope runs on Linux kernel v6.13, SQLite amalgamation, and CodeScope itself.  
所有数据来自对 Linux 内核 v6.13、SQLite 合并版和 CodeScope 自身的真实扫描。

**Hardware / 硬件**: Apple M3 Max, 36 GB RAM  
**OS / 系统**: macOS  
**Measurement / 测量方法**: Token count estimated at 4 chars/token for source code, actual JSON output counted from CodeScope responses.  
**估算方式**: 源码按 4 字符/token 估算，CodeScope 输出按实际 JSON 响应计算。
