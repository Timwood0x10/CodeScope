#!/bin/bash
# hotspots.sh — query project hotspot functions
# Usage: ./skills/hotspots.sh [top_n]
set -e
TOP_N=${1:-10}
echo "=== Hotspot Top $TOP_N ==="
codescope cli get_hotspots "{\"top_n\":$TOP_N}"
