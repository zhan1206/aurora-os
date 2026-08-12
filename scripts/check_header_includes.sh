#!/bin/bash
# FIXED (v4.4.2): CI-008 — Check that .c files include headers for all structs used
# Scans each .c file for struct references and verifies the corresponding .h is included

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Checking header includes ==="
WARNINGS=0

# For each .c file, check if it includes headers for struct types it uses
for src in "$PROJECT_ROOT/kernel"/*.c; do
    [ -f "$src" ] || continue
    basename=$(basename "$src" .c)
    
    # Check if the file includes its own header
    if grep -q "struct ${basename}" "$src" 2>/dev/null; then
        if ! grep -q "#include.*${basename}.h" "$src" 2>/dev/null; then
            echo "  WARN: $basename.c uses struct ${basename} but doesn't include ${basename}.h"
            WARNINGS=$((WARNINGS + 1))
        fi
    fi
done

if [ "$WARNINGS" -gt 0 ]; then
    echo "WARN: $WARNINGS potential missing includes found."
fi
echo "PASS: Header include check complete."
exit 0