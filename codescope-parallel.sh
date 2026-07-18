#!/bin/bash
# ── codescope-parallel — Parallel module indexer with dynamic worker dispatch ──
#
# Strategy:
#   1. `codescope discover` to scan project structure
#   2. Workers are allocated proportionally to module file count
#   3. When a module finishes, its workers are reassigned to remaining modules
#   4. Per-file quarantine: crashed modules get binary-search retry
#   5. All metrics (memory, CPU, time) recorded to METRICS file
#
# Usage:
#   CODESCOPE_WORKERS=8 CODESCOPE_PARALLEL=8 ./codescope-parallel.sh <dir> [db_prefix]

set -euo pipefail

CODESCOPE="${CODESCOPE:-./target/release/codescope}"
PROJECT_DIR="${1:-}"
DB_PREFIX="${2:-/tmp/parallel_$(date +%s)}"
TOTAL_WORKERS="${CODESCOPE_WORKERS:-8}"
PARALLEL="${CODESCOPE_PARALLEL:-8}"  # max concurrent modules
GRAMMARS_DIR="${GRAMMARS_DIR:-engine/grammars}"

# All supported source file extensions (matching FilterPolicy::isSourceFile)
SOURCE_EXTENSIONS=(
    "*.c" "*.h" "*.cpp" "*.hpp" "*.cc" "*.cxx" "*.hh" "*.hxx"
    "*.rs" "*.go" "*.py" "*.java" "*.kt" "*.kts"
    "*.js" "*.jsx" "*.ts" "*.tsx" "*.mjs" "*.cjs" "*.mts" "*.cts"
    "*.swift" "*.rb" "*.php" "*.cs" "*.scala"
    "*.zig" "*.mojo" "*.vue" "*.svelte"
)

if [ -z "$PROJECT_DIR" ] || [ "$1" = "-h" ]; then
    echo "Usage: CODESCOPE_WORKERS=8 CODESCOPE_PARALLEL=8 $0 <project_dir> [db_prefix]"
    exit 1
fi
if [ ! -d "$PROJECT_DIR" ]; then
    echo "Error: directory not found: $PROJECT_DIR"
    exit 1
fi

METRICS_FILE="${DB_PREFIX}_METRICS.txt"
QUARANTINE_DIR="${DB_PREFIX}_quarantine"
mkdir -p "$QUARANTINE_DIR"

echo "=========================================="
echo "  CodeScope Parallel Module Indexer v3"
echo "  Dynamic Worker Dispatch + Per-File Quarantine"
echo "=========================================="
echo "  Project: $PROJECT_DIR"
echo "  Total workers: $TOTAL_WORKERS"
echo "  Max parallel modules: $PARALLEL"
echo "  Metrics: $METRICS_FILE"
echo "=========================================="
echo ""

# ── Metrics ──────────────────────────────────────────────────────
log_metric() { echo "$(date '+%H:%M:%S') | $1 | $2" >> "$METRICS_FILE"; }

log_system_metrics() {
    local label="$1"
    local rss cpu
    rss=$(ps aux | grep codescope | grep -v grep | awk '{sum+=$6} END {printf "%.0f", sum/1024}')
    cpu=$(ps aux | grep codescope | grep -v grep | awk '{sum+=$3} END {printf "%.0f", sum}')
    log_metric "SYS:${label}" "rss_mb=${rss:-0} cpu_pct=${cpu:-0}"
}

log_metric "CONFIG" "total_workers=${TOTAL_WORKERS} parallel=${PARALLEL}"

# ── Step 1: Discover ─────────────────────────────────────────────
echo "[1/3] Discovering project structure..."
T0=$(date +%s)
DISCOVER_JSON=$("$CODESCOPE" discover "$PROJECT_DIR" 2>/dev/null)
TOTAL_FILES=$(echo "$DISCOVER_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin)['total_files'])")
echo "  Total source files: $TOTAL_FILES"

# Parse modules into a temp file (name:count)
TMP_MODULES=$(mktemp)
echo "$DISCOVER_JSON" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for name, count in sorted(data['modules'].items(), key=lambda x: -x[1]):
    print(f'{name}:{count}')
" > "$TMP_MODULES"

echo "  Modules (sorted by size):"
while IFS=: read -r name count; do
    printf "    %-20s %5d files\n" "$name" "$count"
done < "$TMP_MODULES"
T1=$(date +%s)
log_metric "DISCOVER" "${TOTAL_FILES} files in $((T1-T0))s"
echo ""

# ── Binary search for crashing file ──────────────────────────────
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
            local rel="${f#$module_dir/}"; mkdir -p "$test_dir/$(dirname "$rel")"
            ln -sf "$f" "$test_dir/$rel"
        done < "$test_list"
        rm -f "$temp_db" "$temp_db"-wal "$temp_db"-shm "$temp_db"-journal 2>/dev/null
        # Capture the real exit code via `|| ec=$?` — `|| true` would
        # force $? to 0 and make the binary search below always take
        # the "slice OK" branch (ec 0 or timeout 124), never reducing
        # `right`, so crash_file would stay empty and quarantine die.
        local ec=0
        GRAMMARS_DIR="$GRAMMARS_DIR" CODESCOPE_DB_PATH="$temp_db" CODESCOPE_INDEX_MODE=fast \
            CODESCOPE_WORKERS=1 timeout 120 "$CODESCOPE" worker "$temp_db" "$test_dir" "" "q-${module_name}" 0 >/dev/null 2>&1 || ec=$?
        rm -rf "$test_dir" "$test_list"
        if [ "$ec" -eq 0 ] || [ "$ec" -eq 124 ]; then
            left=$((mid + 1))
        else
            crash_file=$(sed -n "$((mid + 1))p" "$all_files")
            right=$mid
        fi
    done
    rm -f "$all_files" "$temp_db" "$temp_db"-wal "$temp_db"-shm "$temp_db"-journal 2>/dev/null
    [ -n "$crash_file" ] && [ -f "$crash_file" ] && echo "$crash_file" || echo ""
}

# ── Index a module ────────────────────────────────────────────────
index_module() {
    local name="$1" count="$2" workers="$3"
    local module_dir="$PROJECT_DIR/$name"
    local module_db="${DB_PREFIX}_${name}.db" module_log="${DB_PREFIX}_${name}.log"
    local quarantine_list="${QUARANTINE_DIR}/${name}.txt"

    # Apply quarantine: copy module dir without crashing files
    if [ -f "$quarantine_list" ]; then
        local clean_dir="${DB_PREFIX}_${name}_clean"
        rm -rf "$clean_dir"
        mkdir -p "$clean_dir"
        rsync -a --exclude-from="$quarantine_list" "$module_dir/" "$clean_dir/" 2>/dev/null || true
        while IFS= read -r pattern; do
            [ -z "$pattern" ] && continue
            find "$clean_dir" -path "*/$(basename "$pattern")" -delete 2>/dev/null || true
        done < "$quarantine_list"
        local qcount=$(wc -l < "$quarantine_list" 2>/dev/null || echo 0)
        [ "$qcount" -gt 0 ] && echo "  [QUARANTINE] $name: skipping $qcount files"
        module_dir="$clean_dir"
    fi

    local t0=$(date +%s)
    # Build argv as an array so paths with spaces survive unquoted
    # expansion. The 5 positional worker args are: db, dir, lang, name, id.
    local worker_args=(
        "$module_db" "$module_dir" "" "linux-${name}" 0
    )
    # Capture the real exit code via `|| ec=$?` — `|| true` would force
    # $? to 0, hide failed modules from the FAILED_MODULES gate below
    # (which routes crashes to quarantine), and break the SUMMARY record.
    local ec=0
    GRAMMARS_DIR="$GRAMMARS_DIR" CODESCOPE_DB_PATH="$module_db" \
        CODESCOPE_INDEX_MODE=fast CODESCOPE_WORKERS="$workers" \
        timeout 600 "$CODESCOPE" worker "${worker_args[@]}" \
        > "$module_log" 2>&1 || ec=$?
    local dur=$(( $(date +%s) - t0 ))
    local nodes=$(sqlite3 "$module_db" "SELECT COUNT(*) FROM graph_nodes;" 2>/dev/null || echo 0)
    echo "$name:$ec:$nodes:$dur:$workers"
}

# ── Step 2: Proportional worker allocation (no rebalance) ──────────
# Each module gets parse workers proportional to its file count. No
# rebalancing — small modules use the in-memory bulk path and don't benefit
# from extra workers; large modules are already streaming. Killing/restarting
# modules caused WAL contention ("DB lock conflict") under the old scheme, so
# we start each module exactly once with a fixed allocation.
echo "[2/3] Proportional worker allocation (${TOTAL_WORKERS} total workers)..."
echo ""

# Read modules (name:count) into an allocation array with proportional workers.
declare -a MODULE_ALLOC=()  # "name:count:workers"
TOTAL_FILES_SUM=0
while IFS=: read -r name count; do
    [ -z "$name" ] && continue
    TOTAL_FILES_SUM=$((TOTAL_FILES_SUM + count))
done < "$TMP_MODULES"

[ "$TOTAL_FILES_SUM" -eq 0 ] && TOTAL_FILES_SUM=1
while IFS=: read -r name count; do
    [ -z "$name" ] && continue
    # ceil(count * TOTAL_WORKERS / TOTAL_FILES_SUM), min 1.
    alloc=$(( (count * TOTAL_WORKERS + TOTAL_FILES_SUM - 1) / TOTAL_FILES_SUM ))
    [ "$alloc" -lt 1 ] && alloc=1
    MODULE_ALLOC+=("$name:$count:$alloc")
done < "$TMP_MODULES"

rm -f "${DB_PREFIX}_SUMMARY.txt"

# Start metrics background monitor (kept — useful for profiling).
# Self-terminates when the stop sentinel is created (no process killing).
METRICS_STOP="${DB_PREFIX}_metrics_stop"
rm -f "$METRICS_STOP"
( while [ ! -f "$METRICS_STOP" ]; do log_system_metrics "LIVE"; sleep 30; done ) &

# Launch modules with bounded concurrency (PARALLEL concurrent at most).
# A simple token gate: track active PIDs; when one finishes, launch the next.
declare -a ACTIVE_PIDS=()
ACTIVE=0
TOTAL_MODULES=${#MODULE_ALLOC[@]}

echo "  Launching $TOTAL_MODULES modules (max $PARALLEL concurrent)..."
for entry in "${MODULE_ALLOC[@]}"; do
    IFS=: read -r name count alloc <<< "$entry"

    # Wait until a slot frees up if we're at the concurrency limit.
    # `wait -n` blocks until ANY background module job exits.
    while [ "$ACTIVE" -ge "$PARALLEL" ]; do
        wait -n 2>/dev/null || sleep 0.5
        ACTIVE=$((ACTIVE - 1))
    done

    (
        result=$(index_module "$name" "$count" "$alloc")
        echo "$result" >> "${DB_PREFIX}_SUMMARY.txt"
        IFS=: read -r mname ec nodes dur wkrs <<< "$result"
        echo "  [DONE] $mname → ${nodes} nodes ${dur}s (${wkrs} workers)"
        log_metric "MODULE:${mname}" "exit=${ec} nodes=${nodes} duration=${dur}s workers=${wkrs}"
    ) &
    ACTIVE=$((ACTIVE + 1))
    log_metric "START:${name}" "workers=${alloc} files=${count}"
done

# Reap remaining module processes.
wait 2>/dev/null || true

# Signal the metrics monitor to stop (self-terminating loop, no kill).
touch "$METRICS_STOP" 2>/dev/null || true

# ── Step 3: Per-file quarantine for failed modules ───────────────
echo ""
echo "[3/3] Per-file quarantine for failed modules..."

FAILED_MODULES=()
if [ -f "${DB_PREFIX}_SUMMARY.txt" ]; then
    while IFS=: read -r name exit_code nodes duration workers; do
        if [ "$exit_code" != "0" ] || [ "$nodes" -eq 0 ] 2>/dev/null; then
            FAILED_MODULES+=("$name")
        fi
    done < "${DB_PREFIX}_SUMMARY.txt"
fi

if [ ${#FAILED_MODULES[@]} -gt 0 ]; then
    echo "  Failed modules: ${FAILED_MODULES[*]}"
    for module_name in "${FAILED_MODULES[@]}"; do
        echo "  ── Processing $module_name ──"
        module_dir="$PROJECT_DIR/$module_name"
        quarantine_list="${QUARANTINE_DIR}/${module_name}.txt"
        max_iter=10; iter=1
        while [ "$iter" -le "$max_iter" ]; do
            qcount=$(wc -l < "$quarantine_list" 2>/dev/null || echo 0)
            echo "    Attempt $iter (quarantined: $qcount)..."
            crash_file=$(find_crashing_file "$module_dir" "${module_name}")
            [ -z "$crash_file" ] && echo "    ✅ No more crashes!" && break
            echo "    ❌ Crashing file: $(basename "$crash_file")"
            echo "$crash_file" >> "$quarantine_list"
            log_metric "QUARANTINE:${module_name}" "$(basename $crash_file)"
            iter=$((iter + 1))
        done
        # Final attempt with quarantine
        result=$(index_module "$module_name" "0" "1")
        IFS=: read -r mname ec nodes dur wkrs <<< "$result"
        echo "  [FINAL] $module_name → exit=$ec nodes=$nodes ${dur}s"
        log_metric "FINAL:${module_name}" "exit=${ec} nodes=${nodes} duration=${dur}s"
    done
else
    echo "  No failed modules!"
fi

# ── Final Summary ────────────────────────────────────────────────
echo ""
echo "=========================================="
echo "  Final Results"
echo "=========================================="

SUCCESS=0 FAIL=0 TOTAL_NODES=0 TOTAL_TIME=0
if [ -f "${DB_PREFIX}_SUMMARY.txt" ]; then
    while IFS=: read -r name exit_code nodes duration workers; do
        if [ "$exit_code" = "0" ] && [ "$nodes" -gt 0 ] 2>/dev/null; then
            SUCCESS=$((SUCCESS + 1))
            TOTAL_NODES=$((TOTAL_NODES + nodes))
            TOTAL_TIME=$((TOTAL_TIME + duration))
            printf "  ✅ %-20s %7d nodes  %4ds  (%d workers)\n" "$name" "$nodes" "$duration" "$workers"
        else
            FAIL=$((FAIL + 1))
            printf "  ❌ %-20s exit=%s\n" "$name" "$exit_code"
        fi
    done < "${DB_PREFIX}_SUMMARY.txt"
fi
echo ""
echo "  Successful: $SUCCESS / $((SUCCESS + FAIL)) modules"
echo "  Total nodes: $TOTAL_NODES"
echo "  Total time: ${TOTAL_TIME}s"
echo "  Quarantined files: $(find "$QUARANTINE_DIR" -name "*.txt" -exec wc -l {} + 2>/dev/null | tail -1 | awk '{print $1}')"

log_metric "FINAL" "success=${SUCCESS} fail=${FAIL} total_nodes=${TOTAL_NODES} total_time=${TOTAL_TIME}s"
log_system_metrics "FINAL"

# ── Merge all module DBs into a single project DB ──
echo ""
echo "  Merging module databases..."
FINAL_DB="${DB_PREFIX}_merged.db"
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

echo ""
echo "  Metrics: $METRICS_FILE"
echo "  DBs: ${DB_PREFIX}_*.db"
echo "=========================================="