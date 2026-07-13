# CodeScope 架构拆解（十二）：Model Engine 与 State Builder

> Phase B 完成后，数据库里有 entity、relation、graph_edges 这些"事实"。但事实本身不是"理解"——你需要把事实组合成更高层的模型：这个模块的能力是什么？它的工作流长什么样？架构分层是否正确？这就是 Model Engine 的工作。

---

## 职责

Model Engine 在 Resolver Pipeline 之后运行，负责从事实（Facts）和解析结果（Resolution）构建**高层模型**。

```
Resolver Pipeline ──→ entity + relation + call_edges
                            │
                            ▼
                    ModelEngine::runAll()
                            │
                    ┌───────┼───────┐
                    ▼       ▼       ▼
              Workflow   Capability  Architecture
              Plugin     Plugin      Plugin
                    │       │       │
                    ▼       ▼       ▼
              SQLite: workflow / capability / architecture 表
```

---

## ModelEngine：插件管理器

```cpp
// engine/src/model/engine.h
class ModelEngine {
public:
    void addPlugin(std::unique_ptr<ModelPlugin> plugin);
    int64_t runAll(uint64_t project_id);
    ModelResult run(const std::string &name, uint64_t project_id);
};
```

`ModelEngine` 不执行任何具体分析——它只管理插件的生命周期和遍历顺序。每个插件实现 `ModelPlugin` 接口：

```cpp
// engine/src/model/plugin.h
class ModelPlugin {
public:
    virtual const char *name() const = 0;
    virtual ModelResult run(uint64_t project_id) = 0;
};
```

---

## StateBuilder：项目状态恢复

```cpp
// engine/src/model/state_builder.h
// 从 SQLite 恢复项目当前状态（能力、工作流、架构、模块摘要）
// 每次 project_overview / explain_module 调用时运行
```

`StateBuilder` 是 Model Engine 的核心消费方——它从已构建的模型中读取数据，组装成 AI 可理解的模块摘要和项目概览。

---

## 当前内置插件

实际项目中注册的插件列表可以从 capabilities 表查到：

| 插件 | 输出表 | 说明 |
|------|--------|------|
| Workflow Plugin | workflow | 跨模块的工作流路径 |
| Capability Plugin | capability | 模块能力声明与验证 |
| Architecture Plugin | architecture | 架构分层与层间依赖 |
| Contract Plugin | contract | 契约（接口实现关系） |

每个插件在 `run()` 中执行 SQL 查询 + 关系分析，结果写入对应的 SQLite 表，供查询引擎（article 09）读取。

---

## 与验证层的关系

Model Engine 和 Verification Layer 共享数据但职责分离：

| | Model Engine | Verification Layer |
|---|---|---|
| 输入 | entity + relation + graph_edges | entity + relation + README |
| 输出 | 模型表（workflow/capability/architecture） | Finding（证据链） |
| 调用时机 | 索引时（Phase B） | 查询时（按需） |
| 使用者 | `project_overview` / `explain_module` | `verify_claim` / `detect_drift` |

简单说：Model Engine **建模型**，Verification Layer **用模型做检查**。

---

## 局限

1. **插件的数量有限**——目前只有 4 个内置插件。架构插件依赖命名约定（Controller/Service/Repository），不在约定内的项目不产生有效输出。
2. **没有插件热加载机制**——所有插件编译进二进制，不能动态添加。如果需要新增模型类型（如"数据流分析"），需要改 C++ 代码重新编译。
3. **StateBuilder 的聚合粒度**——目前以模块（目录）为单位聚合。如果一个目录里混合了多个不同职责的文件，摘要可能不够精确。

---

## 系列导航

| # | 文章 | 主题 |
|---|------|------|
| (一) | 开篇 | 56KB vs 629 bytes，CodeScope 要解决什么问题 |
| (二) | 渐进式就绪 | 毫秒级让 AI 开始理解你的代码 |
| (三) | Worker 隔离 | 为什么索引不会拖垮 MCP Server |
| (四) | 零冗余响应 | 精简响应，按需返回 |
| (五) | C++ 引擎拆解 | 从源码到多维代码图的管线 |
| (六) | MCP 协议层 | 工具的设计哲学 |
| (七) | 语言翻译器 | 10 种语言 → 统一 IR |
| (八) | 存储层 | SQLite WAL + FTS5 + vec0 |
| (九) | 自适应查询 | Fallback 机制与就绪检测 |
| (十) | 性能真相 | 从 200 到 60,000 文件的实测 |
| (十一) | 验证层 | 让 AI 对自己的话负责 |
| **(十二)** | **Model Engine** | **从事实到理解 ← 本文** |
