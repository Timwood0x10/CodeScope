#!/bin/bash
# modules.sh — view project module tree
# Usage: ./skills/modules.sh
echo "=== Module tree ==="
codescope cli get_module_tree '{}'
