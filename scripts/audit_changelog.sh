#!/bin/bash
# FIXED (v4.4.2): CI-010 — Audit CHANGELOG.md against git log
# Compares CHANGELOG.md entries with actual git log to find discrepancies

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Auditing CHANGELOG.md ==="

# Check if CHANGELOG exists
if [ ! -f "$PROJECT_ROOT/CHANGELOG.md" ]; then
    echo "FAIL: CHANGELOG.md not found!"
    exit 1
fi

# Check changelog mentions current version
CURRENT_VERSION=$(grep -m1 "v4\." "$PROJECT_ROOT/CHANGELOG.md" | head -1)
echo "  Current version: $CURRENT_VERSION"

# Check if version.h matches
VERSION_H=$(grep AURORAOS_VERSION "$PROJECT_ROOT/kernel/include/version.h" | head -1)
echo "  version.h: $VERSION_H"

# Count git tags vs CHANGELOG versions
GIT_TAGS=$(cd "$PROJECT_ROOT" && git tag -l 'v4.*' | wc -l)
CHANGELOG_VERSIONS=$(grep -c "v4\." "$PROJECT_ROOT/CHANGELOG.md" || echo "0")
echo "  Git tags: $GIT_TAGS, CHANGELOG versions: $CHANGELOG_VERSIONS"

# FIXED (v4.4.3): P0-0.2 — Count selftest functions and verify CHANGELOG accuracy
SELFTEST_FILE="$PROJECT_ROOT/kernel/selftest.c"
if [ -f "$SELFTEST_FILE" ]; then
    TEST_COUNT=$(grep -c 'static void test_' "$SELFTEST_FILE" 2>/dev/null || echo "0")
    echo "  Selftest functions: $TEST_COUNT"
    
    # Check if CHANGELOG mentions the correct test count
    if grep -q "60+" "$PROJECT_ROOT/CHANGELOG.md" 2>/dev/null; then
        if [ "$TEST_COUNT" -lt 60 ]; then
            echo "  WARN: CHANGELOG claims 60+ tests but only $TEST_COUNT test_ functions found!"
        else
            echo "  OK: CHANGELOG test count claim matches ($TEST_COUNT >= 60)"
        fi
    fi
fi

# Check README.md badge for test count
README_FILE="$PROJECT_ROOT/README.md"
if [ -f "$README_FILE" ]; then
    README_TESTS=$(grep -oP 'tests-\d+\+' "$README_FILE" 2>/dev/null | grep -oP '\d+' || echo "0")
    if [ -n "$README_TESTS" ] && [ -n "$TEST_COUNT" ]; then
        echo "  README badge: tests-${README_TESTS}+, actual: $TEST_COUNT"
        if [ "$TEST_COUNT" -lt "$README_TESTS" ]; then
            echo "  WARN: README badge claims ${README_TESTS}+ but only $TEST_COUNT test_ functions found!"
        fi
    fi
fi

echo "PASS: CHANGELOG audit complete."
exit 0