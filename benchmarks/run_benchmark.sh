#!/usr/bin/env bash
# CodeScope benchmark runner
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"
BASELINES_DIR="${SCRIPT_DIR}/baselines"
CODESCOPE="${PROJECT_ROOT}/target/debug/codescope"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

timestamp() { date +%Y%m%d_%H%M%S; }

bench_index() {
    local project_dir="$1"
    local project_name="$2"
    local outfile="${RESULTS_DIR}/index_${project_name}_$(timestamp).json"

    echo -e "${CYAN}[bench]${NC} Indexing ${project_name}..."
    local start=$(date +%s)

    # Run worker subprocess
    OUTPUT=$($CODESCOPE worker ".codescope/codescope.db" "$project_dir" "" "$project_name" 2>/dev/null)
    local end=$(date +%s)
    local elapsed=$((end - start))

    # Count source files
    local file_count=$(find "$project_dir" \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \
        -o -name '*.rs' -o -name '*.java' -o -name '*.py' -o -name '*.ts' -o -name '*.js' \
        -o -name '*.go' -o -name '*.zig' -o -name '*.swift' \) 2>/dev/null | wc -l | tr -d ' ')
    local speed=$(echo "scale=1; $file_count / $elapsed" | bc 2>/dev/null || echo "0")

    echo -e "${GREEN}[bench]${NC} ${project_name}: ${file_count} files, ${elapsed}s, ${speed} files/s"

    # Extract node count from JSON output
    local nodes=$(echo "$OUTPUT" | grep -o '"total_nodes":[0-9]*' | head -1 | grep -o '[0-9]*' || echo "0")

    # Write result
    cat > "$outfile" <<EOF
{
  "project": "$project_name",
  "files": $file_count,
  "index_time_sec": $elapsed,
  "index_speed_files_per_sec": $speed,
  "nodes": $nodes,
  "date": "$(date +%Y-%m-%d)",
  "machine": "Apple M3 Max, 64GB"
}
EOF
    echo -e "${GREEN}[bench]${NC} Result saved to $outfile"
    echo "$elapsed"
}

bench_queries() {
    local db_path="${PROJECT_ROOT}/.codescope/codescope.db"
    local outfile="${RESULTS_DIR}/queries_$(timestamp).json"

    echo -e "${CYAN}[bench]${NC} Running query benchmarks..."
    echo "{" > "$outfile"
    local first=true

    for tool in get_graph_stats get_hotspots get_module_tree get_entry_points; do
        local args="{}"
        [ "$tool" = "get_hotspots" ] && args='{"top_n":10}'
        [ "$tool" = "find_callers" ] && args='{"function_name":"main"}'
        [ "$tool" = "find_callees" ] && args='{"function_name":"main"}'

        local total_ms=0
        local n=5
        for i in $(seq 1 $n); do
            local start=$(date +%s%N)
            $CODESCOPE cli "$tool" "$args" >/dev/null 2>&1
            local end=$(date +%s%N)
            local ms=$(( (end - start) / 1000000 ))
            total_ms=$((total_ms + ms))
        done
        local avg=$((total_ms / n))

        if [ "$first" = true ]; then first=false; else echo "," >> "$outfile"; fi
        echo "  \"$tool\": $avg" >> "$outfile"
    done

    echo "" >> "$outfile"
    echo "}" >> "$outfile"
    echo -e "${GREEN}[bench]${NC} Query results saved to $outfile"
}

compare_results() {
    local current="$1"
    local baseline="$2"

    [ -f "$current" ] || { echo "Not found: $current"; return 1; }
    [ -f "$baseline" ] || { echo "Not found: $baseline"; return 1; }

    python3 -c "
import json, sys
with open('$baseline') as f: b = json.load(f)
with open('$current') as f: c = json.load(f)

rows = []
# Index timing
if 'index_time_sec' in b and 'index_time_sec' in c:
    bt = b['index_time_sec']; ct = c['index_time_sec']
    delta = (ct - bt) / bt * 100
    flag = '🔴' if delta > 20 else ('🟡' if delta > 10 else '🟢')
    rows.append(('Index Time (s)', f'{bt}', f'{ct}', f'{flag} {delta:+.1f}%'))

# Files
if 'files' in b and 'files' in c:
    rows.append(('Files', f\"{b['files']:,}\", f\"{c['files']:,}\", ''))

# Speed
if 'index_speed_files_per_sec' in b and 'index_speed_files_per_sec' in c:
    bs = b['index_speed_files_per_sec']; cs = c['index_speed_files_per_sec']
    if bs > 0:
        delta = (cs - bs) / bs * 100
        rows.append(('Speed (files/s)', f'{bs}', f'{cs}', f'{delta:+.1f}%'))

print()
print('┌──────────────────────┬────────────┬────────────┬───────────┐')
print('│ Metric               │ Baseline   │ Current    │ Δ         │')
print('├──────────────────────┼────────────┼────────────┼───────────┤')
for name, bv, cv, delta in rows:
    print(f'│ {name:<20} │ {bv:>10} │ {cv:>10} │ {delta:>9} │')
print('└──────────────────────┴────────────┴────────────┴───────────┘')
    "
}

main() {
    mkdir -p "${RESULTS_DIR}"

    case "${1:-}" in
        --index)
            local project_dir="${2:-}"
            local project_name="${3:-$(basename "$project_dir")}"
            [ -d "$project_dir" ] || { echo "Usage: $0 --index <project_dir> [name]"; exit 1; }
            bench_index "$project_dir" "$project_name"
            ;;
        --query)
            bench_queries
            ;;
        --all)
            echo -e "${CYAN}=== CodeScope Benchmark Suite ===${NC}"
            # Index self first
            bench_index "$PROJECT_ROOT" "CodeScope"
            echo ""
            bench_queries
            ;;
        --compare)
            local current="${2:-$(ls -t ${RESULTS_DIR}/*.json 2>/dev/null | head -1)}"
            local baseline="${3:-${BASELINES_DIR}/rustc.json}"
            compare_results "$current" "$baseline"
            ;;
        *)
            echo "Usage:"
            echo "  $0 --index <dir> [name]    Run index benchmark"
            echo "  $0 --query                 Run query benchmarks"
            echo "  $0 --all                   Index + queries"
            echo "  $0 --compare [cur] [base]  Compare"
            ;;
    esac
}

main "$@"
