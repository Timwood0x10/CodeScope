#!/bin/bash
# ──────────────────────────────────────────────────────────────────────────
# codescope-parallel — Parallel module indexer with dynamic worker dispatch
#
# Strategy:
#   1. `codescope discover` scans the project structure (per-directory counts)
#   2. Workers are allocated proportionally to module file count
#   3. When a module finishes, its workers are reassigned to remaining modules
#   4. Per-file quarantine: crashed modules get binary-search retry
#   5. All module DBs are merged into a single project DB at the end
#
# Modes (choose based on how much CPU you can spare):
#   --fast      Use ALL CPU cores, fast filter mode.  Best for dedicated CI.
#   --balanced  Use ~75% of cores (default).  Good for developer workstation.
#   --slow      Use 1-2 workers, minimal resources.  Background indexing.
#
# Usage:
#   ./codescope-parallel.sh [--fast|--balanced|--slow] <project_dir> [db_path]
#
#   Examples:
#     ./codescope-parallel.sh --fast /path/to/project
#     ./codescope-parallel.sh --slow ~/code/myapp ~/myapp.db
#     CODESCOPE=/usr/local/bin/codescope ./codescope-parallel.sh /project
#
# Environment variables:
#   CODESCOPE          Path to codescope binary (auto-detected if unset)
#   CODESCOPE_WORKERS  Override total worker count (overrides --fast etc.)
#   CODESCOPE_PARALLEL Override max concurrent modules (default = workers)
#
# Requirements:
#   - codescope binary (alongside this script, in PATH, or via CODESCOPE env)
#   - sqlite3 CLI (for DB merging)
#   - python3 OR jq (for JSON parsing of discover output)
# ──────────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── Locate the codescope binary ─────────────────────────────────────
find_codescope() {
    # 1. Explicit env var
    if [ -n "${CODESCOPE:-}" ] && [ -x "$CODESCOPE" ]; then
        echo "$CODESCOPE"
        return 0
    fi
    # 2. Same directory as this script (release bundle layout)
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if [ -x "$script_dir/codescope" ]; then
        echo "$script_dir/codescope"
        return 0
    fi
    # 3. PATH lookup
    if command -v codescope >/dev/null 2>&1; then
        echo "codescope"
        return 0
    fi
    return 1
}

# ── Detect CPU cores (portable: Linux + macOS) ─────────────────────
detect_cpu_cores() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif [ -x /usr/sbin/sysctl ] || command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || echo 4
    else
        echo 4
    fi
}

# ── Detect JSON parser (python3 or jq) ─────────────────────────────
detect_json_parser() {
    if command -v python3 >/dev/null 2>&1; then
        echo "python3"
    elif command -v jq >/dev/null 2>&1; then
        echo "jq"
    else
        echo ""
    fi
}

# ── Parse a JSON field from stdin using available tools ────────────
# Usage: echo "$json" | json_get "field_name"
json_get() {
    local field="$1"
    local parser="$JSON_PARSER"
    if [ "$parser" = "python3" ]; then
        python3 -c "import sys,json; print(json.load(sys.stdin)['$field'])"
    elif [ "$parser" = "jq" ]; then
        jq -r ".$field"
    else
        echo ""
    fi
}

# ── Parse modules from discover JSON into "name:count" lines ───────
parse_modules() {
    local parser="$JSON_PARSER"
    if [ "$parser" = "python3" ]; then
        python3 -c "
import sys, json
data = json.load(sys.stdin)
for name, count in sorted(data['modules'].items(), key=lambda x: -x[1]):
    print(f'{name}:{count}')
"
    elif [ "$parser" = "jq" ]; then
        jq -r '.modules | to_entries | sort_by(-.value) | .[] | "\(.key):\(.value)"'
    else
        # Fallback: no JSON parser available — cannot proceed
        return 1
    fi
}

# ── Help ───────────────────────────────────────────────────────────
print_help() {
    cat <<'EOF'
codescope-parallel — Parallel module indexer with dynamic worker dispatch

USAGE:
    codescope-parallel.sh [OPTIONS] <project_dir> [db_path]

OPTIONS:
    --fast, -f       Use ALL CPU cores + fast filter mode. Best for CI.
    --balanced, -b   Use ~75% of CPU cores (default). Developer workstation.
    --slow, -s       Use 1-2 workers, minimal resources. Background indexing.
    --help, -h       Show this help message.

ARGUMENTS:
    project_dir      Root directory of the project to index.
    db_path          Output database path (default: <project_dir>/.codescope/codescope.db).

ENVIRONMENT:
    CODESCOPE          Path to codescope binary (auto-detected if unset).
    CODESCOPE_WORKERS  Override total worker count.
    CODESCOPE_PARALLEL Override max concurrent modules.

EXAMPLES:
    # Fast index using all cores (CI/CD):
    codescope-parallel.sh --fast /path/to/large/project

    # Balanced index (default, uses 75% of cores):
    codescope-parallel.sh /path/to/project

    # Slow background index:
    codescope-parallel.sh --slow /path/to/project /tmp/index.db

    # Custom worker count:
    CODESCOPE_WORKERS=16 codescope-parallel.sh /path/to/project
EOF
}

# ── Parse arguments ────────────────────────────────────────────────
MODE="balanced"
PROJECT_DIR=""
DB_PATH=""

while [ $# -gt 0 ]; do
    case "$1" in
        --fast|-f)      MODE="fast"; shift ;;
        --balanced|-b)  MODE="balanced"; shift ;;
        --slow|-s)      MODE="slow"; shift ;;
        --help|-h)      print_help; exit 0 ;;
        --)             shift; break ;;
        -*)             echo "Error: unknown option: $1" >&2; print_help; exit 1 ;;
        *)
            if [ -z "$PROJECT_DIR" ]; then
                PROJECT_DIR="$1"
            elif [ -z "$DB_PATH" ]; then
                DB_PATH="$1"
            else
                echo "Error: too many arguments" >&2; print_help; exit 1
            fi
            shift
            ;;
    esac
done

if [ -z "$PROJECT_DIR" ]; then
    echo "Error: project_dir is required" >&2
    echo ""
    print_help
    exit 1
fi

if [ ! -d "$PROJECT_DIR" ]; then
    echo "Error: directory not found: $PROJECT_DIR" >&2
    exit 1
fi

# Resolve project_dir to absolute path (worker subprocesses may have different CWD)
PROJECT_DIR="$(cd "$PROJECT_DIR" && pwd)"

# Default DB path: project_dir/.codescope/codescope.db
if [ -z "$DB_PATH" ]; then
    DB_PATH="$PROJECT_DIR/.codescope/codescope.db"
fi

# ── Locate binary and tools ────────────────────────────────────────
CODESCOPE_BIN="$(find_codescope)" || {
    echo "Error: codescope binary not found." >&2
    echo "  Set CODESCOPE env var, or place 'codescope' alongside this script or in PATH." >&2
    exit 1
}

JSON_PARSER="$(detect_json_parser)"
if [ -z "$JSON_PARSER" ]; then
    echo "Error: neither python3 nor jq found — need one for JSON parsing." >&2
    exit 1
fi

if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "Error: sqlite3 CLI not found — need it for DB merging." >&2
    exit 1
fi

# ── Determine worker count based on mode ───────────────────────────
CPU_CORES="$(detect_cpu_cores)"
case "$MODE" in
    fast)
        TOTAL_WORKERS="${CODESCOPE_WORKERS:-$CPU_CORES}"
        INDEX_MODE="fast"
        ;;
    balanced)
        # Use 75% of cores, at least 2, at most CPU_CORES
        BALANCED=$(( CPU_CORES * 3 / 4 ))
        [ "$BALANCED" -lt 2 ] && BALANCED=2
        TOTAL_WORKERS="${CODESCOPE_WORKERS:-$BALANCED}"
        INDEX_MODE=""
        ;;
    slow)
        TOTAL_WORKERS="${CODESCOPE_WORKERS:-1}"
        INDEX_MODE=""
        ;;
    *)
        echo "Error: unknown mode: $MODE" >&2; exit 1
        ;;
esac

# Max concurrent modules (default = total workers)
PARALLEL="${CODESCOPE_PARALLEL:-$TOTAL_WORKERS}"
# Cap parallel modules to avoid excessive memory for large projects
[ "$PARALLEL" -gt "$TOTAL_WORKERS" ] && PARALLEL="$TOTAL_WORKERS"

# ── Temp directory for module DBs and state ────────────────────────
DB_PREFIX="$(dirname "$DB_PATH")/.codescope_parallel_$(date +%s)_$$"
METRICS_FILE="${DB_PREFIX}_METRICS.txt"
QUARANTINE_DIR="${DB_PREFIX}_quarantine"
MODULE_STATE_DIR="${DB_PREFIX}_state"
mkdir -p "$QUARANTINE_DIR" "$MODULE_STATE_DIR"

# ── All supported source file extensions ───────────────────────────
# Used by the quarantine binary-search to find crashing files.
SOURCE_EXTENSIONS=(
    "*.c" "*.h" "*.cpp" "*.hpp" "*.cc" "*.cxx" "*.hh" "*.hxx"
    "*.rs" "*.py" "*.go" "*.java"
    "*.js" "*.jsx" "*.ts" "*.tsx"
)

# ── Banner ─────────────────────────────────────────────────────────
echo "=========================================="
echo "  CodeScope Parallel Module Indexer"
echo "  Dynamic Worker Dispatch + Quarantine"
echo "=========================================="
echo "  Project:     $PROJECT_DIR"
echo "  Binary:      $CODESCOPE_BIN"
echo "  Mode:        $MODE"
echo "  CPU cores:   $CPU_CORES"
echo "  Workers:     $TOTAL_WORKERS"
echo "  Parallel:    $PARALLEL modules"
echo "  Index mode:  ${INDEX_MODE:-default}"
echo "  Output DB:   $DB_PATH"
echo "  JSON parser: $JSON_PARSER"
echo "  Metrics:     $METRICS_FILE"
echo "=========================================="
echo ""

# ── Metrics ────────────────────────────────────────────────────────
log_metric() { echo "$(date '+%H:%M:%S') | $1 | $2" >> "$METRICS_FILE"; }

log_system_metrics() {
    local label="$1"
    local rss cpu
    rss=$(ps aux 2>/dev/null | grep codescope | grep -v grep | awk '{sum+=$6} END {printf "%.0f", sum/1024}')
    cpu=$(ps aux 2>/dev/null | grep codescope | grep -v grep | awk '{sum+=$3} END {printf "%.0f", sum}')
    log_metric "SYS:${label}" "rss_mb=${rss:-0} cpu_pct=${cpu:-0}"
}

log_metric "CONFIG" "mode=${MODE} total_workers=${TOTAL_WORKERS} parallel=${PARALLEL} cpu=${CPU_CORES} index_mode=${INDEX_MODE:-default}"

# ── Step 1: Discover ───────────────────────────────────────────────
echo "[1/3] Discovering project structure..."
T0=$(date +%s)
DISCOVER_JSON=$("$CODESCOPE_BIN" discover "$PROJECT_DIR" 2>/dev/null)
TOTAL_FILES=$(echo "$DISCOVER_JSON" | json_get "total_files")
TOTAL_FILES=${TOTAL_FILES:-0}
echo "  Total source files: $TOTAL_FILES"

# Parse modules into a temp file (name:count)
TMP_MODULES=$(mktemp)
echo "$DISCOVER_JSON" | parse_modules > "$TMP_MODULES" || {
    echo "Error: failed to parse discover output" >&2
    rm -f "$TMP_MODULES"
    exit 1
}

MODULE_COUNT=$(wc -l < "$TMP_MODULES" | tr -d ' ')
echo "  Modules found: $MODULE_COUNT"

if [ "$MODULE_COUNT" -eq 0 ] || [ "$TOTAL_FILES" -eq 0 ] || [ "$TOTAL_FILES" = "" ]; then
    echo "  No source files found. Nothing to index."
    rm -f "$TMP_MODULES"
    exit 0
fi

echo "  Modules (sorted by size):"
while IFS=: read -r name count; do
    [ -z "$name" ] && continue
    printf "    %-24s %5d files\n" "$name" "$count"
done < "$TMP_MODULES"
T1=$(date +%s)
log_metric "DISCOVER" "${TOTAL_FILES} files, ${MODULE_COUNT} modules in $((T1-T0))s"
echo ""

# ── Binary search for crashing file ────────────────────────────────
find_crashing_file() {
    local module_dir="$1" module_name="$2"
    local temp_db="${DB_PREFIX}_${module_name}_q.db"
    local all_files=$(mktemp)

    # Build find expression for all supported extensions
    local find_expr=()
    for ext in "${SOURCE_EXTENSIONS[@]}"; do
        find_expr+=(-o -name "$ext")
    done
    find_expr=("${find_expr[@]:1}")  # Remove leading -o

    find "$module_dir" -type f \( "${find_expr[@]}" \) 2>/dev/null | sort > "$all_files"
    local total
    total=$(wc -l < "$all_files" | tr -d ' ')
    [ "$total" -eq 0 ] && { rm -f "$all_files"; echo ""; return; }

    echo "  [QUARANTINE] $module_name: binary searching $total files..."
    local left=0 right=$((total - 1)) crash_file=""

    while [ "$left" -le "$right" ]; do
        local mid=$(( (left + right) / 2 ))
        local test_list=$(mktemp)
        sed -n "$((left + 1)),$((mid + 1))p" "$all_files" > "$test_list"
        local test_dir="${DB_PREFIX}_${module_name}_bs"
        rm -rf "$test_dir"; mkdir -p "$test_dir"
        while IFS= read -r f; do
            local rel="${f#$module_dir/}"
            mkdir -p "$test_dir/$(dirname "$rel")"
            ln -sf "$f" "$test_dir/$rel"
        done < "$test_list"
        rm -f "$temp_db" 2>/dev/null
        local env_args=()
        [ -n "$INDEX_MODE" ] && env_args+=("CODESCOPE_INDEX_MODE=$INDEX_MODE")
        env_args+=("CODESCOPE_DB_PATH=$temp_db" "CODESCOPE_WORKERS=1")
        # Fix: capture real exit code without || true swallowing it.
        # This is the standard bash idiom for "get exit code without set -e killing us".
        local ec=0
        env "${env_args[@]}" timeout 120 "$CODESCOPE_BIN" worker "$temp_db" "$test_dir" "" "q-${module_name}" 0 >/dev/null 2>&1 || ec=$?
        rm -rf "$test_dir" "$test_list"
        if [ "$ec" -eq 0 ] || [ "$ec" -eq 124 ]; then
            left=$((mid + 1))
        else
            crash_file=$(sed -n "$((mid + 1))p" "$all_files")
            right=$mid
        fi
    done
    rm -f "$all_files" "$temp_db" 2>/dev/null
    [ -n "$crash_file" ] && [ -f "$crash_file" ] && echo "$crash_file" || echo ""
}

# ── Index a module ─────────────────────────────────────────────────
index_module() {
    local name="$1" count="$2" workers="$3"
    local module_dir="$PROJECT_DIR/$name"
    local module_db="${DB_PREFIX}_${name}.db"
    local module_log="${DB_PREFIX}_${name}.log"
    local quarantine_list="${QUARANTINE_DIR}/${name}.txt"

    # Apply quarantine: copy module dir without crashing files
    if [ -f "$quarantine_list" ]; then
        local clean_dir="${DB_PREFIX}_${name}_clean"
        rm -rf "$clean_dir"
        mkdir -p "$clean_dir"
        # Portable copy (no rsync dependency)
        cp -r "$module_dir/." "$clean_dir/" 2>/dev/null || true
        while IFS= read -r pattern; do
            [ -z "$pattern" ] && continue
            find "$clean_dir" -path "*/$(basename "$pattern")" -delete 2>/dev/null || true
        done < "$quarantine_list"
        local qcount
        qcount=$(wc -l < "$quarantine_list" 2>/dev/null | tr -d ' ' || echo 0)
        [ "$qcount" -gt 0 ] 2>/dev/null && echo "  [QUARANTINE] $name: skipping $qcount files"
        module_dir="$clean_dir"
    fi

    local t0
    t0=$(date +%s)
    local env_args=()
    [ -n "$INDEX_MODE" ] && env_args+=("CODESCOPE_INDEX_MODE=$INDEX_MODE")
    env_args+=("CODESCOPE_DB_PATH=$module_db" "CODESCOPE_WORKERS=$workers")
    # Fix: capture real exit code without || true swallowing it.
    local ec=0
    env "${env_args[@]}" timeout 600 "$CODESCOPE_BIN" worker \
        "$module_db" "$module_dir" "" "$name" 0 \
        > "$module_log" 2>&1 || ec=$?
    local dur=$(( $(date +%s) - t0 ))
    local nodes
    nodes=$(sqlite3 "$module_db" "SELECT COUNT(*) FROM graph_nodes;" 2>/dev/null || echo 0)
    echo "$name:$ec:$nodes:$dur:$workers"
}

# ── Step 2: Dynamic worker dispatch ────────────────────────────────
echo "[2/3] Dynamic worker dispatch (${TOTAL_WORKERS} total workers)..."
echo ""

# Read all modules into an array
declare -a MODULE_QUEUE=()
while IFS=: read -r name count; do
    [ -z "$name" ] && continue
    MODULE_QUEUE+=("$name:$count")
done < "$TMP_MODULES"
rm -f "$TMP_MODULES"

SUMMARY_FILE="${DB_PREFIX}_SUMMARY.txt"
rm -f "$SUMMARY_FILE"

# Start metrics background monitor
( while true; do log_system_metrics "LIVE"; sleep 30; done ) &
METRICS_PID=$!

rm -f "${MODULE_STATE_DIR}"/*

ACTIVE=0
NEXT_INDEX=0
TOTAL_MODULES=${#MODULE_QUEUE[@]}

# Initial allocation: start with 1 worker per module, up to PARALLEL modules
AVAILABLE_WORKERS=$TOTAL_WORKERS
INITIAL_WORKERS=1

echo "  Initial dispatch: 1 worker per module, up to $PARALLEL modules..."

start_module() {
    local idx=$1
    if [ "$idx" -ge "$TOTAL_MODULES" ]; then return 1; fi
    local entry="${MODULE_QUEUE[$idx]}"
    local name count
    IFS=: read -r name count <<< "$entry"

    local alloc=$INITIAL_WORKERS
    [ "$alloc" -gt "$AVAILABLE_WORKERS" ] && alloc=$AVAILABLE_WORKERS
    [ "$alloc" -lt 1 ] && alloc=1

    echo "$alloc" > "${MODULE_STATE_DIR}/${name}_workers"
    AVAILABLE_WORKERS=$((AVAILABLE_WORKERS - alloc))

    (
        result=$(index_module "$name" "$count" "$alloc")
        echo "$result" >> "$SUMMARY_FILE"
        local mname ec nodes dur wkrs
        IFS=: read -r mname ec nodes dur wkrs <<< "$result"
        echo "  [DONE] $mname -> ${nodes} nodes ${dur}s (${wkrs} workers)"
        log_metric "MODULE:${mname}" "exit=${ec} nodes=${nodes} duration=${dur}s workers=${wkrs}"
    ) &
    local pid=$!
    echo "$pid" > "${MODULE_STATE_DIR}/${name}_pid"
    ACTIVE=$((ACTIVE + 1))
    NEXT_INDEX=$((idx + 1))
    log_metric "START:${name}" "workers=${alloc} files=${count}"
}

# Start initial batch
while [ "$NEXT_INDEX" -lt "$TOTAL_MODULES" ] && [ "$ACTIVE" -lt "$PARALLEL" ]; do
    start_module "$NEXT_INDEX"
done

# Main dispatch loop: when a module finishes, start next or rebalance
while [ "$ACTIVE" -gt 0 ]; do
    for pid_file in "${MODULE_STATE_DIR}"/*_pid; do
        [ -f "$pid_file" ] || continue
        pid=$(cat "$pid_file" 2>/dev/null)
        [ -z "$pid" ] && continue

        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true

            name=$(basename "$pid_file" _pid)
            workers=$(cat "${MODULE_STATE_DIR}/${name}_workers" 2>/dev/null || echo 1)
            rm -f "${MODULE_STATE_DIR}/${name}_pid" "${MODULE_STATE_DIR}/${name}_workers"

            ACTIVE=$((ACTIVE - 1))
            AVAILABLE_WORKERS=$((AVAILABLE_WORKERS + workers))

            # Start next module if any remain
            if [ "$NEXT_INDEX" -lt "$TOTAL_MODULES" ]; then
                start_module "$NEXT_INDEX"
            else
                # No more modules — rebalance: restart largest remaining
                # with more workers to finish faster.
                max_files=0; max_name=""
                for sf in "${MODULE_STATE_DIR}"/*_pid; do
                    [ -f "$sf" ] || continue
                    rname=$(basename "$sf" _pid)
                    for entry in "${MODULE_QUEUE[@]}"; do
                        IFS=: read -r ename ecount <<< "$entry"
                        if [ "$ename" = "$rname" ] && [ "$ecount" -gt "$max_files" ]; then
                            max_files=$ecount
                            max_name=$rname
                        fi
                    done
                done

                if [ -n "$max_name" ] && [ "$AVAILABLE_WORKERS" -gt 0 ]; then
                    old_workers=$(cat "${MODULE_STATE_DIR}/${max_name}_workers" 2>/dev/null || echo 1)
                    new_workers=$((old_workers + AVAILABLE_WORKERS))
                    echo "  [REBALANCE] $max_name: ${old_workers}->${new_workers} workers"
                    log_metric "REBALANCE:${max_name}" "old=${old_workers} new=${new_workers}"

                    # Kill old process and restart with more workers
                    old_pid=$(cat "${MODULE_STATE_DIR}/${max_name}_pid" 2>/dev/null)
                    if [ -n "$old_pid" ]; then
                        kill "$old_pid" 2>/dev/null || true
                        wait "$old_pid" 2>/dev/null || true
                        rm -f "${MODULE_STATE_DIR}/${max_name}_pid"
                        ACTIVE=$((ACTIVE - 1))
                    fi

                    AVAILABLE_WORKERS=0
                    echo "$new_workers" > "${MODULE_STATE_DIR}/${max_name}_workers"

                    fcount=0
                    for entry in "${MODULE_QUEUE[@]}"; do
                        IFS=: read -r ename ecount <<< "$entry"
                        [ "$ename" = "$max_name" ] && fcount=$ecount && break
                    done

                    (
                        result=$(index_module "$max_name" "$fcount" "$new_workers")
                        echo "$result" >> "$SUMMARY_FILE"
                        local mname ec nodes dur wkrs
                        IFS=: read -r mname ec nodes dur wkrs <<< "$result"
                        echo "  [DONE] $mname -> ${nodes} nodes ${dur}s (${wkrs} workers)"
                        log_metric "MODULE:${mname}" "exit=${ec} nodes=${nodes} duration=${dur}s workers=${wkrs}"
                    ) &
                    new_pid=$!
                    echo "$new_pid" > "${MODULE_STATE_DIR}/${max_name}_pid"
                    ACTIVE=$((ACTIVE + 1))
                fi
            fi
            break  # restart loop after state change
        fi
    done
    sleep 0.5
done

kill "$METRICS_PID" 2>/dev/null || true
wait "$METRICS_PID" 2>/dev/null || true

# ── Step 3: Per-file quarantine for failed modules ─────────────────
echo ""
echo "[3/3] Per-file quarantine for failed modules..."

FAILED_MODULES=()
if [ -f "$SUMMARY_FILE" ]; then
    while IFS=: read -r name exit_code nodes duration workers; do
        if [ "$exit_code" != "0" ] || [ "$nodes" -eq 0 ] 2>/dev/null; then
            FAILED_MODULES+=("$name")
        fi
    done < "$SUMMARY_FILE"
fi

if [ ${#FAILED_MODULES[@]} -gt 0 ]; then
    echo "  Failed modules: ${FAILED_MODULES[*]}"
    for module_name in "${FAILED_MODULES[@]}"; do
        echo "  -- Processing $module_name --"
        module_dir="$PROJECT_DIR/$module_name"
        quarantine_list="${QUARANTINE_DIR}/${module_name}.txt"
        max_iter=10; iter=1
        while [ "$iter" -le "$max_iter" ]; do
            qcount=$(wc -l < "$quarantine_list" 2>/dev/null | tr -d ' ' || echo 0)
            echo "    Attempt $iter (quarantined: $qcount)..."
            crash_file=$(find_crashing_file "$module_dir" "${module_name}")
            [ -z "$crash_file" ] && echo "    OK - No more crashes!" && break
            echo "    FAIL - Crashing file: $(basename "$crash_file")"
            echo "$crash_file" >> "$quarantine_list"
            log_metric "QUARANTINE:${module_name}" "$(basename "$crash_file")"
            iter=$((iter + 1))
        done
        # Final attempt with quarantine applied
        result=$(index_module "$module_name" "0" "1")
        IFS=: read -r mname ec nodes dur wkrs <<< "$result"
        echo "  [FINAL] $module_name -> exit=$ec nodes=$nodes ${dur}s"
        log_metric "FINAL:${module_name}" "exit=${ec} nodes=${nodes} duration=${dur}s"
    done
else
    echo "  No failed modules!"
fi

# ── Final Summary ──────────────────────────────────────────────────
echo ""
echo "=========================================="
echo "  Final Results"
echo "=========================================="

SUCCESS=0 FAIL=0 TOTAL_NODES=0 TOTAL_TIME=0
if [ -f "$SUMMARY_FILE" ]; then
    while IFS=: read -r name exit_code nodes duration workers; do
        if [ "$exit_code" = "0" ] && [ "$nodes" -gt 0 ] 2>/dev/null; then
            SUCCESS=$((SUCCESS + 1))
            TOTAL_NODES=$((TOTAL_NODES + nodes))
            TOTAL_TIME=$((TOTAL_TIME + duration))
            printf "  OK  %-24s %7d nodes  %4ds  (%d workers)\n" "$name" "$nodes" "$duration" "$workers"
        else
            FAIL=$((FAIL + 1))
            printf "  FAIL %-24s exit=%s\n" "$name" "$exit_code"
        fi
    done < "$SUMMARY_FILE"
fi
echo ""
echo "  Successful: $SUCCESS / $((SUCCESS + FAIL)) modules"
echo "  Total nodes: $TOTAL_NODES"
echo "  Total time: ${TOTAL_TIME}s"
quarantined_count=$(find "$QUARANTINE_DIR" -name "*.txt" -exec wc -l {} + 2>/dev/null | tail -1 | awk '{print $1}')
echo "  Quarantined files: ${quarantined_count:-0}"

log_metric "FINAL" "success=${SUCCESS} fail=${FAIL} total_nodes=${TOTAL_NODES} total_time=${TOTAL_TIME}s"
log_system_metrics "FINAL"

# ── Merge all module DBs into a single project DB ──────────────────
echo ""
echo "  Merging module databases..."
FINAL_DB="$DB_PATH"
mkdir -p "$(dirname "$FINAL_DB")"
rm -f "$FINAL_DB"

FIRST=true
for db in "${DB_PREFIX}"_*.db; do
    [ "$db" = "${DB_PREFIX}_merged.db" ] && continue
    [ ! -f "$db" ] && continue
    MODULE=$(basename "$db" .db | sed "s/^${DB_PREFIX##*/}_//")
    if [ "$FIRST" = true ]; then
        cp "$db" "$FINAL_DB"
        FIRST=false
        echo "    Base: $MODULE"
    else
        echo "    Merge: $MODULE"
        sqlite3 "$FINAL_DB" <<-EOSQL
			ATTACH DATABASE '$db' AS other;
			INSERT OR IGNORE INTO graph_nodes SELECT * FROM other.graph_nodes;
			INSERT OR IGNORE INTO graph_edges SELECT * FROM other.graph_edges;
			INSERT OR IGNORE INTO entity SELECT * FROM other.entity;
			INSERT OR IGNORE INTO relation SELECT * FROM other.relation;
			DETACH DATABASE other;
		EOSQL
    fi
done

if [ -f "$FINAL_DB" ]; then
    TOTAL=$(sqlite3 "$FINAL_DB" "SELECT COUNT(*) FROM graph_nodes" 2>/dev/null || echo 0)
    EDGES=$(sqlite3 "$FINAL_DB" "SELECT COUNT(*) FROM graph_edges" 2>/dev/null || echo 0)
    echo "    Merged DB: $FINAL_DB"
    echo "    Total nodes: $TOTAL"
    echo "    Total edges: $EDGES"
fi

# ── Cleanup temp files (keep DBs + logs for debugging) ─────────────
# Uncomment the next line to auto-clean temp files after successful merge:
# rm -rf "${DB_PREFIX}"_*.db "${DB_PREFIX}"_*.log "$MODULE_STATE_DIR" "$QUARANTINE_DIR"

echo ""
echo "  Metrics:     $METRICS_FILE"
echo "  Module DBs:  ${DB_PREFIX}_*.db"
echo "  Module logs: ${DB_PREFIX}_*.log"
echo "=========================================="
