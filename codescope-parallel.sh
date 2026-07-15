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
    find "$module_dir" -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" 2>/dev/null | sort > "$all_files"
    local total=$(wc -l < "$all_files")
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
        rm -f "$temp_db"*.db 2>/dev/null
        GRAMMARS_DIR="$GRAMMARS_DIR" CODESCOPE_DB_PATH="$temp_db" CODESCOPE_INDEX_MODE=fast \
            CODESCOPE_WORKERS=1 timeout 120 "$CODESCOPE" worker "$temp_db" "$test_dir" "" "q-${module_name}" 0 >/dev/null 2>&1 || true
        local ec=$?; rm -rf "$test_dir" "$test_list"
        if [ "$ec" -eq 0 ] || [ "$ec" -eq 124 ]; then
            left=$((mid + 1))
        else
            crash_file=$(sed -n "$((mid + 1))p" "$all_files")
            right=$mid
        fi
    done
    rm -f "$all_files" "$temp_db"*.db 2>/dev/null
    [ -n "$crash_file" ] && [ -f "$crash_file" ] && echo "$crash_file" || echo ""
}

# ── Index a module ───────────────────────────────────────────────
index_module() {
    local name="$1" count="$2" workers="$3" module_dir="$PROJECT_DIR/$name"
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
    GRAMMARS_DIR="$GRAMMARS_DIR" CODESCOPE_DB_PATH="$module_db" \
        CODESCOPE_INDEX_MODE=fast CODESCOPE_WORKERS="$workers" \
        timeout 600 "$CODESCOPE" worker "$module_db" "$module_dir" "" "linux-${name}" 0 \
        > "$module_log" 2>&1 || true
    local ec=$? dur=$(( $(date +%s) - t0 ))
    local nodes=$(sqlite3 "$module_db" "SELECT COUNT(*) FROM graph_nodes;" 2>/dev/null || echo 0)
    echo "$name:$ec:$nodes:$dur:$workers"
}

# ── Step 2: Dynamic worker dispatch ──────────────────────────────
echo "[2/3] Dynamic worker dispatch (${TOTAL_WORKERS} total workers)..."
echo ""

# Read all modules
declare -a MODULE_QUEUE=()  # "name:count"
while IFS=: read -r name count; do
    MODULE_QUEUE+=("$name:$count")
done < "$TMP_MODULES"

rm -f "${DB_PREFIX}_SUMMARY.txt"

# Start metrics background monitor
( while true; do log_system_metrics "LIVE"; sleep 30; done ) &
METRICS_PID=$!

# Track running modules using temp files (bash 3.2 compatible — no declare -A)
MODULE_STATE_DIR="${DB_PREFIX}_state"
mkdir -p "$MODULE_STATE_DIR"
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
    IFS=: read -r name count <<< "$entry"
    
    local alloc=$INITIAL_WORKERS
    [ "$alloc" -gt "$AVAILABLE_WORKERS" ] && alloc=$AVAILABLE_WORKERS
    [ "$alloc" -lt 1 ] && alloc=1
    
    echo "$alloc" > "${MODULE_STATE_DIR}/${name}_workers"
    AVAILABLE_WORKERS=$((AVAILABLE_WORKERS - alloc))
    
    (
        result=$(index_module "$name" "$count" "$alloc")
        echo "$result" >> "${DB_PREFIX}_SUMMARY.txt"
        IFS=: read -r mname ec nodes dur wkrs <<< "$result"
        echo "  [DONE] $mname → ${nodes} nodes ${dur}s (${wkrs} workers)"
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
                # No more modules — rebalance: restart largest remaining with more workers
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
                    echo "  [REBALANCE] $max_name: ${old_workers}→${new_workers} workers"
                    log_metric "REBALANCE:${max_name}" "old=${old_workers} new=${new_workers}"
                    
                    # Kill old process
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
                        echo "$result" >> "${DB_PREFIX}_SUMMARY.txt"
                        IFS=: read -r mname ec nodes dur wkrs <<< "$result"
                        echo "  [DONE] $mname → ${nodes} nodes ${dur}s (${wkrs} workers)"
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