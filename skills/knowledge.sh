#!/bin/bash
# knowledge.sh — direct-query a knowledge-layer table (v0.2.1)
# Usage:
#   ./skills/knowledge.sh <table> [limit]
# Examples:
#   ./skills/knowledge.sh architecture_edge 20
#   ./skills/knowledge.sh capability 10
#   ./skills/knowledge.sh module_summary
# Supported tables: entity, relation, architecture_edge, module_edge,
#                  capability, document, module_summary
TABLE="${1:-architecture_edge}"
LIMIT="${2:-100}"
echo "=== Knowledge graph: ${TABLE} (limit ${LIMIT}) ==="
codescope cli get_knowledge_graph "{\"table\":\"${TABLE}\",\"limit\":${LIMIT}}"
