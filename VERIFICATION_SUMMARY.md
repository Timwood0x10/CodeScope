# codescope 二进制验证汇总

> 日期：2026-08-01
> 测试对象：用户新构建的 codescope 二进制（`bin/codescope`，2026-08-01 11:44 构建，13.9MB，静态 rpath）
> 对比基线：官方 release v0.2.4（调用图/验证管道全挂）

## 一、部署修复（v0.2.4 → 新二进制）

### 1. 代码签名（根因）
- **症状**：服务启动即被杀，crash report 标记 `Code Signature Invalid`，内核 SIGKILL。
- **根因**：macOS 按页校验签名，签名失效的二进制/动态库被直接杀掉。
- **修复**：`codesign --force --sign - <binary>`，同时对 `liblbug.0.dylib` 做相同处理。
- **注意**：以后每次替换/重编二进制后若出现秒死 SIGKILL，先重新签名。

### 2. dylib rpath 目录缺失
- 二进制 rpath 指向 `@executable_path/../engine/third_party/ladybug/lib/macos` 与
  `@executable_path/../../engine/third_party/ladybug/lib/macos`，本机两处都不存在。
- **修复**：手动建立符号链接
  `~/.codescope/engine/third_party/ladybug/lib/macos/liblbug.0.dylib → ~/.codescope/bin/liblbug.0.dylib`
- 实际加载 dylib 版本：0.18.2。删除符号链接后服务以 rc=-6（dyld 缺库）退出。

### 3. 驱动方式
MCP stdio + JSON 行协议，`CODESCOPE_DB_PATH` 指向各项目 `.codescope/codescope.db`。

## 二、Go 项目测试（/Users/scc/go/src/goagent，1387 文件）

| 功能 | v0.2.4（旧） | 新二进制 |
|---|---|---|
| 索引规模 | 11578 节点 / 2736 调用边 | **12366 节点 / 5858 调用边**（翻倍） |
| `trace_flow` | 空 callees | ✅ 递归解析（emitToolEvent → Warn → attrs） |
| `find_callers` | 全 0 | ✅ 自由函数可解析：`emitToolEvent` → `executeToolCalls`（conf 0.85, exact_local） |
| `find_callers`（接口方法） | 0 | ⚠️ 返回 9 个候选需按 entity 消歧；经接口分派的 caller 仍为 0（Go 动态分派固有局限） |
| `verify_claim` | "no verifier registered" | ✅ **Supported / 0.8 + 真实证据边**（FunctionImplementsVerifier） |
| verifier 注册表 | 空 | ✅ 4 个 verifier（Capability/Contract/Architecture/FunctionImplements），evidence_backend_ready |

## 三、Rust 项目测试（/Users/scc/code/rustcode/memscope-rs，221 文件）

| 测试 | 结果 |
|---|---|
| 索引 | ✅ 6740 节点 / 3197 调用边 |
| `trace_flow init_logging` | ✅ 递归 callees（init → init_global_tracking → error） |
| `find_callers init_logging` | ✅ 2 个真实调用者（guard.rs:151 `start_with` / mem_ctx.rs:25 `init`，conf 0.85 exact_local） |
| verifier 注册表 | ✅ 4 个 verifier，evidence_backend_ready |
| `verify_claim`（正确断言） | ✅ Supported / 0.8 + 证据边 |
| `verify_claim`（不存在函数） | ✅ Contradicted / 0.85（负例正确拒绝） |

**结论：codescope 对 Go / Rust 支持同等完整，调用图解析与验证管道对两种语言均真实可用。**

## 四、已知局限（诚实记录）

### 1. FunctionImplementsVerifier 只做存在性检查，不校验语义
故意传入错误断言 `init_logging implements TCP_server` 仍返回 **Supported**（0.8）——
verifier 忽略 object 字段，只确认函数存在且有调用边。即：
- ✅ 正例可信（函数确实存在并实现行为）
- ❌ **无法发现"语义上错误"的断言**（存在但功能不匹配）

### 2. Go 接口方法分派
接口方法（如 `AfterStep`）经 interface 调用的 caller 解析不到——调用点在抽象层，
需要按 entity 消歧。属 Go 动态分派固有难题，非本次二进制回归。

### 3. 展示层 `ready_features`
`project_overview.ready_features.call_graph / metrics / semantic_search` 恒为 false，
但实际调用边已构建（`get_graph_stats.total_edges > 0`）——显示状态与真实数据不一致。

### 4. `verify_statement`（旧 API）仍返回 Unknown
新版推荐使用 `verify_claim`。

## 五、结论
- 新二进制**修复了调用图解析与验证管道**，两项核心能力（find_callers / verify_claim）从"不可用"变为"真实可用"。
- 语义搜索（embedding）仍需独立的 `enhance_project` / embedding 步骤，本次未覆盖。
