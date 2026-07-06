#!/bin/bash
# trace.sh — 追踪两点之间的调用路径
# Usage: ./skills/trace.sh <from_function> <to_function>
set -e
FROM=$1
TO=$2
if [ -z "$FROM" ] || [ -z "$TO" ]; then
    echo "Usage: $0 <from_function> <to_function>"
    exit 1
fi
echo "=== 调用路径: $FROM → $TO ==="
codescope cli codescope_trace "{\"from\":\"$FROM\",\"to\":\"$TO\"}"
