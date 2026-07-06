#!/bin/bash
# analyze.sh — 完整分析流水线
# Usage: ./skills/analyze.sh <project_path> [language_filter]
set -e
PROJECT=$1
LANG=${2:-""}
if [ -z "$PROJECT" ]; then
    echo "Usage: $0 <project_path> [language_filter]"
    exit 1
fi
echo "╔═══════════════════════════════════════════╗"
echo "║      CodeScope 完整分析流水线              ║"
echo "╚═══════════════════════════════════════════╝"
echo ""
echo "=== [1/5] 索引项目 ==="
codescope cli index_project "{\"project_path\":\"$PROJECT\",\"language_filter\":\"$LANG\"}"
echo ""
echo "=== [2/5] 项目概览 ==="
codescope cli project_overview '{}'
echo ""
echo "=== [3/5] 入口点 & 模块树 ==="
codescope cli get_entry_points '{}'
codescope cli get_module_tree '{}'
echo ""
echo "=== [4/5] 热点函数 ==="
codescope cli get_hotspots '{"top_n":10}'
echo ""
echo "=== [5/5] 图统计 ==="
codescope cli get_graph_stats '{}'
echo ""
echo "=== 完成 ==="
