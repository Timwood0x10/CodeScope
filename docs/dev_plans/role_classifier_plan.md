# Role Classifier 升级计划：从机械路径关键字到多信号融合

## 元信息

| 项目 | 值 |
|------|-----|
| 版本 | v0.2.1 |
| 影响范围 | `module_summary.role` 列的填充逻辑 |
| 涉及模块 | `engine/src/model/state_builder.cpp`（classifier）、`engine/src/store/store_schema.cpp`（migration） |
| 前置 | v0.2.1 已合，`module_summary` 表稳定，`role` 列已存在（migration 已上线） |
| 验证 | `bun` 项目实测 role 分布 + 单元测试 classifier 6 类 |

---

## 一、问题陈述

### 现状（v0.2.1）

`module_summary.role` 由 `state_builder.cpp:30-38` 的 SQL CASE 机械分类，6 类规则全凭**模块路径关键字** + **入出度阈值**：

| Role | 现有规则（机械） |
|------|----------------|
| `example` | 路径含 `/examples/` 或 `/example/` |
| `entry` | 路径含 `/cmd/` |
| `api` | 路径含 `/api/` |
| `tool` | `incoming ≥ 10 AND outgoing ≤ 5 AND total ≤ 20` |
| `business` | `incoming ≥ 5 AND outgoing ≥ 5` |
| `infra` | 其余全部（兜底） |

### 缺陷

1. **路径关键字太脆**——`src/core`、`tests/` 是约定，但别的库未必遵循。某库 core/tracker 真叫 core，另一库 core 是 leftover；路径关键字无语义保证。
2. **role 沦为 utilization 换皮**——若 role 纯靠调用图数值（incoming/outgoing/dead）推，那 `module_summary` 已有的 `utilization`/`dead_entities` 就能近似推出，role 无增量信息量。
3. **`infra` 是垃圾筐**——兜底类不区分「真 infra」「没命中关键字」「度数模式没撞上」，信号丢失。

### 目标

让 role 有**调用图没有的独立信息量**——引入两类新信号：

| 新信号 | 来源 | 已存？ |
|--------|------|--------|
| **`pub` 可见性** | entity 级的 pub/私有标记 | ❌ **未存**——`import.is_pub` 在 import 表，但 entity 表无 visibility 列 |
| **entry-reachable** | `graph_nodes.is_entry_point` + `entry_points` 表 | ✅ 已存 |

`pub` 可见性是关键——它区分「对外接口层」（pub fn 被跨模块调）vs「内部实现层」（私有 fn 只在同模块调），这是调用图数值推不出的。

---

## 二、设计：多信号融合 classifier

### 2.1 schema 改动

#### Migration 1：`entity` 表加 `visibility` 列

```sql
-- store_schema.cpp migration 节，类比 v0.2.1 的 role migration
ALTER TABLE entity ADD COLUMN visibility INTEGER NOT NULL DEFAULT 0;
-- 0 = private（默认）, 1 = pub/public, 2 = protected（Java/C# 兼容预留）
```

Visitor 层（7 个真实 Visitor）在 emit entity 时填 `visibility`：
- Rust: `pub fn`/`pub struct` → 1；`fn`/`struct` → 0
- Go: 大写首字母 exported → 1；小写 → 0
- Python: 无显式 private，`__leading` 双下划线 → 0；其余 → 1（Python 默认 public）
- C/C++: header 中声明 → 1；static 匿名 → 0
- Java: `public` → 1；`private` → 0；`protected` → 2
- JS/TS: `export` → 1；其余 → 0
- Swift: `public`/`open` → 1；`internal`（默认）→ 0

**实现量**：7 个 Visitor 各加一处 `visibility` 填充，类比现有 `resolve_strategy` 的 `setCallStrategy` 模式。

#### Migration 2：`module_summary` 加聚合列（可选，供 classifier 复用）

```sql
ALTER TABLE module_summary ADD COLUMN pub_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE module_summary ADD COLUMN entry_reachable INTEGER NOT NULL DEFAULT 0;
-- pub_count: 该模块下 visibility=1 的 entity 数
-- entry_reachable: 该模块下是否含 is_entry_point=1 的节点
```

这两个列是**物化缓存**，避免 classifier 每次 INSERT 时重 JOIN。也可不加、靠子查询即时算——决策见 §2.3。

### 2.2 classifier 规则（多信号融合）

替换 `state_builder.cpp:30-38` 的 CASE。新规则按**优先级**匹配（命中即停，不再往下）：

| 优先级 | Role | 规则（多信号融合） |
|--------|------|-------------------|
| 1 | `test` | 模块名含 `test`/`tests`/`_test`/`mod tests`（强信号）**或** 模块内全部 entity 无外部调用（incoming=0 且无 entry_reachable）**且** 有 `#[test]`/`#[test]` 属性标记 |
| 2 | `api` | `pub_count > 0 AND incoming ≥ 2×outgoing AND incoming ≥ 3 AND utilization ≥ 0.3`（pub 多、被跨模块大量调、对外接口层；utilization 下限防低利用模块误命中） |
| 3 | `entry` | `entry_reachable = 1`（含 main/init/setup/run/handler 入口） |
| 4 | `core` | `incoming ≥ 10 AND outgoing ≤ incoming×0.8 AND utilization ≥ 0.7 AND pub_count > 0`（被很多模块依赖、自身依赖少、利用率高、有对外接口——中枢） |
| 5 | `utility` | `outgoing ≤ 5 AND pub_count > 0 AND utilization ≥ 0.5`（几乎只被别人调、有 pub 但自身依赖少——工具层） |
| 6 | `business` | `pub_count > 0 AND incoming ≥ 10`（实现层：被多模块调且自己也调多——outgoing 偏高不命中 core/api，但有 pub 有 incoming，明显非 infra） |
| 7 | `dead` | `incoming = 0 AND outgoing = 0`，**或** `dead_entities = total`（冗余/叶节点） |
| 8 | `infra` | 其余（真兜底——没命中上述任何语义规则） |

**v0.2.2 实测调参记录**（bun 项目，240 模块）：
- 首版阈值（api 3×/core 0.5×/utility outgoing≤2）→ infra 77%，缺 core/utility/business
- 放宽（api 2×/core 0.8×/utility outgoing≤5）+ 加 business 类 → infra 45.8%，business 80，dead 44，test 5，utility 1
- `business` 类是实测后回补的——bun 这类 JS/TS 项目大量模块「有 pub、incoming≥10、outgoing 也偏高」既非 core 也非 api，原 v0.2.1 删掉的 business 实测证明该保留

**关键改动**：
- `test` 提到优先级 1——测试层最易识别，且其他规则误把 test 当 dead
- `dead` 从兜底升级为显式类——区分「真冗余」vs「没命中关键字」
- `api`/`core`/`utility` 都引入 `pub_count`——这是调用图数值推不出的独立信号
- 阈值用**比例**（`3×`、`0.5×`）而非绝对数——适应大小不同的项目

### 2.3 实现选择：物化 vs 即时

| 方案 | SQL | 性能 | 维护 |
|------|-----|------|------|
| **A. 物化**（加 pub_count/entry_reachable 列） | classifier JOIN module_summary 直接读 | 快（O(1) 读） | 要写 migration + Visitor 填充逻辑 |
| **B. 即时**（子查询即时算） | classifier JOIN entity + graph_nodes 子查询 | 慢（每模块重 JOIN），但 module_summary 仅 42 行，可接受 | 零 migration，classifier 自洽 |

**选 B**——module_summary 行数少（memscope-rs 42 行），即时算的 JOIN 成本可忽略；避免加列带来的 Visitor 改动 + migration 复杂度。若未来大项目（rustc 数万模块）实测成瓶颈再升 A。

### 2.4 阈值调参

阈值（`3×`、`0.7`、`10`、`0.5×`）是初值，需在 `bun` 项目实测后调：
- 跑 classifier，dump role 分布
- 人工抽检 10 个模块每类，确认标签合理
- 调阈值直到 role 分布符合直觉（core/api 各 5-15%，utility 20-40%，test/dead 视项目而定，infra 兜底应 < 30%）

阈值不写死成常量——放 `config.h` 或 `state_builder.h` 的 `constexpr`，便于调参。

---

## 三、非目标（Non-Goals）

1. **不重构调用图**——role classifier 只读 relation/graph_nodes，不改它们
2. **不引入 LLM 语义判断**——role 靠结构化信号融合，不调 GPT/Claude 判「这模块像不像 core」
3. **不做 role 的用户手动标注**——role 是自动 classifier 产出，不做人工标注/纠正接口
4. **不区分 sub-role**——`api` 不再细分 `api-rest`/`api-graphql`；粒度到 6 类为止
5. **不改 `explain_module` 输出**——role 仍作为 module_summary 的一个字段输出，不变 API

---

## 四、实现步骤

| 步骤 | 文件 | 改动 |
|------|------|------|
| 1 | `store_schema.cpp` migration 节 | 加 `entity.visibility` 列 migration（类比 role migration） |
| 2 | 7 个 Visitor（python/swift/rust/c/js/go/java） | emit entity 时填 `visibility`，类比 `setCallStrategy` |
| 3 | `state_builder.cpp` `buildModuleSummaries` | 替换 CASE 为多信号融合版（§2.2 规则），即時 JOIN entity.visibility + graph_nodes.is_entry_point |
| 4 | `tests/test_bun.cpp` | 加 role 分布断言（core/api/utility/test/dead/infra 各类数） |
| 5 | 实测调参 | bun 项目跑，dump role 分布，调阈值到合理 |

---

## 五、风险与缓解

| 风险 | 缓解 |
|------|------|
| **Visitor 填 visibility 漏语言** | 7 个 Visitor 必须全覆盖；CI 加单测：每个语言 sample 项目跑完，entity.visibility 分布非全 0 |
| **阈值不适配小项目** | 小项目（<50 模块）role 分布可能偏 infra；文档明说「role 在 <50 模块项目上参考价值有限」 |
| **pub 语义跨语言不一致** | Python 默认 public、Rust 默认 private、Go 凭首字母——文档明说各语言 visibility 映射规则 |
| **classifier 回归** | 保留旧 CASE 逻辑为 `classifyModuleRoleLegacy`，用 `#ifdef USE_LEGACY_ROLE` 切换，便于 A/B 对比 |

---

## 六、验证

### 6.1 单元测试

```
tests/test_role_classifier.cpp:
  - test_test_role: mod tests 命中 test
  - test_api_role: pub_count=5, incoming=15, outgoing=2 → api
  - test_core_role: incoming=20, outgoing=5, utilization=0.8, pub_count=3 → core
  - test_dead_role: incoming=0, outgoing=0 → dead
  - test_infra_fallback: 无信号命中 → infra
  - test_priority: test + dead 同时命中 → test（优先级 1 > 6）
```

### 6.2 集成测试（bun 项目）

```bash
./build/test_bun ~/code/researcher/bun
sqlite3 .codescope/codescope.db "SELECT role, COUNT(*) FROM module_summary GROUP BY role ORDER BY 2 DESC"
```

预期分布（bun ~270 模块）：
- utility 30-50%（大量 helper）
- core 10-20%（runtime/core 子系统）
- test 15-25%
- api 5-10%
- entry 1-3%
- dead 5-15%
- infra <30%（兜底）

若 infra >30%，说明阈值过严，调参。

### 6.3 构建

```
make astgraph_engine → [100%] Built ✅
cargo check codescope → Finished ✅
```

---

## 七、时间线

| 阶段 | 估时 | 产出 |
|------|------|------|
| schema migration + Visitor 填 visibility | 2h | entity.visibility 全语言覆盖 |
| classifier 重写 + 单测 | 3h | 多信号融合 CASE + 6 类单测过 |
| bun 实测调参 | 1h | role 分布合理 |
| 文档更新 | 0.5h | README 双语 + skills.md role 局限改为「v0.2.2 多信号融合」 |
| **合计** | **6.5h** | |

---

## 八、与调用图的关系（澄清）

- **结构数据共享**：role classifier 读 relation/graph_nodes（调用图）的 incoming/outgoing 统计
- **抽象层级不重叠**：调用图是边（A 调 B，细粒度有向海量）；role 是节点标签（这模块是 core 还是 test，粗粒度每模块一个）
- **独立信息量**：role 引入调用图没有的 `visibility`（pub/私有）+ `entry_reachable`（入口可达）两个信号，不沦为 utilization 换皮
- **类比**：调用图像 CPU 指令流，role 像「这进程是数据库还是前端」的进程分类——底层用了指令，但概念不在同一层
