# CodeScope 快速开始指南

> 5 分钟从零开始索引你的第一个项目

## 一、安装

### 方式 1：一键构建（推荐）

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/main/bootstrap.sh)
```

脚本会自动检测你的操作系统，安装缺失的依赖（Xcode、Homebrew、LLVM、cmake、Rust），然后编译 CodeScope。

### 方式 2：下载预编译二进制

```bash
# macOS ARM64
curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/main/install.sh | bash

# Windows PowerShell
irm https://raw.githubusercontent.com/Timwood0x10/CodeScope/main/install.ps1 | iex
```

### 方式 3：手动构建

```bash
# 1. 安装系统依赖
# macOS:
brew install llvm@21 cmake pkg-config sqlite3

# Linux (Ubuntu):
sudo apt-get install -y build-essential cmake llvm-dev libclang-dev libsqlite3-dev

# 2. 安装 Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# 3. 构建
git clone https://github.com/Timwood0x10/CodeScope.git
cd CodeScope
cargo build --release
```

## 二、快速验证

```bash
# 查看二进制
./target/release/codescope --help

# 扫描一个项目，看看能发现多少源码文件
./target/release/codescope discover /path/to/your/project
```

## 三、索引项目

```bash
# 索引整个项目（按模块派生子进程，内存相互隔离）
./target/release/codescope index-parallel /path/to/your/project

# 大项目可调参：--workers 解析核心总数（默认 8）、--parallel 并发模块 worker 上限（默认 4）
./target/release/codescope index-parallel /path/to/your/project --workers 8 --parallel 4
```

## 四、启动 MCP 服务器

```bash
# 启动 MCP 服务器（MCP 协议使用 stdio 通信，无端口）
./target/release/codescope

# 指定数据库路径（通过环境变量，无 --db-path 参数）
CODESCOPE_DB_PATH=/path/to/codescope.db ./target/release/codescope
```

## 五、支持的索引模式

| 模式 | `CODESCOPE_INDEX_MODE` | 作用 |
|:----|:----|:-----|
| fast | `fast` | 发现阶段额外剪枝（11 类目录 + 4 类缓存文件），且不构建 FTS |
| normal | `normal`（默认） | 平衡的发现策略 + 完整管线 |
| deep | `deep` | 同 `normal`，但重建 n-gram 语义向量 |
| strict | `strict` | **仅**影响发现：对识别出的语言加白名单 gate，不改动解析管线 |

## 六、支持的语言

| 语言 | 扩展名 | 状态 |
|:----|:------|:-----|
| Python | `.py` | 已内置语法，端到端解析 |
| Go | `.go` | 已内置语法 |
| Rust | `.rs` | 已内置语法 |
| JavaScript | `.js`, `.mjs` | 已内置语法 |
| TypeScript | `.ts` | 已内置语法 |
| TSX | `.tsx` | 已内置语法 |
| C | `.c`, `.h` | 已内置语法（以 `-O0` 编译，见下方 SIGILL 说明） |
| C++ | `.cpp`, `.cc`, `.cxx`, `.hpp`, `.hxx` | 已内置语法 |
| Java | `.java` | 已内置语法 |

`.kt`/`.kts`、`.rb`、`.scala`、`.swift` 会按扩展名被识别，但没有内置语法：它们会在
`parse_failures` 中记录为 `language_missing`（用 `codescope parse-failures` 查看），
且不计入 fail-fast 跳过次数。

## 七、常见问题

### Q: 构建失败，提示找不到 LLVM

确保 LLVM 在 PATH 中：

```bash
# macOS Homebrew
export PATH="/opt/homebrew/opt/llvm@21/bin:$PATH"
export LDFLAGS="-L/opt/homebrew/opt/llvm@21/lib"
export CPPFLAGS="-I/opt/homebrew/opt/llvm@21/include"
```

### Q: 索引 C 项目时 SIGILL 崩溃

这是 tree-sitter C grammar v0.24.2 在 Apple Silicon 上的已知问题。CodeScope 已通过将 C grammar 编译为 `-O0` 修复此问题，正常使用不会触发。如果仍遇到 SIGILL，请确认使用的是最新构建（`cargo build --release` 重新编译）。

### Q: 如何配合 AI 使用？

CodeScope 实现 MCP 协议，可以直接集成到 Claude Desktop、Cursor 等 AI 工具中：

```json
{
  "mcpServers": {
    "codescope": {
      "command": "/path/to/codescope"
    }
  }
}
```

## 八、下一步

- [架构文档](architecture.md) — 了解 CodeScope 的工作原理
- [优化文档](../optimization/optimization.zh.md) — 性能优化记录
- [性能报告](../optimization/perf-full-index-2026-08-11.zh.md) — 全量索引性能统计
