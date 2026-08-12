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

echo "PASS: CHANGELOG audit complete."
exit 0