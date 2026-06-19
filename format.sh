#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
#  AuroraOS Code Formatting Script
#  ─────────────────────────────────────────────────────────────────────
#  Formats all .c and .h files in kernel/, arch/, boot/, userspace/
#  using clang-format with the project's .clang-format configuration.
#
#  Usage:
#    ./format.sh          - format all files in-place
#    ./format.sh --check  - dry-run, show which files would change
# ─────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────
SRC_DIRS=("kernel" "arch" "boot" "userspace")
DRY_RUN=false

if [[ "${1:-}" == "--check" ]]; then
    DRY_RUN=true
fi

# ── Check clang-format availability ──────────────────────────────────
if ! command -v clang-format &>/dev/null; then
    echo "ERROR: clang-format not found in PATH"
    echo "Install it with: sudo apt install clang-format"
    exit 1
fi

echo "=============================================="
echo " AuroraOS Code Formatter"
echo " clang-format $(clang-format --version | head -1)"
if [ "$DRY_RUN" = true ]; then
    echo " Mode: --dry-run (no files will be modified)"
else
    echo " Mode: in-place formatting"
fi
echo "=============================================="
echo ""

# ── Collect all source files ─────────────────────────────────────────
FILES=()
for dir in "${SRC_DIRS[@]}"; do
    if [ -d "$dir" ]; then
        while IFS= read -r -d '' f; do
            FILES+=("$f")
        done < <(find "$dir" -type f \( -name '*.c' -o -name '*.h' \) -print0 2>/dev/null || true)
    fi
done

if [ ${#FILES[@]} -eq 0 ]; then
    echo "No source files found in ${SRC_DIRS[*]}"
    exit 0
fi

echo "Found ${#FILES[@]} source file(s) to process"
echo ""

# ── Format files ─────────────────────────────────────────────────────
MODIFIED=0
TOTAL=${#FILES[@]}

for f in "${FILES[@]}"; do
    if [ "$DRY_RUN" = true ]; then
        if ! clang-format --dry-run --Werror "$f" &>/dev/null; then
            echo "  [WOULD MODIFY] $f"
            MODIFIED=$((MODIFIED + 1))
        fi
    else
        BEFORE=$(md5sum "$f" 2>/dev/null | cut -d' ' -f1 || true)
        clang-format -i "$f"
        AFTER=$(md5sum "$f" 2>/dev/null | cut -d' ' -f1 || true)
        if [ "$BEFORE" != "$AFTER" ]; then
            echo "  [MODIFIED] $f"
            MODIFIED=$((MODIFIED + 1))
        fi
    fi
done

# ── Summary ──────────────────────────────────────────────────────────
echo ""
echo "=============================================="
if [ "$DRY_RUN" = true ]; then
    if [ "$MODIFIED" -gt 0 ]; then
        echo " RESULT: $MODIFIED file(s) would be modified"
        exit 1
    else
        echo " RESULT: All $TOTAL file(s) comply with clang-format"
        exit 0
    fi
else
    if [ "$MODIFIED" -gt 0 ]; then
        echo " RESULT: $MODIFIED file(s) reformatted"
    else
        echo " RESULT: All $TOTAL file(s) already formatted"
    fi
    exit 0
fi