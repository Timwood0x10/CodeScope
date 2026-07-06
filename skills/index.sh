#!/bin/bash
# index.sh — 索引一个项目
# Usage: ./skills/index.sh <project_path> [language_filter]
set -e
PROJECT=$1
LANG=${2:-""}
if [ -z "$PROJECT" ]; then
    echo "Usage: $0 <project_path> [language_filter]"
    exit 1
fi
echo "=== 索引 $PROJECT ==="
codescope cli index_project "{\"project_path\":\"$PROJECT\",\"language_filter\":\"$LANG\"}"
echo "=== 完成 ==="
codescope cli get_graph_stats '{}'
