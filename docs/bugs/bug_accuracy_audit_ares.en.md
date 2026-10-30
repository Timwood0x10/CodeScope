# bug: accuracy audit against a real Go repo (v0.2.7)

Audit date: 2026-10-18. Target: `Timwood0x10/ARES` (Go, 1718 files, 25441 entities,
7840 relations after `index-parallel`). Every claim below was checked against the source
tree, not against another tool.

Scope: `verify_integrity`, `find_symbol`, `find_callers`, `search`, and the interaction
between `index-parallel` and a running server.

---

## Finding 1 — `find_symbol` / `find_callers` report `line` 0-based, callers 1-based users expect

**Repro**

```bash
codescope cli find_symbol '{"symbol_name":"ParetoDominance"}'
# → {"file_path":".../genome/multi_objective.go","line":49}

sed -n '49p;50p' internal/runtime/ares_evolution/genome/multi_objective.go
# 49: // no worse in all others. Dimension direction (maximize/minimize) is respected.
# 50: func ParetoDominance(a, b *mutation.Strategy) bool {
```

**Expected** `line: 50`. **Actual** `line: 49`. Same for `ParetoFront`: reported 91,
source 92.

**Root cause (hypothesis)**: the store keeps `entity.start_row` 0-based; the MCP layer
exposes it as `line` without `+1`. Verified in the DB:

```sql
select name, start_row from entity where name in ('ParetoDominance','ParetoFront');
-- ParetoDominance|49, ParetoFront|91   (both 0-based)
```

**Fix + test**: add 1 at the serialization boundary; assert
`find_symbol(line) == grep -n '^func X' | cut -d: -f1` in an E2E test against a fixture
file whose function is on a line > 1.

---

## Finding 2 — `DeadFunction` false positives: function values and interface dispatch

**Repro (8 sampled flags, each checked in its own package and for value-use)**

| flagged | verdict | why |
|---|---|---|
| `panelAvailable` (cmd/ares) | **FP** | used as a value: `{..., Available: panelAvailable}` in a route table |
| `mutateNWithProbs` (mutation) | **FP** | `ad.mutator.mutateNWithProbs(...)` + declared in an interface |
| `setCandidate` (ares_bootstrap) | **FP** | method value: `WithEvalGateBeforeRun(exec.setCandidate)` |
| `writeEmbedding` (ares_bootstrap) | **FP** | interface method `writer.writeEmbedding(...)` |
| `aggregateEvidence` (tools/planner) | **FP** | package call + `s.aggregateEvidence(...)` method |
| `lookupAgent` (sdk) | **true dead** | zero uses beyond its own definition |
| `parseArgs` (sdk) | **true dead** | zero uses beyond its own definition |
| `cosineSimilarity` (memory/manager_impl.go) | **true dead** | the live calls are `ConflictResolver.cosineSimilarity` and the distillation resolver's, both different functions |

⇒ sample precision **5 FP / 8 = 62.5% FP**, i.e. **37.5% precision**. The two FP classes
are consistent: **function values** and **interface/method dispatch**. A naive caller that
trusts `DeadFunction` would delete live code.

**Suggested fix**: before emitting `DeadFunction`, treat "identifier appears outside a call
expression" (assignment, composite literal field, arg position) and "same name declared in
an interface" as *used*. A cheap first step: if the entity has zero call edges **and** the
identifier appears elsewhere in the same package outside comments, downgrade to
`Unknown` instead of `DeadFunction`.

---

## Finding 3 — capability findings are README prose, not claims

**Repro**: `verify_integrity` on this repo emits, at confidence 0.7:

```
Capability 'Features' Contradicted: ... nothing calls it and it is not exported
Capability 'KeyFeatures' Contradicted: ...
Capability 'AKGAdaptiveKnowledgeGraphAPIFrozen034Sch' Contradicted: ...
Capability 'InPracticeTasksAreDurableAndOutliveTheir' Contradicted: ...
```

`KeyFeatures` comes from the README heading `### Key Features` (line 722); the others are
sentences parsed as identifiers (`Most034AKGFeaturesAreNotWiredIntoProduct`,
`FrozenTheFeaturesBehindItAreNotLiveVerif` are truncated prose).

**Impact**: 10 of 10 capability findings on this repo are noise, and they inflate
`contradicted` while lowering `trust_score` to 0 — which hides the one *true* finding
(`internal/evoapi`, 0 importers).

**Suggested fix**: require a capability name to look like an identifier (no spaces after
camel-casing collapse, length limits, reject names ending in a verb fragment), or only
accept capabilities from an explicit manifest/list rather than free prose.

---

## Finding 4 — `index-parallel` replaces the DB; a running server silently serves an empty graph

**Repro**

```bash
# server running, healthy:
codescope cli get_graph_stats '{}'   # → {"total_nodes":24301,...}

codescope index-parallel /path/to/project
# → "installed its database at .codescope/codescope.db (199208960 bytes)"

codescope cli get_graph_stats '{}'   # → {"total_nodes":0,"total_edges":0,"total_files":0}
```

`lsof` on the server PID shows it holds the **new** file (same path, same size), so this is
not a deleted-inode issue: the server keeps a cached snapshot/project state and never
re-reads it. Restarting the server fixes it.

**Impact**: every absence-type verdict (`DeadModule`, "no callers", "symbol not found")
becomes wrong in the worst direction — it says "absent" while holding a stale view, and it
looks like a normal empty project rather than an error.

**Suggested fix (pick one)**:
1. `get_graph_stats` returns an explicit `stale_index: true` when the on-disk DB mtime/inode
   differs from what the server opened;
2. the server re-opens on mtime change (cheap `stat` before each tool call);
3. at minimum, `index-parallel` prints `restart the MCP server to pick this up` — today the
   only hint is a byte count.

---

## Note — what the audit did NOT find

- `find_symbol` / `search` / `trace` are genuinely useful for navigation and saved a large
  amount of file reading; the misses above are all in *verdict* tools, not lookup tools.
- `DeadModule` was **correct** where the index was fresh (`internal/evoapi`: 252 entities,
  0 importers), and its two false positives in the same run (`knowledge/retriever`,
  `postgres/services`) both disappeared after re-indexing — i.e. that rule is only as good
  as the index, which is Finding 4.
