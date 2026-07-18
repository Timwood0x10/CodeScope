# Code Review — 未提交改动 (commit `a97d0ca`) + `codescope-*.sh` 脚本

> 范围:本次 review 仅针对 (1) 提交 `a97d0ca`（"resolve multiple critical bugs and improve code parsing"，含 15 个源码文件 + `codescope-parallel.sh` 重写）与 (2) `codescope-index.sh`（`b62e633`）。
> 方法:逐条重读修复点源码核实是否真正修对;重读两个脚本全文找健壮性 bug。**未修改任何文件。**

---

## 一、结论速览

| 对象 | 结论 |
|------|------|
| 源码 bug 修复（#2/#3/#4/#5/#7） | ✅ **全部修对**，且未引入新的正确性问题 |
| `codescope-parallel.sh` | ⚠️ 有 1 个 **HIGH** bug，会让"按文件隔离崩溃文件"的招牌容错功能**完全失效** |
| `codescope-index.sh` | ⚠️ 同样有该 **HIGH** bug + 路径过滤过宽会**误删合法文件** |
| 发布打包 | ⚠️ 当前 release 只带 `codescope-parallel.sh`，**漏带 `codescope-index.sh`** |
| "狂暴模式" | 💡 好想法，但有前置依赖（见第四节），需你授权后再实现 |

---

## 二、源码修复核实（逐条）

### ✅ #2 resolver arity 硬编码 0 — 已修，链路完整
- `pipeline.cpp:439` `ref_sql` 选 `r.arity`（列 3）。
- `pipeline.cpp:526` `r.arity = sqlite3_column_int(ref_st, 3)` 读出。
- `pipeline.cpp:675` `applyConstraints(..., ref.call_kind, ref.arity)` 传入。
- `pipeline.cpp:222` `factorSignatureMatch(caller_arity, c.arity)` 使用真实调用点 arity。
- 调用点硬编码 `0` 已删除，重载排名为反的 bug 真正修正。

### ✅ #3 知识图谱 `total` 永远 0 — 已修
- `engine_ffi.cpp:246` `total++` 在结果循环**内部**；`:249-250` 循环后正确输出 `"total":` 与 `truncated`。原"循环后才声明 `int64_t total=0`"已消除。
- 附带修复：`:241` 文本列经 `jsonEscape` 转义，顺带堵住了 JSON 注入。

### ✅ #4 capability_drift 精确名匹配 → 全误报 drift — 已修
- `capability_drift.cpp:36-42` 改为双向前缀 `LIKE`：`(LOWER(e.name) LIKE LOWER(?)||'%' OR LOWER(?) LIKE LOWER(e.name)||'%')`。原 `e.name=?` 已删除，README 派生名与代码符号双向能匹配，不再全误报。

### ✅ #5 capability_verifier 匹配方向反 → 真实实现判 Contradicted — 已修
- `capability_verifier.cpp:102-109` `entitiesWithCallers` 同样改为双向前缀 `LIKE`，注释明确点名了上一版"单方向翻转仍错"的修复。验证声明卖点的正确性恢复。

### ✅ #7 CSR 把 64 位 id 压进 `uint32_t` — 已修（写入侧 + 全仓）
- `store_graph.cpp:868` `buf` 为 `std::vector<uint64_t>`；`:873-874` `src/tgt` 以 `int64_t` 读出；`:900` `buf.push_back(static_cast<uint64_t>(tgt))`。截断已消除。
- 全仓 `grep "static_cast<uint32_t>(...id|node|tgt|...)"` **零命中** → 读取侧也无残留截断。#7 彻底修净。

### 源码修复是否引入新问题？
- `capability_drift.cpp`/`capability_verifier.cpp` 用 `SQLITE_STATIC` 绑定 `cap_name`/`subject`（均为函数参数引用，生命周期覆盖 `stmt` 且 `stmt` 在函数内 finalize）→ **安全**。
- 双向 `LIKE` 为每行 O(1)，无性能回归。
- 结论：源码修复干净，无回归。

---

## 三、脚本审查（`codescope-parallel.sh` / `codescope-index.sh`）

### 🔴 HIGH — `|| true` 吞掉真实退出码，隔离容错功能整体失效（两个脚本都有）

**根因**：`cmd ... || true` 之后紧跟 `local ec=$?`。在 bash 中 `|| true` 的退出码恒为 0，因此 `ec` 永远 0，后续所有基于 `ec` 的崩溃判断形同虚设。

**`codescope-parallel.sh`**
- `:359` `env ... timeout 120 "$CODESCOPE_BIN" worker ... >/dev/null 2>&1 || true` → `ec` 恒 0。
- `:362` `if [ "$ec" -eq 0 ] || [ "$ec" -eq 124 ]; then left=$((mid+1)); ...` → 恒真 → 二分查找**永远只移动 left**，`crash_file` 永远设不上 → `find_crashing_file` 返回空，**按文件隔离从未触发**。
- `:403-405` `env ... timeout 600 "$CODESCOPE_BIN" worker ... > "$module_log" 2>&1 || true` → `ec` 恒 0。
- `:566` 仅 `nodes -eq 0` 才判失败；一个**崩溃但产出 ≥1 节点**的模块会被误报为 SUCCESS。

**`codescope-index.sh`**
- `:118-127` 同样 `|| true` + `local ec=$?` → `ec` 恒 0；`:131` 崩溃但非零节点的批次误报成功。
- `:178-180` 二分查找内同样 `|| true` + `test_ec=$?` → `:183` 恒真 → **二分查找找不到崩溃文件**，`:196` 的隔离清退永远执行不到。

**影响**：两个脚本的招牌特性——"单文件崩溃自动隔离、其余照常索引"——**目前完全不工作**。崩溃文件要么导致整个模块/批次失败，要么被静默当成成功。

**修复（两处共 4 个点，改动极小）**：
```bash
# 错误写法
cmd ... || true
local ec=$?

# 正确写法（捕获真实退出码且不被 set -e 中断）
local ec=0
cmd || ec=$?
```
这是 bash 下"既要拿退出码、又不能被 `set -e` 杀掉"的标准惯用法。

---

### 🟠 MEDIUM — `codescope-index.sh` 路径过滤过宽，误删合法文件
- `:63` `grep -v -E "(/\.git/|/node_modules/|/target/|/build/|/venv/|/__pycache__/|/\.)"` 把子串匹配到路径**任意位置**，而非仅目录分量。
- 反例：`src/builders/build_util.c` 含 `/build/` → 被排除；`src/mytargets/target_x.c` 含 `/target/` → 被排除。这些**合法源码被静默漏索引**。
- 修复：把过滤锚定到目录边界，例如
  `grep -v -E '/((\.git|node_modules|target|build|venv|__pycache__)(/|$)|[.][^/]*/)'`，
  或直接复用 C++ 侧 `discover`（它已正确跳过目录，且与 `codescope-parallel.sh` 一致）。

### 🟠 MEDIUM — 合并假设 id 空间互不重叠（`INSERT OR IGNORE`）
- `codescope-parallel.sh:649-653` 合并各模块 DB 时用 `INSERT OR IGNORE INTO entity/relation/graph_nodes/graph_edges SELECT * FROM other`。
- 若各模块 DB 的实体 id 从 1 重新编号（per-DB），第二个模块的相同 id 行会被**静默丢弃** → 丢节点/边。
- 需确认 `worker` 是否分配全局唯一 id；否则合并前必须重编号。建议在合并前打印各 DB 的 `MAX(id)` 来核实是否有重叠。

### 🟠 MEDIUM（待确认）— rebalance 重启同一模块 DB，可能重复计数
- `codescope-parallel.sh:519-547` rebalance 杀掉旧进程后，用更多 worker **重写同一 `module_db`**。若 `worker` 对已有 DB 是追加而非替换，节点会被翻倍。取决于 `worker` 语义，建议确认。

### 🟡 LOW — `codescope-parallel.sh:308` 空 `total_files` 在 `set -e` 下崩
- `[ "$TOTAL_FILES" -eq 0 ]`：若 `json_get` 返回空（discover 缺字段），`-eq` 遇空串抛整数错误 → 脚本中止。建议 `TOTAL_FILES=${TOTAL_FILES:-0}` 并校验数字。

### 🟡 LOW — rebalance 可无限超订 worker
- 每次 rebalance 把全部 `AVAILABLE_WORKERS` 加给最大模块，worker 数可超过核数，仅影响吞吐不崩。

### 🟡 LOW — 空项目 `split -l 0`（`codescope-index.sh`）
- 若 `TOTAL=0`，`FILES_PER_BATCH=0`，`split -l 0` 报错（`set -e` 下中止）。建议 `FILES_PER_BATCH` 下限置 1。

### 🟡 LOW — 二分查找把超时(124)当成功
- `:359`/`:178` 用 `timeout 120` 跑二分，而真实索引 `timeout 600`；慢文件在两轮超时阈值不一致下可能被误判。且把 124 当"无崩溃"会漏掉慢崩文件。

### ℹ️ INFO — 依赖不一致
- `codescope-index.sh` 硬编码要求 `python3`（`:17` 默认值 + `index_batch` 内用 python）；`codescope-parallel.sh` 支持 `python3` 或 `jq`。建议统一。

---

## 四、关于"发布带上脚本 + 狂暴模式"（仅评估，未实现）

**现状**：`_ci.yml` 打包只 `cp codescope-parallel.sh`，`install.sh` 也只 `chmod` 它 → **`codescope-index.sh` 当前不进 release**。要让用户两个都能用，需把 `codescope-index.sh` 也加进 `_ci.yml` 拷贝 + `install.sh` 赋权。

**狂暴模式（opt-in 加速）设计建议**：
- 新增显式开关，默认关：`--berserk` 或 `CODESCOPE_BERSERK=1`。
- 开启后牺牲安全性换速度，例如：跳过"按文件隔离"二分重试（直接放弃崩溃模块）、并发数突破核数、关闭 metrics 监控、加大批大小、去掉 worker 超时余量、可选关 FTS。
- **重要前置依赖**：当前脚本的隔离逻辑因上面 HIGH bug 本就失效，所以"狂暴模式跳过隔离"在修 bug 前是**空操作**。建议**先修 HIGH bug**（让正常模式容错恢复工作），再上狂暴模式才有意义。
- 狂暴模式必须在 banner 里显著警告"可能丢文件/结果不完整"，且**绝不默认开启**。

> 这部分需要你授权后我才会动手（改 `_ci.yml` / `install.sh` / 两个脚本加 `--berserk`）。

---

## 五、建议优先级

1. **先修脚本 HIGH bug**（`|| true` → `|| ec=$?`，4 处）—— 恢复容错招牌功能，改动极小、零风险。
2. **修 MEDIUM**：路径过滤锚定目录边界；核实合并 id 空间 / worker 重写语义。
3. 顺手 LOW：空值/空项目/超时一致性。
4. **最后**再做 release 带双脚本 + 狂暴模式（需你点头）。

源码侧（a97d0ca）无需返工——修复都已验证正确。
