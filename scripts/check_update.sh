#!/bin/bash
# check_update.sh - Check GitHub for newer AuroraOS releases
#
# Compares the local version (from version.h) with the latest GitHub release.
# Exits with 0 if up-to-date, 1 if update available, 2 on error.
#
# Usage:
#   ./scripts/check_update.sh              # check and print status
#   ./scripts/check_update.sh --json       # output JSON for automation
#   ./scripts/check_update.sh --quiet      # only set exit code, no output

set -euo pipefail

REPO="zhan1206/aurora-os"
API_URL="https://api.github.com/repos/${REPO}/releases/latest"
LOCAL_VERSION="v3.3.0"

# Parse arguments
JSON_MODE=0
QUIET_MODE=0
for arg in "$@"; do
    case "$arg" in
        --json)  JSON_MODE=1 ;;
        --quiet) QUIET_MODE=1 ;;
        --help)  echo "Usage: $0 [--json] [--quiet]"; exit 0 ;;
    esac
done

# Function to extract version from GitHub API
fetch_latest_tag() {
    if command -v curl &>/dev/null; then
        curl -s --connect-timeout 10 "${API_URL}" 2>/dev/null | \
            grep -o '"tag_name": *"[^"]*"' | head -1 | \
            sed 's/.*"tag_name": *"\([^"]*\)".*/\1/'
    elif command -v wget &>/dev/null; then
        wget -q -O - --timeout=10 "${API_URL}" 2>/dev/null | \
            grep -o '"tag_name": *"[^"]*"' | head -1 | \
            sed 's/.*"tag_name": *"\([^"]*\)".*/\1/'
    else
        echo ""
    fi
}

# Fetch latest version
LATEST_TAG=$(fetch_latest_tag)

if [ -z "$LATEST_TAG" ]; then
    if [ "$QUIET_MODE" -eq 0 ]; then
        if [ "$JSON_MODE" -eq 1 ]; then
            echo '{"status":"error","message":"Cannot fetch GitHub releases"}'
        else
            echo "[UPDATE] Warning: Cannot fetch GitHub releases (network issue?)"
        fi
    fi
    exit 2
fi

# Strip leading 'v' if present for comparison
local_ver="${LOCAL_VERSION#v}"
latest_ver="${LATEST_TAG#v}"

# Compare versions
if [ "$local_ver" = "$latest_ver" ]; then
    if [ "$QUIET_MODE" -eq 0 ]; then
        if [ "$JSON_MODE" -eq 1 ]; then
            echo "{\"status\":\"up-to-date\",\"local\":\"${LOCAL_VERSION}\",\"latest\":\"${LATEST_TAG}\"}"
        else
            echo "[UPDATE] AuroraOS is up-to-date (${LOCAL_VERSION})"
            echo "  GitHub latest: ${LATEST_TAG}"
            echo "  Repository:    https://github.com/${REPO}"
        fi
    fi
    exit 0
else
    if [ "$QUIET_MODE" -eq 0 ]; then
        if [ "$JSON_MODE" -eq 1 ]; then
            echo "{\"status\":\"update-available\",\"local\":\"${LOCAL_VERSION}\",\"latest\":\"${LATEST_TAG}\"}"
        else
            echo ""
            echo "  ============================================"
            echo "  |  UPDATE AVAILABLE!                      |"
            echo "  ============================================"
            echo "  |  Current:  ${LOCAL_VERSION}"
            echo "  |  Latest:   ${LATEST_TAG}"
            echo "  |"
            echo "  |  Run: scripts/update.sh to update"
            echo "  |  Repo: https://github.com/${REPO}"
            echo "  ============================================"
            echo ""
        fi
    fi
    exit 1
fi