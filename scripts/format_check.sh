#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
#  AuroraOS CI Code Style Check (clang-format)
#  ─────────────────────────────────────────────────────────────────────
#  Checks if all source files comply with the project's .clang-format
#  configuration. Outputs GitHub Actions annotations for non-compliant
#  files.
#
#  Exit 0 if all files comply, 1 otherwise.
# ─────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────
SRC_DIRS=("kernel" "arch" "boot" "userspace")
IS_GITHUB_CI="${GITHUB_ACTIONS:-false}"
ISSUES=0

# ── Helpers ──────────────────────────────────────────────────────────
github_warning() {
    local file="$1" line="$2" msg="$3"
    if [ "$IS_GITHUB_CI" = "true" ]; then
        echo "::warning file=${file},line=${line}::${msg}"
    else
        echo "[WARN] ${file}:${line} — ${msg}"
    fi
}

github_error() {
    local file="$1" line="$2" msg="$3"
    if [ "$IS_GITHUB_CI" = "true" ]; then
        echo "::error file=${file},line=${line}::${msg}"
    else
        echo "[ERROR] ${file}:${line} — ${msg}"
    fi
}

# ── Check clang-format availability ──────────────────────────────────
if ! command -v clang-format &>/dev/null; then
    echo "ERROR: clang-format not found in PATH"
    echo "Install it with: sudo apt install clang-format"
    exit 1
fi

echo "=============================================="
echo " AuroraOS CI Format Check"
echo " clang-format $(clang-format --version | head -1)"
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

echo "Checking ${#FILES[@]} source file(s)..."
echo ""

# ── Check each file with clang-format --dry-run ──────────────────────
for f in "${FILES[@]}"; do
    # Run clang-format in dry-run mode; capture the diff output
    DIFF_OUTPUT=$(clang-format --dry-run --Werror "$f" 2>&1) || true

    if [ -n "$DIFF_OUTPUT" ]; then
        # Extract line numbers from diff output
        while IFS= read -r diff_line; do
            # Parse unified diff to find changed line numbers
            if [[ "$diff_line" =~ ^@@\ -([0-9]+), ]]; then
                local_line="${BASH_REMATCH[1]}"
                github_error "$f" "$local_line" "File does not comply with clang-format. Run './format.sh' to fix."
            fi
        done <<< "$DIFF_OUTPUT"

        # If no line numbers parsed, emit a general annotation
        if ! echo "$DIFF_OUTPUT" | grep -q "^@@"; then
            if [ -n "$DIFF_OUTPUT" ]; then
                github_error "$f" "1" "File does not comply with clang-format. Run './format.sh' to fix."
            fi
        fi

        ISSUES=$((ISSUES + 1))
        echo "  [FAIL] $f"
    else
        echo "  [PASS] $f"
    fi
done

# ── Summary ──────────────────────────────────────────────────────────
echo ""
echo "=============================================="
if [ "$ISSUES" -gt 0 ]; then
    echo " RESULT: $ISSUES file(s) need formatting"
    echo "=============================================="
    echo ""
    echo "::warning::$ISSUES file(s) do not comply with clang-format"

    if [ "$IS_GITHUB_CI" = "true" ]; then
        {
            echo "## Format Check"
            echo ""
            echo "| Check | Result |"
            echo "|-------|--------|"
            echo "| clang-format | :warning: $ISSUES file(s) non-compliant |"
        } >> "$GITHUB_STEP_SUMMARY"
    fi
    exit 1
else
    echo " RESULT: All files comply with clang-format"
    echo "=============================================="

    if [ "$IS_GITHUB_CI" = "true" ]; then
        {
            echo "## Format Check"
            echo ""
            echo "| Check | Result |"
            echo "|-------|--------|"
            echo "| clang-format | :white_check_mark: PASS |"
        } >> "$GITHUB_STEP_SUMMARY"
    fi
    exit 0
fi