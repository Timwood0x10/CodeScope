#!/usr/bin/env bash
# graph_baseline.sh — graph-SHAPE regression baseline.
#
# The other benchmarks here measure speed and memory. This one measures the
# RESULT: how many call edges the resolver produces on a fixed tree, and how
# they are distributed across resolution kinds, parse-time strategies and
# deciding factors. A change to any visitor or to a resolver factor can add,
# drop or re-point edges, and `make test-engine` cannot see it — the unit tests
# pin hand-built fixtures, not the shape of a real project's graph.
#
#   ./benchmarks/graph_baseline.sh                 # compare against the baselines
#   ./benchmarks/graph_baseline.sh --update        # (re)write the baselines
#   ./benchmarks/graph_baseline.sh --only self     # one project
#
# Exit status is non-zero when any baseline differs, so CI (or a pre-commit
# hook) can gate on it. Requires bin/codescope, built by `make build`.
#
# Determinism: the shape is reproducible for a fixed binary and tree — two
# consecutive index runs of the fallback project produced identical edge sets
# (0 added, 0 removed) — so the comparison is exact, with no tolerance window.
# A baseline that drifts because the CODE changed is the point; a baseline that
# drifts between two runs of the same binary would be a bug in the indexer,
# which is what makes this script's failure mode informative.
#
# Projects are local checkouts, named by language, so a change that helps Go and
# hurts Rust is visible instead of averaged away. Set CODESCOPE_BENCH_PROJECTS
# to relocate them; missing projects are skipped with a warning.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE_DIR="$ROOT/benchmarks/baselines"
# One work directory per PROCESS: a fixed path let two concurrent runs index
# into the same files, which showed up as "no such table: entity" for whichever
# project lost the race rather than as a clear failure.
WORK_DIR="${TMPDIR:-/tmp}/codescope-graph-baseline/$$"
PROJECTS_ROOT="${CODESCOPE_BENCH_PROJECTS:-/Users/scc/code/researcher}"
BIN="$ROOT/bin/codescope"

UPDATE=0
ONLY=""
while [[ $# -gt 0 ]]; do
	case "$1" in
	--update) UPDATE=1 ;;
	--only)
		shift
		ONLY="${1:-}"
		;;
	*)
		echo "usage: $0 [--update] [--only <name>]" >&2
		exit 2
		;;
	esac
	shift
done

# name|path  — the name is the baseline file's suffix, the path is a checkout.
# `self` is this repository: the C++ tree the engine is developed against.
#
# One project per language, so a change that helps Go and hurts Rust is visible
# instead of averaged away. A Java checkout is deliberately NOT listed:
# index-parallel cannot index a Maven layout at all — discover_modules drops the
# `samples` component of `src/main/java/org/.../samples/petclinic/` and reports
# "no source modules found" for the whole tree (see benchmarks/README.md). Add
# one back when that is fixed, or the run would fail on a project the tool
# cannot read rather than on a real regression.
PROJECTS=(
	"self|$ROOT"
	"c-redis|$PROJECTS_ROOT/redis"
	"go-tinygo|$PROJECTS_ROOT/tinygo"
	"rust-pyo3|$PROJECTS_ROOT/pyo3"
	"ts-codebase-memory-mcp|$PROJECTS_ROOT/codebase-memory-mcp"
)

[[ -x "$BIN" ]] || {
	echo "graph_baseline: $BIN not found — run 'make build' first" >&2
	exit 2
}
mkdir -p "$BASELINE_DIR" "$WORK_DIR"

# The factor that decided each scored match, extracted from
# relation.reason ("decided_by=<factor> name=... arity=... score=...").
FACTOR_SQL="SELECT substr(r, 1, instr(r || ' ', ' ') - 1) AS c, COUNT(*) AS n
	FROM (SELECT substr(reason, instr(reason, 'decided_by=') + 11) AS r
	      FROM relation WHERE type = 1 AND reason LIKE '%decided_by=%')
	GROUP BY c ORDER BY c"

# dump_shape <db> <out> — the canonical, sorted, diffable shape of one graph.
dump_shape()
{
	local db="$1" out="$2"
	{
		printf '{\n'
		printf '  "files": %s,\n' \
			"$(sqlite3 "$db" 'SELECT COUNT(DISTINCT file_path) FROM entity;')"
		printf '  "entities": %s,\n' \
			"$(sqlite3 "$db" 'SELECT COUNT(*) FROM entity;')"
		printf '  "call_edges": %s,\n' \
			"$(sqlite3 "$db" 'SELECT COUNT(*) FROM relation WHERE type = 1;')"
		printf '  "call_sites": %s,\n' \
			"$(sqlite3 "$db" 'SELECT COUNT(*) FROM reference;')"
		dump_group "$db" "resolution_kind" \
			"SELECT COALESCE(NULLIF(resolution_kind, ''), '(empty)') AS c,
				COUNT(*) AS n
			 FROM relation WHERE type = 1 GROUP BY c ORDER BY c"
		# The parse-time strategy lives on the call SITE (reference), not on
		# the edge: it is what the visitor guessed before the resolver ran, so
		# its distribution is a different question from resolution_kind.
		dump_group "$db" "resolve_strategy" \
			"SELECT COALESCE(NULLIF(resolve_strategy, ''), '(empty)') AS c,
				COUNT(*) AS n
			 FROM reference GROUP BY c ORDER BY c"
		dump_group "$db" "deciding_factor" "$FACTOR_SQL"
		printf '  "unresolved_call_sites": %s\n' \
			"$(sqlite3 "$db" 'SELECT (SELECT COUNT(*) FROM reference) -
				(SELECT COUNT(*) FROM relation WHERE type = 1);')"
		printf '}\n'
	} >"$out"
}

# dump_group <db> <key> <sql> — one JSON object of value→count, last value
# without a trailing comma so the file is valid JSON.
dump_group()
{
	local db="$1" key="$2" sql="$3"
	printf '  "%s": {\n' "$key"
	sqlite3 "$db" \
		"SELECT '    \"' || c || '\": ' || n || ',' FROM ($sql);" |
		sed '$ s/,$//'
	printf '  },\n'
}

total=0
failures=0
for entry in "${PROJECTS[@]}"; do
	name="${entry%%|*}"
	path="${entry#*|}"
	[[ -n "$ONLY" && "$name" != "$ONLY" ]] && continue
	if [[ ! -d "$path" ]]; then
		printf '  %-26s skipped (no checkout at %s)\n' "$name" "$path"
		continue
	fi

	db="$WORK_DIR/$name.db"
	current="$WORK_DIR/$name.json"
	baseline="$BASELINE_DIR/graph_$name.json"
	rm -f "$db" "$db-wal" "$db-shm"

	CODESCOPE_DB_PATH="$db" "$BIN" index-parallel "$path" -w 4 \
		>"$WORK_DIR/$name.log" 2>&1 ||
		{
			printf '  %-26s INDEX FAILED (see %s)\n' "$name" \
				"$WORK_DIR/$name.log"
			failures=$((failures + 1))
			continue
		}
	# `index-parallel` exits 0 when discovery found nothing to index, so the
	# exit status alone cannot tell a real index from an empty one; the
	# scheduler's own `note` is the only actionable diagnosis.
	if [[ "$(sqlite3 "$db" 'SELECT COUNT(*) FROM entity;' 2>/dev/null || echo 0)" == 0 ]]; then
		printf '  %-26s INDEXED NOTHING: %s\n' "$name" \
			"$(grep -o '"note":"[^"]*"' "$WORK_DIR/$name.log" | tail -1)"
		printf '    (the indexer saw no source files in %s)\n' "$path"
		failures=$((failures + 1))
		continue
	fi
	dump_shape "$db" "$current"
	total=$((total + 1))

	if [[ "$UPDATE" == 1 ]]; then
		cp "$current" "$baseline"
		printf '  %-26s baseline updated (%s edges)\n' "$name" \
			"$(sqlite3 "$db" 'SELECT COUNT(*) FROM relation WHERE type=1;')"
		continue
	fi
	if [[ ! -f "$baseline" ]]; then
		printf '  %-26s no baseline yet — run with --update\n' "$name"
		failures=$((failures + 1))
		continue
	fi
	if diff -q "$baseline" "$current" >/dev/null; then
		printf '  %-26s ✓ identical (%s edges)\n' "$name" \
			"$(sqlite3 "$db" 'SELECT COUNT(*) FROM relation WHERE type=1;')"
	else
		printf '  %-26s ✗ SHAPE CHANGED\n' "$name"
		diff -u "$baseline" "$current" | sed -n '1,60p'
		failures=$((failures + 1))
	fi
done

if [[ "$total" == 0 ]]; then
	printf 'graph_baseline: no project could be measured (work dir kept: %s)\n' \
		"$WORK_DIR" >&2
	exit 2
fi
if [[ "$UPDATE" == 1 ]]; then
	printf 'graph_baseline: %s baseline(s) written to %s\n' "$total" \
		"$BASELINE_DIR"
	[[ "$failures" == 0 ]] && rm -rf "$WORK_DIR"
	exit 0
fi
if [[ "$failures" != 0 ]]; then
	printf 'graph_baseline: %s of %s project(s) differ from their baseline\n' \
		"$failures" "$total"
	printf 'inspect the diff above; if the change is intended, re-run with --update\n'
	printf '(the databases are kept for inspection: %s)\n' "$WORK_DIR"
	exit 1
fi
rm -rf "$WORK_DIR"
printf 'graph_baseline: %s project(s) match their baseline\n' "$total"
