#!/bin/bash
# FIXED (v4.4.2): CI-009 — Check static/header consistency
# Scans .c files for static functions that are declared extern in .h files

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Checking static vs header declarations ==="
CONFLICTS=0

# Find all static functions in .c files
for src in "$PROJECT_ROOT/kernel"/*.c; do
    [ -f "$src" ] || continue
    
    # Get static function names
    static_funcs=$(grep -Po 'static\s+\w+\s+\K\w+(?=\s*\()' "$src" 2>/dev/null || true)
    
    for func in $static_funcs; do
        # Check if any header has this as extern
        if grep -rn "extern.*${func}" "$PROJECT_ROOT/kernel/" --include='*.h' 2>/dev/null | grep -q .; then
            echo "  CONFLICT: $func is static in $(basename $src) but declared extern in header"
            CONFLICTS=$((CONFLICTS + 1))
        fi
    done
done

if [ "$CONFLICTS" -gt 0 ]; then
    echo "WARN: $CONFLICTS static/header conflicts found."
    exit 1
fi
echo "PASS: No static/header conflicts."
exit 0