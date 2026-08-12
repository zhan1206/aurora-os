#!/bin/bash
# FIXED (v4.4.2): CI-007 — Prevent nested comment regression
# Scans kernel/ for nested /* inside /* */ comments
# Exit 0 = clean, Exit 1 = nested comments found

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Checking for nested comments ==="

FOUND=0
while IFS= read -r line; do
    file=$(echo "$line" | cut -d: -f1)
    lineno=$(echo "$line" | cut -d: -f2)
    echo "  NESTED: $file:$lineno"
    FOUND=1
done < <(grep -rnE '^\s*\*.*/\*.*\*/' "$PROJECT_ROOT/kernel/" --include='*.c' --include='*.h' 2>/dev/null || true)

if [ "$FOUND" -eq 1 ]; then
    echo "FAIL: Nested comments found in kernel source!"
    exit 1
fi
echo "PASS: No nested comments found."
exit 0