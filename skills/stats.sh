#!/bin/bash
# stats.sh — 查询项目统计
# Usage: ./skills/stats.sh
echo "=== 图统计 ==="
codescope cli get_graph_stats '{}'
echo ""
echo "=== 项目信息 ==="
codescope cli get_project_info '{}'
echo ""
echo "=== 入口点 ==="
codescope cli get_entry_points '{}'
