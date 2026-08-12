#!/bin/bash
# analyze.sh — full analysis pipeline
# Usage: ./skills/analyze.sh <project_path> [language_filter]
# NOTE: MCP tool `index_project` was removed from TOOL_HANDLERS; indexing is
# now done via `codescope worker` (serial) or `codescope index-parallel`
# (parallel). `get_hotspots` has no MCP tool either, so the hotspot step is
# skipped (get_knowledge_graph is the knowledge-layer alternative).
set -e
PROJECT=$1
LANG=${2:-""}
if [ -z "$PROJECT" ]; then
    echo "Usage: $0 <project_path> [language_filter]"
    exit 1
fi
DB="${CODESCOPE_DB_PATH:-/tmp/codescope_index.db}"
echo "╔═══════════════════════════════════════════╗"
echo "║      CodeScope Full Analysis Pipeline      ║"
echo "╚═══════════════════════════════════════════╝"
echo ""
echo "=== [1/5] Indexing project ==="
codescope worker "$DB" "$PROJECT" "$LANG" "analyze-sh" 0
echo ""
echo "=== [2/5] Project overview ==="
codescope cli project_overview '{}'
echo ""
echo "=== [3/5] Entry points & module tree ==="
codescope cli get_entry_points '{}'
codescope cli get_module_tree '{}'
echo ""
echo "=== [4/5] Knowledge graph (replaces removed get_hotspots) ==="
codescope cli get_knowledge_graph '{"limit":10}'
echo ""
echo "=== [5/5] Graph stats ==="
codescope cli get_graph_stats '{}'
echo ""
echo "=== Done ==="
