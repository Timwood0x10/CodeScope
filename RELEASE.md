# CodeScope v0.2.0

> **Project Truth Engine** — 把源码变成可验证的事实，让 AI 基于项目真相回答问题。

***

## What's New in v0.2.0

### 🔧 Build System Overhaul

- **一键构建**：新增 `bootstrap.sh` — 自动检测 macOS/Linux，安装缺失依赖，编译。一条命令完成：`bash <(curl -fsSL ...)/bootstrap.sh`
- **跨平台 LadybugDB**：CMake 现在在 macOS（Homebrew）和 Linux（`/usr/local`）上都能自动找到 LadybugDB
- **CI 修复**：GitHub Actions 现在在 Ubuntu 和 macOS 上都安装 LadybugDB，CI 编译通过
- **快速开始指南**：新增 `QUICK_START.md` — 5 分钟从零开始上手

### 🐛 Bug Fixes

- **FilterPolicy `"tools"` 误杀**：`normal_skip_dirs_` 中的 `"tools"` 条目会匹配任何路径中的 `tools` 目录，导致 JDK 所有 `jdk/tools/xxx/...` 路径下的文件被过滤。已修复：改为前缀匹配 `"tools-"` / `"tools_"`，加入 `skip_dir_prefixes_`
- **quarantine 阶段死循环**：`codescope-parallel.sh` 的 binary search 在 0 节点模块上陷入无限循环（10 次重试后跳过，但超时模块多时总时间爆炸）

### 📊 Performance

- **文件级并行索引**：支持 `--file-list` 参数，通过 JSON 文件传递文件列表，避免 ARG_MAX 限制
- **分模块并行索引 v3**：动态 worker 调度，小模块释放 worker 给大模块

### 🧪 Multi-Language Indexing Test Results

| Project | Language | Files | Success | Nodes | Time |
|---------|----------|:-----:|:-------:|:-----:|:----:|
| wasmtime | Rust | 2,145 | **12/12** | 18,961 | **40s** |
| CodeScope | C++/Rust | 1,860 | **2/2** | 329 | **<1s** |
| Go stdlib | Go | 7,018 | **54/54** | 115,655 | **42s** |
| tinygo | Go | 1,230 | **16/18** | 25,688 | **17s** |
| JDK | Java | 20,137 | **56/69** | 205,842 | **52s** |
| Linux kernel | C | 60,650 | **21/24** | 205,728 | **96s** |

### 📚 Documentation

- `QUICK_START.md`：中文快速开始指南
- `README.md` / `README_CN.md`：更新构建说明
- `docs/optimization/`：性能优化记录

***

## Quick Start

```bash
# One-command build
bash <(curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/bootstrap.sh)

# Index a project
codescope cli index_project '{"project_path":"/path/to/project"}'

# Start MCP server
codescope
```