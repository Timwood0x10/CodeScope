#!/bin/bash
# ── codescope-index — File-level parallel indexer (parallel batches) ──
#
# Strategy:
#   1. Discover all source files
#   2. Split into N batches (N = WORKERS)
#   3. Run ALL batches in parallel, each with 1 worker
#   4. If a batch crashes, binary-search to find the crashing file
#   5. Quarantine the crashing file, retry
#   6. Report per-batch results
#
# Usage:
#   CODESCOPE_WORKERS=8 ./codescope-index.sh <project_dir> [output_db]

set -euo pipefail

CODESCOPE="${CODESCOPE:-./target/release/codescope}"
PROJECT_DIR="${1:-}"
OUTPUT_DB="${2:-/tmp/codescope_index.db}"
WORKERS="${CODESCOPE_WORKERS:-8}"  # = number of parallel batches
PARALLEL="${WORKERS}"              # max concurrent batches = WORKERS
GRAMMARS_DIR="${GRAMMARS_DIR:-engine/grammars}"

if [ -z "$PROJECT_DIR" ] || [ "$1" = "-h" ]; then
    echo "Usage: CODESCOPE_WORKERS=8 $0 <project_dir> [output_db]"
    exit 1
fi
if [ ! -d "$PROJECT_DIR" ]; then
    echo "Error: directory not found: $PROJECT_DIR"
    exit 1
fi

BASE="${OUTPUT_DB%.db}"
METRICS_FILE="${BASE}_METRICS.txt"
QUARANTINE_DIR="${BASE}_quarantine"
STATE_DIR="${BASE}_state"
mkdir -p "$QUARANTINE_DIR" "$STATE_DIR"
rm -f "${STATE_DIR}"/*

log_metric() { echo "$(date '+%H:%M:%S') | $1 | $2" >> "$METRICS_FILE"; }
log_system() {
    local rss=$(ps aux | grep codescope | grep -v grep | awk '{sum+=$6} END {printf "%.0f", sum/1024}')
    local cpu=$(ps aux | grep codescope | grep -v grep | awk '{sum+=$3} END {printf "%.0f", sum}')
    log_metric "SYS" "rss_mb=${rss:-0} cpu_pct=${cpu:-0}"
}

echo "=========================================="
echo "  CodeScope Parallel Batch Indexer"
echo "  ${WORKERS} batches × 1 worker = ${WORKERS} parallel"
echo "=========================================="
echo "  Project: $PROJECT_DIR"
echo "  Output: $OUTPUT_DB"
echo "=========================================="
echo ""

# ── Step 1: Discover & split ─────────────────────────────────────
echo "[1/4] Discovering files..."
T0=$(date +%s)
ALL_FILES="${BASE}_files.txt"
find "$PROJECT_DIR" -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \
    -o -name "*.rs" -o -name "*.go" -o -name "*.py" -o -name "*.java" \
    -o -name "*.js" -o -name "*.ts" -o -name "*.tsx" -o -name "*.kt" \
    2>/dev/null | grep -v -E '/(\.git|node_modules|target|build|venv|__pycache__)(/|$)|/[.][^/]*/' \
    > "$ALL_FILES" || true

TOTAL=$(wc -l < "$ALL_FILES" 2>/dev/null || echo 0)
echo "  Candidate files: $TOTAL"
echo ""

echo "[2/4] Splitting into ${WORKERS} batches..."
FILES_PER_BATCH=$(( (TOTAL + WORKERS - 1) / WORKERS ))
rm -f "${BASE}_batch_"*
split -l "$FILES_PER_BATCH" "$ALL_FILES" "${BASE}_batch_"
BATCHES=$(ls "${BASE}_batch_"* 2>/dev/null | sort)
BATCH_COUNT=$(echo "$BATCHES" | wc -l)
echo "  $BATCH_COUNT batches, ~$FILES_PER_BATCH files each"
echo ""

# ── Step 3: Index all batches in parallel ────────────────────────
echo "[3/4] Indexing ${BATCH_COUNT} batches in parallel (${PARALLEL} at a time)..."
echo ""

log_metric "CONFIG" "workers=${WORKERS} files=${TOTAL} batches=${BATCH_COUNT}"

# Start system metrics monitor
( while true; do log_system; sleep 30; done ) &
METRICS_PID=$!

# Index a single batch (1 worker, time-boxed)
index_batch() {
    local batch_file="$1"
    local batch_name=$(basename "$batch_file")
    local batch_db="${BASE}_${batch_name}.db"
    local batch_log="${BASE}_${batch_name}.log"
    local quarantine_file="${QUARANTINE_DIR}/${batch_name}.txt"
    local max_attempts=3 attempt=1

    while [ "$attempt" -le "$max_attempts" ]; do
        # Build file list JSON
        local file_list_json=$(python3 -c "
import json
with open('$batch_file') as f:
    files = [line.strip() for line in f if line.strip()]
print(json.dumps(files))
")
        local file_count=$(echo "$file_list_json" | python3 -c "import sys,json; print(len(json.load(sys.stdin)))")
        [ "$file_count" -eq 0 ] && return 0

        # Write to a temp file (avoids ARG_MAX)
        local list_file="${STATE_DIR}/${batch_name}_files.json"
        echo "$file_list_json" > "$list_file"

        local t0=$(date +%s)
        # Fix: capture real exit code without || true swallowing it.
        local ec=0
        GRAMMARS_DIR="$GRAMMARS_DIR" \
        CODESCOPE_DB_PATH="$batch_db" \
        CODESCOPE_INDEX_MODE=fast \
        CODESCOPE_WORKERS=1 \
        timeout 600 "$CODESCOPE" worker \
            "$batch_db" \
            "$PROJECT_DIR" \
            "" \
            "batch-${batch_name}" \
            0 \
            --file-list "$list_file" \
            > "$batch_log" 2>&1 || ec=$?

        local dur=$(( $(date +%s) - t0 ))
        local nodes=$(sqlite3 "$batch_db" "SELECT COUNT(*) FROM graph_nodes;" 2>/dev/null || echo 0)
        local edges=$(sqlite3 "$batch_db" "SELECT COUNT(*) FROM graph_edges;" 2>/dev/null || echo 0)

        if [ "$ec" -eq 0 ] && [ "$nodes" -gt 0 ] 2>/dev/null; then
            echo "$batch_name:0:$nodes:$edges:$dur"
            log_metric "BATCH:${batch_name}" "exit=0 nodes=${nodes} edges=${edges} duration=${dur}s files=${file_count}"
            return 0
        fi

        echo "  [CRASH] $batch_name (attempt $attempt) — exit=$ec nodes=$nodes" >&2
        log_metric "CRASH:${batch_name}" "exit=${ec} nodes=${nodes} attempt=${attempt}"

        # Binary search for crashing file
        if [ "$file_count" -le 1 ]; then
            local crash_file=$(cat "$batch_file" | head -1)
            echo "$crash_file" >> "$quarantine_file"
            echo "  [QUARANTINE] $(basename $crash_file)" >&2
            log_metric "QUARANTINE:${batch_name}" "$(basename $crash_file)"
            return 1
        fi

        local left=0 right=$((file_count - 1)) crash_file=""
        local all_batch_files=$(mktemp)
        cat "$batch_file" > "$all_batch_files"

        while [ "$left" -le "$right" ]; do
            local mid=$(( (left + right) / 2 ))
            local test_list=$(mktemp)
            sed -n "$((left + 1)),$((mid + 1))p" "$all_batch_files" > "$test_list"
            local test_json=$(python3 -c "
import json
with open('$test_list') as f:
    files = [line.strip() for line in f if line.strip()]
print(json.dumps(files))
")
            local test_json_file="${STATE_DIR}/${batch_name}_test.json"
            echo "$test_json" > "$test_json_file"

            rm -f "${BASE}_test.db" 2>/dev/null
            # Fix: capture real exit code without || true swallowing it.
            local test_ec=0
            GRAMMARS_DIR="$GRAMMARS_DIR" \
            CODESCOPE_DB_PATH="${BASE}_test.db" \
            CODESCOPE_INDEX_MODE=fast \
            CODESCOPE_WORKERS=1 \
            timeout 120 "$CODESCOPE" worker \
                "${BASE}_test.db" \
                "$PROJECT_DIR" \
                "" \
                "test" \
                0 \
                --file-list "$test_json_file" \
                > /dev/null 2>&1 || test_ec=$?
            rm -f "${BASE}_test.db" 2>/dev/null "$test_list" "$test_json_file"

            if [ "$test_ec" -eq 0 ] || [ "$test_ec" -eq 124 ]; then
                left=$((mid + 1))
            else
                crash_file=$(sed -n "$((mid + 1))p" "$all_batch_files")
                right=$mid
            fi
        done
        rm -f "$all_batch_files"

        if [ -n "$crash_file" ] && [ -f "$crash_file" ]; then
            echo "  [QUARANTINE] $(basename $crash_file)" >&2
            echo "$crash_file" >> "$quarantine_file"
            log_metric "QUARANTINE:${batch_name}" "$(basename $crash_file)"
            grep -v -F "$crash_file" "$batch_file" > "${batch_file}.tmp" && mv "${batch_file}.tmp" "$batch_file"
        else
            echo "  [WARN] Could not find crashing file, skipping batch" >&2
            return 1
        fi
        attempt=$((attempt + 1))
    done
    return 1
}

# Run batches in parallel with a limit
ACTIVE=0
for batch_file in $BATCHES; do
    # Wait if we have too many active jobs
    while [ "$ACTIVE" -ge "$PARALLEL" ]; do
        for pid in $(jobs -p); do
            if ! kill -0 "$pid" 2>/dev/null; then
                wait "$pid" 2>/dev/null || true
                ACTIVE=$((ACTIVE - 1))
            fi
        done
        sleep 0.5
    done

    # Start batch in background
    (
        result=$(index_batch "$batch_file")
        echo "$result" >> "${BASE}_SUMMARY.txt"
        IFS=: read -r name ec nodes edges dur <<< "$result"
        if [ "$ec" = "0" ] && [ "$nodes" -gt 0 ] 2>/dev/null; then
            echo "  [DONE] $name → ${nodes} nodes ${edges} edges ${dur}s"
        fi
    ) &
    ACTIVE=$((ACTIVE + 1))
done

# Wait for all batches
for pid in $(jobs -p); do
    wait "$pid" 2>/dev/null || true
done

kill "$METRICS_PID" 2>/dev/null || true
echo ""

# ── Step 4: Report results ───────────────────────────────────────
echo "[4/4] Results"
echo ""

SUCCESS=0 FAIL=0 TOTAL_NODES=0 TOTAL_EDGES=0
echo "  Per-Batch Results:"
echo "  ─────────────────────────────────────"

if [ -f "${BASE}_SUMMARY.txt" ]; then
    while IFS=: read -r name ec nodes edges dur; do
        if [ "$ec" = "0" ] && [ "$nodes" -gt 0 ] 2>/dev/null; then
            SUCCESS=$((SUCCESS + 1))
            TOTAL_NODES=$((TOTAL_NODES + nodes))
            TOTAL_EDGES=$((TOTAL_EDGES + edges))
            printf "  ✅ %-20s %7d nodes  %7d edges  %4ds\n" "$name" "$nodes" "$edges" "$dur"
        else
            FAIL=$((FAIL + 1))
            printf "  ❌ %-20s crashed\n" "$name"
        fi
    done < "${BASE}_SUMMARY.txt"
fi

echo ""
echo "  Successful: $SUCCESS / $((SUCCESS + FAIL)) batches"
echo "  Total nodes: $TOTAL_NODES"
echo "  Total edges: $TOTAL_EDGES"
echo "  Quarantined: $(find "$QUARANTINE_DIR" -name "*.txt" -exec wc -l {} + 2>/dev/null | tail -1 | awk '{print $1}') files"
echo ""

log_metric "FINAL" "success=${SUCCESS} fail=${FAIL} total_nodes=${TOTAL_NODES} total_edges=${TOTAL_EDGES}"
log_system

echo "  Metrics: $METRICS_FILE"
echo "=========================================="