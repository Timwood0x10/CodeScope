#!/bin/bash
# hotspots.sh — 查询项目热点函数
# Usage: ./skills/hotspots.sh [top_n]
set -e
TOP_N=${1:-10}
echo "=== 热点函数 Top $TOP_N ==="
codescope cli get_hotspots "{\"top_n\":$TOP_N}"
