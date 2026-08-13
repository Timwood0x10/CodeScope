#!/bin/bash
# stats.sh — query project statistics
# Usage: ./skills/stats.sh
echo "=== Graph stats ==="
codescope cli get_graph_stats '{}'
echo ""
echo "=== Project overview ==="
codescope cli project_overview '{}'
echo ""
echo "=== Entry points ==="
codescope cli get_entry_points '{}'
