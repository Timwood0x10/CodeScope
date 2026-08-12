#!/bin/bash
# hotspots.sh — query project hotspot functions
# Usage: ./skills/hotspots.sh [top_n]
# NOTE: the MCP tool `get_hotspots` was removed from TOOL_HANDLERS. Use
# `get_knowledge_graph` (knowledge layer) or `find_callers` for density.
set -e
TOP_N=${1:-10}
echo "=== Hotspot Top $TOP_N (via get_knowledge_graph) ==="
codescope cli get_knowledge_graph "{\"limit\":$TOP_N}"
