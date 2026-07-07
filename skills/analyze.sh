#!/bin/bash
# analyze.sh — full analysis pipeline
# Usage: ./skills/analyze.sh <project_path> [language_filter]
set -e
PROJECT=$1
LANG=${2:-""}
if [ -z "$PROJECT" ]; then
    echo "Usage: $0 <project_path> [language_filter]"
    exit 1
fi
echo "╔═══════════════════════════════════════════╗"
echo "║      CodeScope Full Analysis Pipeline      ║"
echo "╚═══════════════════════════════════════════╝"
echo ""
echo "=== [1/5] Indexing project ==="
codescope cli index_project "{\"project_path\":\"$PROJECT\",\"language_filter\":\"$LANG\"}"
echo ""
echo "=== [2/5] Project overview ==="
codescope cli project_overview '{}'
echo ""
echo "=== [3/5] Entry points & module tree ==="
codescope cli get_entry_points '{}'
codescope cli get_module_tree '{}'
echo ""
echo "=== [4/5] Hotspot functions ==="
codescope cli get_hotspots '{"top_n":10}'
echo ""
echo "=== [5/5] Graph stats ==="
codescope cli get_graph_stats '{}'
echo ""
echo "=== Done ==="
