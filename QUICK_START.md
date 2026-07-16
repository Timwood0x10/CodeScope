# CodeScope 快速开始指南

> 5 分钟从零开始索引你的第一个项目

## 一、安装

### 方式 1：一键构建（推荐）

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/bootstrap.sh)
```

脚本会自动检测你的操作系统，安装缺失的依赖（Xcode、Homebrew、LLVM、cmake、Rust），然后编译 CodeScope。

### 方式 2：下载预编译二进制

```bash
# macOS ARM64
curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/install.sh | bash

# Windows PowerShell
irm https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/install.ps1 | iex
```

### 方式 3：手动构建

```bash
# 1. 安装系统依赖
# macOS:
brew install llvm@21 cmake pkg-config sqlite3 ladybug

# Linux (Ubuntu):
sudo apt-get install -y build-essential cmake llvm-dev libclang-dev libsqlite3-dev
curl -fsSL https://install.ladybugdb.com | sh

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
# 索引整个项目
./target/release/codescope cli index_project '{"project_path":"/path/to/your/project"}'

# 或者用并行索引脚本（更快，适合大项目）
CODESCOPE_WORKERS=8 ./codescope-parallel.sh /path/to/your/project
```

## 四、启动 MCP 服务器

```bash
# 启动 MCP 服务器（MCP 协议使用 stdio 通信，无端口）
./target/release/codescope

# 指定数据库路径（通过环境变量，无 --db-path 参数）
CODESCOPE_DB_PATH=/path/to/codescope.db ./target/release/codescope
```

## 五、支持的索引模式

| 模式 | 速度 | 用途 | 命令 |
|:----|:----:|:-----|:----|
| 快速 | 最快 | 日常开发，快速查询 | `CODESCOPE_INDEX_MODE=fast` |
| 标准 | 中等 | 默认，平衡速度和精度 | 默认 |
| 严格 | 最慢 | 生产环境，完整验证 | `CODESCOPE_INDEX_MODE=strict` |

## 六、支持的语言

| 语言 | 状态 | 说明 |
|:----|:----:|:-----|
| Rust | ✅ 稳定 | 所有项目测试通过 |
| Go | ✅ 稳定 | 标准库 54/54 模块通过 |
| Java | ✅ 可用 | JDK 56/69 模块通过 |
| C/C++ | ✅ 稳定 | tree-sitter C grammar SIGILL 已通过 -O0 编译修复 |
| Python | ✅ 稳定 | 基础支持 |
| JavaScript/TypeScript | ✅ 稳定 | 基础支持 |

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

- [架构文档](docs/zh/architecture.md) — 了解 CodeScope 的工作原理
- [优化文档](docs/optimization/optimization-chinese.md) — 性能优化记录
- [性能报告](docs/zh/e2e_benchmark_report.md) — 大规模项目索引测试