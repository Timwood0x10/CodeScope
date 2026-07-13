# CodeScope Architecture (11): Verification Layer — Making AI Accountable

> Before writing this document, I ran `verify_integrity`. It checks whether features claimed in the README actually exist in the code. It reported one missing capability: `CommunityDetection` was in the capability list, but the MCP Server has no tool for it. An awkward but honest discovery.

---

## Three Questions

Code analysis tools can answer "what's in the code", but not "does the code actually do what the README claims?" — that's the verification layer's job.

1. **How to turn natural-language "promises" into executable checks?** (Claim parsing)
2. **How to verify a promise against code facts?** (Verifier dispatch)
3. **How to persist findings so AI can see them next time?** (Evidence chain)

---

## Architecture

The verification layer lives in `engine/src/verify/` (23 source files):

```
Natural language / README / PR description
        │
        ▼
  ClaimParser ───→ Claim (structured assertion)
        │                │
        │                ▼
        │         VerifierRegistry
        │                │
        │         ┌──────┼──────┐
        │         ▼      ▼      ▼
        │   Capability  Contract  Architecture
        │   Verifier   Verifier   Verifier
        │         │      │      │
        │         ▼      ▼      ▼
        │         Finding (evidence + confidence)
        │                │
        │                ▼
        │         SQLite findings table (persistent)
```

---

## ClaimParser: Natural Language → Structured Assertion

```cpp
struct Claim {
    ClaimType type;           // capability_exists / contract_holds / architecture_follows
    std::string subject;      // subject: module name / function name
    std::string predicate;    // predicate: supports / implements / ensures
    std::string object;       // object: feature / interface / property
    double confidence;        // parse confidence (0-1)
};
```

Supported sentence patterns:

| Pattern | Example | ClaimType |
|---------|---------|-----------|
| X supports Y | "Login module supports JWT" | capability_exists |
| X implements Y | "Server implements Service interface" | contract_holds |
| X ensures Y | "System ensures data consistency" | architecture_follows |

---

## VerifierRegistry: Dispatch to the Right Verifier

Three built-in verifiers in priority order:

1. **CapabilityVerifier**: Check "does module X actually support feature Y?"
2. **ContractVerifier**: Check "does entity X implement interface Y?"
3. **ArchitectureVerifier**: Check layer constraints (Controller→Service→Repository)

---

## Drift Detection (Batch Scan)

| Tool | Scope | Detects |
|------|-------|---------|
| `documentation_drift` | README | Claimed languages vs actual entities |
| `capability_drift` | Capability list | Claimed capabilities vs actual code |
| `architecture_drift` | All CALLS edges | Layer violations |
| `drift` (full) | All above | Aggregate report |

### Finding Data Structure

```cpp
struct Finding {
    uint64_t id;
    FindingSeverity severity;
    std::string category;
    std::string title;
    std::string detail;
    double confidence;
    std::string source_kind;
};
```

Findings are written to the SQLite `findings` table and support filtering by `source_kind`.

---

## Series Navigation

| # | Article | Topic |
|---|---------|-------|
| (1) | Intro | 56KB vs 629 bytes, what problem CodeScope solves |
| (2) | Progressive Readiness | ms-level project understanding |
| (3) | Worker Isolation | Indexing won't crash MCP Server |
| (4) | Zero-Redundancy Responses | Lean responses, on-demand return |
| (5) | C++ Engine Pipeline | Source code to multi-dimensional code graph |
| (6) | MCP Protocol Layer | Tool design philosophy |
| (7) | Language Translators | 10 languages → unified IR |
| (8) | Storage Layer | SQLite WAL + FTS5 + vec0 |
| (9) | Adaptive Queries | Fallback mechanism & readiness detection |
| (10) | Performance Truth | 200 to 60,000 files measured |
| **(11)** | **Verification Layer** | **Making AI Accountable ← this article** |
