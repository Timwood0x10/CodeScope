#!/bin/bash
# trace.sh — trace call path between two points
# Usage: ./skills/trace.sh <from_function> <to_function>
set -e
FROM=$1
TO=$2
if [ -z "$FROM" ] || [ -z "$TO" ]; then
    echo "Usage: $0 <from_function> <to_function>"
    exit 1
fi
echo "=== Call path: $FROM → $TO ==="
codescope cli codescope_trace "{\"from\":\"$FROM\",\"to\":\"$TO\"}"
