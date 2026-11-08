#!/usr/bin/env bash
# ── check-code-rules.sh — enforce the size limits of code_rules §1.1/§1.4 ──
#
# Usage:
#   ./scripts/check-code-rules.sh                    # check against the baseline
#   ./scripts/check-code-rules.sh --update-baseline  # rewrite the baseline
#
# Rules enforced (hard limits fail, soft limits only warn):
#   file     : >1000 lines fails, >800 warns
#   function : >120 lines fails, >40 warns
#   struct   : >12 fields fails, >7 warns
#
# Why a baseline: code_rules §1.1 grandfathers existing over-limit code .
# The baseline records today's hard violations;
# the gate fails only on violations that are *not* in it. Because the baseline
# key includes the measured size, growing a grandfathered function produces a
# new key and therefore fails — shrinking it simply drops the key.
#
# Scope: engine/src, engine/include, server/src. Parsing is heuristic (braces,
# not a real parser) and deliberately conservative: string literals and line
# comments are stripped before counting, namespaces/impl blocks are handled by
# tracking a per-function base depth, and anything ambiguous is skipped rather
# than reported.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE="$ROOT/scripts/code-rules-baseline.txt"
HARD_FILE=1000
WARN_FILE=800
HARD_FUNC=120
WARN_FUNC=40
HARD_FIELDS=12
WARN_FIELDS=7

UPDATE=0
case "${1:-}" in
--update-baseline) UPDATE=1 ;;
--help | -h)
	sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
	exit 0
	;;
"") ;;
*)
	echo "unknown argument: $1" >&2
	exit 2
	;;
esac

cd "$ROOT"

scan_cpp() {
	find engine/src engine/include -type f \( -name '*.cpp' -o -name '*.h' \) \
		-not -path '*/build/*' -not -path '*/build-*/*' -print0
}

scan_rust() {
	find server/src -type f -name '*.rs' -print0
}

# Shared awk helpers: strip string literals and line comments before counting
# braces, otherwise `"{"` or a JSON template shifts the depth and merges
# unrelated functions into one giant span.
AWK_COMMON='
function strip(s,   out, i, c, q, esc, n) {
	out = ""; q = 0; esc = 0
	n = length(s)
	for (i = 1; i <= n; i++) {
		c = substr(s, i, 1)
		if (q) {
			if (esc) { esc = 0 }
			else if (c == "\\") { esc = 1 }
			else if (c == "\"") { q = 0 }
			continue
		}
		if (c == "\"") { q = 1; continue }
		if (c == "/" && substr(s, i + 1, 1) == "/") break
		out = out c
	}
	return out
}
function delta(s,   t, d) {
	t = s; d = gsub(/\{/, "", t)
	t = s; d -= gsub(/\}/, "", t)
	return d
}
'

measure_cpp_functions() {
	scan_cpp | xargs -0 awk "$AWK_COMMON"'
	function symname(s,   i, t, n, parts) {
		i = index(s, "(")
		if (i == 0) return s
		t = substr(s, 1, i - 1)
		gsub(/[[:space:]]+$/, "", t)
		n = split(t, parts, /[[:space:]]+/)
		t = parts[n]
		sub(/^[*&]+/, "", t)
		return t
	}
	function is_func(s,   i, head) {
		if (s ~ /^(namespace|struct|class|enum|union|template|using|typedef|extern)[[:space:]]/) return 0
		i = index(s, "(")
		if (i == 0) return 0
		head = substr(s, 1, i - 1)
		if (index(head, "=") > 0) return 0
		if (s ~ /;[[:space:]]*\{?[[:space:]]*$/) return 0
		return 1
	}
	FNR == 1 { depth = 0; pending = 0; tracking = 0; file = FILENAME }
	{
		clean = strip($0)
		before = depth
		depth += delta(clean)

		if (tracking) {
			if (depth <= fbase) {
				printf "function\t%s\t%s\t%d\n", file, fsym, FNR - fstart + 1
				tracking = 0
			}
			next
		}

		if (!pending) {
			if ($0 ~ /^[A-Za-z_~]/ && $0 !~ /^\/\//) {
				pending = 1; pstart = FNR; sig = $0; pbase = before
			}
		} else {
			sig = sig " " $0
		}

		if (pending && index(clean, "{") > 0) {
			if (is_func(strip(sig))) {
				tracking = 1; fstart = pstart; fsym = symname(strip(sig)); fbase = pbase
			}
			pending = 0; sig = ""
		} else if (pending && index(clean, ";") > 0) {
			pending = 0; sig = ""
		}
	}
	'
}

measure_cpp_fields() {
	scan_cpp | xargs -0 awk "$AWK_COMMON"'
	FNR == 1 { depth = 0; inside = 0; fields = 0; name = ""; stmt = "" }
	{
		clean = strip($0)
		if (!inside) {
			if (clean ~ /^(struct|class)[[:space:]]+[A-Za-z_]/) {
				if (index(clean, "{") == 0 && index(clean, ";") > 0) next  # forward declaration
				n = split(clean, p, /[[:space:]:{]+/)
				name = p[2]
				if (index(clean, "{") > 0) { inside = 1; depth = 1 }
				else { inside = 2; depth = 0 }
			}
			next
		}
		if (inside == 2) {
			if (index(clean, "{") > 0) { inside = 1; depth = 1 }
			next
		}
		depth += delta(clean)
		if (depth <= 0) {
			printf "fields\t%s\t%s\t%d\n", FILENAME, name, fields
			inside = 0; fields = 0; name = ""; stmt = ""
			next
		}
		# Count a member per complete statement, not per line: a member
		# declaration may wrap, and so may a method declaration. A statement
		# with "(" is a method; anything else at depth 1 is data.
		if (depth == 1 && clean ~ /^\t[A-Za-z_]/) {
			stmt = (stmt == "") ? clean : stmt " " clean
			if (stmt ~ /;[[:space:]]*$/) {
				if (stmt !~ /\(/ &&
				    stmt !~ /^[[:space:]]*(using|typedef|friend|static|return)[[:space:]]/)
					fields++
				stmt = ""
			}
		} else if (depth != 1) {
			stmt = ""
		}
	}
	'
}

measure_rust_functions() {
	scan_rust | xargs -0 awk "$AWK_COMMON"'
	FNR == 1 { depth = 0; tracking = 0 }
	{
		clean = strip($0)
		before = depth
		cand = 0; fsym = ""
		if (match(clean, /^[[:space:]]*(pub(\([^)]*\))?[[:space:]]+)?(const[[:space:]]+)?(async[[:space:]]+)?(unsafe[[:space:]]+)?(extern[[:space:]]+"[^"]*"[[:space:]]+)?fn[[:space:]]+[A-Za-z_][A-Za-z0-9_]*/)) {
			if (index(clean, "{") > 0) {
				seg = substr(clean, RSTART, RLENGTH)
				n = split(seg, p, /[[:space:]]+/)
				fsym = p[n]; cand = 1
			}
		}
		depth += delta(clean)
		if (tracking) {
			if (depth <= fbase) {
				printf "function\t%s\t%s\t%d\n", FILENAME, fsym_done, FNR - fstart + 1
				tracking = 0
			}
		} else if (cand) {
			tracking = 1; fstart = FNR; fbase = before; fsym_done = fsym
		}
	}
	'
}

file_sizes() {
	{
		scan_cpp
		scan_rust
	} | xargs -0 wc -l 2>/dev/null | awk '$2 != "total" { printf "file\t%s\t-\t%d\n", $2, $1 }'
}

measure() {
	measure_cpp_functions
	measure_cpp_fields
	measure_rust_functions
	file_sizes
}

hard_violations() {
	measure | awk -F'\t' -v hf="$HARD_FILE" -v hn="$HARD_FUNC" -v hs="$HARD_FIELDS" '
		$1 == "file"     && $4 > hf { print $1 "|" $2 "|" $3 "|" $4 }
		$1 == "function" && $4 > hn { print $1 "|" $2 "|" $3 "|" $4 }
		$1 == "fields"   && $4 > hs { print $1 "|" $2 "|" $3 "|" $4 }
	' | LC_ALL=C sort
}

warn_report() {
	measure | awk -F'\t' -v wf="$WARN_FILE" -v wn="$WARN_FUNC" -v ws="$WARN_FIELDS" '
		$1 == "file"     && $4 > wf { printf "  warn  file      %s (%d lines)\n", $2, $4 }
		$1 == "function" && $4 > wn { printf "  warn  function  %s:%s (%d lines)\n", $2, $3, $4 }
		$1 == "fields"   && $4 > ws { printf "  warn  struct    %s:%s (%d fields)\n", $2, $3, $4 }
	' | LC_ALL=C sort -t'(' -k2 -rn | head -20
}

if [[ "$UPDATE" == "1" ]]; then
	# Regenerating the baseline is how a new violation becomes "pre-existing".
	# In CI that would silently legalise whatever the branch just introduced,
	# so it is refused there: re-baselining is a deliberate local act that has
	# to show up in review.
	if [[ -n "${CI:-}" || -n "${GITHUB_ACTIONS:-}" ]]; then
		echo "refusing --update-baseline in CI: it would grandparent the" >&2
		echo "violations this branch introduced. Re-baseline locally and send" >&2
		echo "the baseline change through review." >&2
		exit 2
	fi
	{
		echo "# Grandfathered violations of plan/rules/code_rules.md §1.1/§1.4."
		echo "# CI forbids regenerating this file; update it locally only when"
		echo "# re-baselining on purpose, and say why in the commit message."
		hard_violations
	} >"$BASELINE"
	echo "baseline written: $BASELINE ($(grep -vc '^#' "$BASELINE" | tr -d ' ') entries)"
	exit 0
fi

if [[ ! -f "$BASELINE" ]]; then
	echo "missing baseline: $BASELINE" >&2
	echo "run: $0 --update-baseline" >&2
	exit 2
fi

# A violation is grandfathered when the baseline already records the same
# (kind, path, symbol) at a size >= the current one. Comparing on the key
# alone would fail whenever a grandfathered function shrinks — the ratchet must
# allow improvement and only block new or larger violations.
new_violations="$(
	hard_violations | awk -F'|' '
		NR == FNR {
			if ($0 ~ /^#/) next   # header lines are documentation
			base[$1 "|" $2 "|" $3] = $4
			next
		}
		{
			key = $1 "|" $2 "|" $3
			if (!(key in base) || $4 > base[key]) print $0
		}
	' "$BASELINE" -
)"

echo "[rules] hard limits: file>${HARD_FILE}, function>${HARD_FUNC}, struct-fields>${HARD_FIELDS}"
warn_report

if [[ -n "$new_violations" ]]; then
	echo
	echo "✗ new violations of plan/rules/code_rules.md §1.1/§1.4:" >&2
	echo "$new_violations" | while IFS='|' read -r kind path sym size; do
		case "$kind" in
		file) echo "  $path: $size lines (max $HARD_FILE)" >&2 ;;
		function) echo "  $path: $sym(): $size lines (max $HARD_FUNC)" >&2 ;;
		fields) echo "  $path: $sym: $size fields (max $HARD_FIELDS)" >&2 ;;
		esac
	done
	echo
	echo "fix: split the unit, or (for pre-existing code only) run" >&2
	echo "     $0 --update-baseline  and explain why in the PR." >&2
	exit 1
fi

echo "  ✓ no new size violations"
