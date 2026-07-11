# CodeScope × goagent — 桥接验证 & Token 节省

> 日期：2026-07-11
> 项目：[goagent](https://github.com/Timwood0x10/ares)（Go）
> 1,124 文件，331,390 行代码

---

## 桥接验证

旧 GA 系统（scheduler）与新系统（coordinator）通过 `GenomePopulationAdapter` + `submitToCoordinator` 桥打通。调用链通过 grep 验证（非调用图——图为 struct 方法调用未捕获）。

### 调用链

| 链路 | 状态 | 位置 |
|------|------|------|
| `Run()` → `submitToCoordinator()` | ✅ | `genome_wiring.go:462` |
| `submitToCoordinator()` → `generateDiffPatches()` | ✅ | `genome_wiring.go:578` |
| `submitToCoordinator()` → `Coordinator.Submit()` | ✅ | `genome_wiring.go:584` |
| 生产路径 `bootstrap.go` → `Coordinator.Submit()` | ✅ | `bootstrap.go:303,363` |
| 生产路径 `genome_wiring_system.go:743` → `Coordinator.Submit()` | ✅ | |
| 自愈路径 `evolution_bridge.go:70` → `Coordinator.Submit()` | ✅ | |

### 架构

```
旧系统 Scheduler（事件驱动）
  → GenomePopulationAdapter.Run()
    → 评分 + 进化 (population.Evolve())
    → submitToCoordinator() ← 新桥
      → generateDiffPatches()
      → Coordinator.Submit(PatchProposal{Source: SourceGA})
      → Coordinator.Evaluate()

新系统（决策层）
  → Coordinator 接收 patch proposals
  → 决定 Apply / Delay / Reject
  → 通过 Patch Executor 应用
```

---

## Token 节省

| 指标 | 值 |
|------|-----|
| 源文件 | 1,124 / 1,253 |
| 代码行数 | 331,390 |
| 原始 token | 3,313,900 |
| 知识图谱 token | 140,470 |
| **Token 节省** | **95.8%** |
| 查询延迟 | 0.5 ms |
| 索引时间 | 31 s |
| 实体数 | 13,535 |
| 关系边 | 1,024 |
| 已解析引用 | 1,022 |

### 计算方式

```
原始 token:      331,390 行 × 10 = 3,313,900
知识图谱 token:  13,535 × 10 + 1,024 × 5 = 140,470
节省:             95.8%
```

**AI 的上下文从 331 万 token 压缩到 14 万 token，信息密度高 23 倍。**

---

## 孤儿模块（基于 grep 导入路径，排除自引用）

| 模块 | 实体数 | 外部导入 | 判定 |
|------|--------|---------|------|
| `compat/` | 173 | 0 | ✅ 孤儿 |
| `api/evolution/` | 66 | 0 | ✅ 孤儿 |
| `api/service/ga/` | 15 | 0 | ✅ 孤儿（已删除） |
| `graphservice/` | 39 | 0 | ✅ 孤儿（已删除） |
| `EvolutionBridge` | — | 0 | ✅ 孤儿（从未实例化） |
| `retrieval_api/` | 20 | 0 | ✅ 孤儿（已删除） |
| `ares_quant/` | 992 | 0 | ✅ 孤儿（新发现） |

### 与审计文档一致性

人工审计的 6 个孤儿模块全部命中，100% 一致。CodeScope 自动分析与手动 grep 结论一致。

---

## 关键结论

**Go 的调用图（relation 表）不足以检测连接。** 接口调用和 struct 方法调用（如 `a.submitToCoordinator(ctx)`）不会被当前 Resolver Pipeline 捕获。正确的孤儿检测方法是**基于导入路径的分析**（grep 导入路径，排除自引用），与人工审计方法一致。