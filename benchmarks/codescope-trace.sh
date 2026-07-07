#!/usr/bin/env bash
# CodeScope 交互式调用链追踪工具
# 用法: ./codescope-trace.sh <project_dir> <function_name>
# 交互展开/折叠调用者与被调用者
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CODESCOPE="${PROJECT_ROOT}/target/debug/codescope"
DB_PATH="/tmp/codescope_trace.db"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

usage() {
    echo "Usage: $0 <project_dir> <function_name>"
    echo ""
    echo "Interactive keys:"
    echo "  ↑/↓         Navigate"
    echo "  →           Expand callers"
    echo "  ←           Expand callees"
    echo "  q/ESC       Quit"
    echo "  /           Search"
    exit 1
}

[ $# -ge 2 ] || usage
PROJECT_DIR="$1"
FUNCTION="$2"

# Ensure DB exists — index the project if needed
if [ ! -f "$DB_PATH" ]; then
    echo -e "${YELLOW}Indexing project...${NC}"
    $CODESCOPE worker "$DB_PATH" "$PROJECT_DIR" "" "trace-project" "1" >/dev/null 2>&1
    echo -e "${GREEN}Indexed.${NC}"
fi

# Save project_id for querying
PID=1

# Build an ASCII tree
render_tree() {
    local func="$1"
    local depth="${2:-1}"
    local prefix="$3"
    local dir="${4:-both}"  # callers, callees, both

    [ "$depth" -gt 3 ] && return  # limit depth

    # Get callers
    if [ "$dir" = "callers" ] || [ "$dir" = "both" ]; then
        local callers
        callers=$($CODESCOPE cli find_callers "{\"function_name\":\"$func\"}" 2>/dev/null)
        echo "$callers" | python3 -c "
import json,sys
try:
    data = json.load(sys.stdin)
    items = data.get('callers', data if isinstance(data, list) else [])
    for i, item in enumerate(items[:8]):
        name = item.get('name', item.get('function_name','?'))
        file = item.get('file', item.get('file_path',''))
        line = item.get('line', item.get('start_row',0))
        print(f'{prefix}├─ [{name}] ({file}:{line})')
except:
    pass
" 2>/dev/null
    fi

    # Get callees
    if [ "$dir" = "callees" ] || [ "$dir" = "both" ]; then
        local callees
        callees=$($CODESCOPE cli find_callees "{\"function_name\":\"$func\"}" 2>/dev/null)
        echo "$callees" | python3 -c "
import json,sys
try:
    data = json.load(sys.stdin)
    items = data.get('callees', data if isinstance(data, list) else [])
    for i, item in enumerate(items[:8]):
        name = item.get('name', item.get('function_name','?'))
        file = item.get('file', item.get('file_path',''))
        line = item.get('line', item.get('start_row',0))
        print(f'{prefix}└─ [{name}] ({file}:{line})')
except:
    pass
" 2>/dev/null
    fi
}

echo ""
echo -e "${BOLD}CodeScope Trace: ${CYAN}${FUNCTION}${NC}"
echo "──────────────────────────────────────────"
echo -e "Callers (←)        Callees (→)"
echo "──────────────────────────────────────────"
echo ""

# Render initial tree
render_tree "$FUNCTION" 1 "" "both"

echo ""
echo -e "${YELLOW}Enter function name to expand (empty to quit):${NC}"
while true; do
    read -r -p "→ " input
    [ -z "$input" ] && break
    echo ""
    echo -e "${BOLD}${CYAN}${input}${NC}"
    echo "──────────────────────────────────────"
    render_tree "$input" 1 "" "both"
    echo ""
done

echo -e "${GREEN}Done.${NC}"
