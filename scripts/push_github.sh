#!/bin/bash
# GitHub push script for AuroraOS
# Usage: bash push_github.sh <token>

set -euo pipefail

TOKEN="$1"
PROJECT_DIR="/mnt/d/自制操作系统"
USER="zhan1206"
REPO="aurora-os"
GIT_USER_NAME="zhan1206"
GIT_USER_EMAIL="zhan1206@users.noreply.github.com"

echo "============================================"
echo " AuroraOS GitHub Push Script"
echo "============================================"

# Step 1: Verify token
echo ""
echo "[1/5] Verifying token..."
RESPONSE=$(curl -s -H "Authorization: token $TOKEN" https://api.github.com/user)
LOGIN=$(echo "$RESPONSE" | grep -o '"login": *"[^"]*"' | cut -d'"' -f4)
if [ -n "$LOGIN" ]; then
    echo "  Token: VALID (user: $LOGIN)"
else
    echo "  Token: INVALID"
    echo "  Response: $RESPONSE"
    exit 1
fi

# Check if repo exists
echo "  Checking repository..."
REPO_CHECK=$(curl -s -o /dev/null -w "%{http_code}" -H "Authorization: token $TOKEN" "https://api.github.com/repos/$USER/$REPO")
if [ "$REPO_CHECK" = "200" ]; then
    echo "  Repository: $USER/$REPO EXISTS"
elif [ "$REPO_CHECK" = "404" ]; then
    echo "  Repository: $USER/$REPO NOT FOUND"
    echo "  Creating repository..."
    CREATE_RESP=$(curl -s -X POST -H "Authorization: token $TOKEN" \
        -H "Content-Type: application/json" \
        -d "{\"name\":\"$REPO\",\"private\":false,\"description\":\"AuroraOS — A minimal x86_64 operating system kernel built from scratch\",\"has_issues\":true,\"has_projects\":false,\"has_wiki\":false}" \
        "https://api.github.com/user/repos")
    if echo "$CREATE_RESP" | grep -q '"full_name"'; then
        echo "  Repository created: $USER/$REPO"
    else
        echo "  Failed to create repository:"
        echo "$CREATE_RESP"
        exit 1
    fi
else
    echo "  Unexpected response: HTTP $REPO_CHECK"
    exit 1
fi

# Step 2: Initialize git
echo ""
echo "[2/5] Initializing git repository..."
cd "$PROJECT_DIR"
if ! git rev-parse --git-dir >/dev/null 2>&1; then
    git init
    git checkout -b main
    echo "  Repository initialized (branch: main)"
else
    echo "  Repository already exists"
fi

# Configure git user
git config user.name "$GIT_USER_NAME"
git config user.email "$GIT_USER_EMAIL"
echo "  Git user: $GIT_USER_NAME <$GIT_USER_EMAIL>"

# Step 3: Configure remote
echo ""
echo "[3/5] Configuring remote..."
git remote remove origin 2>/dev/null || true
git remote add origin "https://$USER:$TOKEN@github.com/$USER/$REPO.git"
echo "  Remote: $USER/$REPO.git"

# Step 4: Stage and commit
echo ""
echo "[4/5] Staging and committing..."
git add -A
echo "  Staged files:"
git status --short | head -40
echo ""
git commit -m "$(cat <<'EOF'
v2.5.0: Comprehensive CI/CD pipeline, build cleanup, documentation polish

- Add GitHub Actions CI/CD pipeline with 4 jobs (lint, build-linux, build-macos, compliance)
- Create reusable composite action for QEMU boot testing
- Add JUnit XML test report generation script
- Expand shell command reference (21 commands) in README and user manual
- Fix user manual outdated FAQ entries
- Update architecture document version to v2.5.0
- Add userspace binaries to .gitignore
- Fix test count consistency (15→16) across README and CI
- Remove resolved strict-aliasing FAQ entry
- Add workflow verification script
EOF
)" || {
    echo "  Nothing to commit (already up to date)"
}

# Step 5: Push
echo ""
echo "[5/5] Pushing to GitHub..."
PUSH_OUTPUT=$(git push -u origin main 2>&1)
PUSH_CODE=$?
echo "$PUSH_OUTPUT"

if [ $PUSH_CODE -eq 0 ]; then
    echo ""
    echo "============================================"
    echo " SUCCESS"
    echo "============================================"
    echo " Repository: https://github.com/$USER/$REPO"
    echo ""
else
    echo ""
    echo "============================================"
    echo " PUSH FAILED"
    echo "============================================"
    echo " Error output above."
    echo ""
    echo " Troubleshooting:"
    echo "  - Check if the repository exists: https://github.com/$USER/$REPO"
    echo "  - Verify token has 'repo' scope"
    echo "  - Ensure you have push access to the repository"
    exit 1
fi