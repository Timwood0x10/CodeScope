#!/bin/bash
# index.sh — index a project
# Usage: ./skills/index.sh <project_path> [language_filter]
set -e
PROJECT=$1
LANG=${2:-""}
if [ -z "$PROJECT" ]; then
    echo "Usage: $0 <project_path> [language_filter]"
    exit 1
fi
echo "=== Indexing $PROJECT ==="
codescope cli index_project "{\"project_path\":\"$PROJECT\",\"language_filter\":\"$LANG\"}"
echo "=== Done ==="
codescope cli get_graph_stats '{}'
