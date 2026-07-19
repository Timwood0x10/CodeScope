# CodeScope 代码审查 — 未完成项清单

**审查日期**: 2026-07-19
**已完成**: 5 Critical / 10 High / 11 Medium（已从本文件移除，见原审查记录）
**以下为未完成项（P3 机会修复 / pre-existing，本轮未处理）**

---

## Low 发现（未修复，18+ 条）

### IR 翻译器
- `semantic_unit.h:22-26` — `SourceRange::isValid()` 逻辑错（`||` 应为 `&&`），但无调用方
- `c_translator.cpp:571-606` — `handleAttributedDeclarator` 死代码从未被调用
- `c_translator.cpp:79` — `resolveWithFallback` 声明未定义
- `js_visitor.cpp:233-238` — `nodeTextView` 从未被调用
- `semantic_emitter.cpp:102-112` — `emitScope` 从未被调用，且语义错误
- `swift_translator.cpp`/`swift_visitor.cpp` — 未编译（grammar 已移除）
- `python_translator.cpp:453` — `handleAssignment` 死分支（`=` 是匿名节点）
- 所有翻译器 — 列号为字节偏移而非字符（非 ASCII 源码错位）
- `python_visitor.cpp:197-207` — 装饰器、superclasses 静默丢弃
- 所有 import 处理 — 存储整条语句文本而非模块名
- `rust_translator.cpp:347-367` — struct field_declaration 未处理
- `rust_translator.cpp:478-491` — scoped_identifier 调用永不解析
- `python_visitor.cpp:122-176` — tuple 解包、`*args`、`**kwargs` 丢失

### store
- `store_project.cpp:93-96` — `insertSymbol` bind 索引 2 双重绑定（最终值正确）
- `store_core.cpp:397-404` — `setProjectReadiness` SQL 拼接（有白名单防护，安全）
- `store_graph.cpp`/`store_type.cpp` — `to_string(project_id)` 拼接（uint64_t，无注入）
- `store_schema.cpp:846-848` — `type_info` 迁移 else 分支变量名错误（double-finalize probe，泄漏 probe2）

### verify/model
- `model/state_builder.cpp:225` — `buildWorkflowState` 硬编码假数据
- `model/state_builder.cpp:293` — `buildArchitectureState` compliance 恒为 0
- `model/plugins/capability.cpp:77-78` — "Support" 漏检
- `verify/architecture_verifier.cpp:105` — 路径复数规则错误（entitys/factorys）
- `engine_verify_drift_ffi.cpp:342-345` — drift 检测用子串匹配分类契约

### engine 顶层
- `async_knowledge.cpp:331-334` — `modules` UPDATE 用字符串拼接（language 字段）
- `query_engine.cpp:830-834` — `locateNode` 整数拼接 SQL（uint64_t，无注入）
- `graph_query.cpp:60-64, 120-128` — 整数拼接（已白名单，无注入）

### server
- `tools/mod.rs:444`、`main.rs:264` — `CString::new(s).unwrap_or_default()` 静默丢弃含 NUL 的 lang_filter
- `tools/mod.rs:288-291` — `run_worker` kill 失败时 drain 线程泄漏
- `tools/mod.rs:836` — `h_get_knowledge_graph` limit 未 clamp 即 `as i32`
- `merge.rs:289-293` — `SELECT *` 列数依赖 schema 一致性

---

## 未处理项（pre-existing，超出本轮范围）

- `store_schema.cpp`（1323 行）和 `store_graph.cpp`（1201 行）超过 1000 行文件大小限制 —— pre-existing 问题。
- `ahocorasick.h` 仍使用 raw `new`/`delete` —— pre-existing，FFI 边界外的工具类，本轮未触及。
