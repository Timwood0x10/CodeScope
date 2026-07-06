#!/bin/bash
# modules.sh — 查看项目模块树
# Usage: ./skills/modules.sh
echo "=== 模块树 ==="
codescope cli get_module_tree '{}'
