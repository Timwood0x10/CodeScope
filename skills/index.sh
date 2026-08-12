#!/bin/bash
# index.sh — index a project
# Usage: ./skills/index.sh <project_path> [language_filter]
# NOTE: the MCP tool `index_project` was removed from TOOL_HANDLERS; the CLI
# entry point is now `codescope worker <db> <dir> <lang> <name> <pid>` (serial)
# or `codescope index-parallel <dir>` (parallel). This script uses worker.
set -e
PROJECT=$1
LANG=${2:-""}
if [ -z "$PROJECT" ]; then
    echo "Usage: $0 <project_path> [language_filter]"
    exit 1
fi
DB="${CODESCOPE_DB_PATH:-/tmp/codescope_index.db}"
echo "=== Indexing $PROJECT ==="
codescope worker "$DB" "$PROJECT" "$LANG" "index-sh" 0
echo "=== Done ==="
codescope cli get_graph_stats '{}'
