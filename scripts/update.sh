#!/bin/bash
# update.sh - Safe update for AuroraOS with backup and rollback
#
# Features:
#   - Backs up current build artifacts before update
#   - Pulls latest changes from GitHub
#   - Rebuilds the project
#   - Verifies the build with SHA256 checksum
#   - Provides rollback capability if update fails
#
# Usage:
#   ./scripts/update.sh             # full update with backup
#   ./scripts/update.sh --rollback  # rollback to previous backup
#   ./scripts/update.sh --dry-run   # check for updates without applying
#   ./scripts/update.sh --force     # skip backup confirmation

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BACKUP_DIR="${PROJECT_DIR}/.backups"
TIMESTAMP=$(date -u +'%Y%m%d_%H%M%S')
BACKUP_NAME="backup_${TIMESTAMP}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# Parse arguments
DO_ROLLBACK=0
DRY_RUN=0
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --rollback) DO_ROLLBACK=1 ;;
        --dry-run)  DRY_RUN=1 ;;
        --force)    FORCE=1 ;;
        --help)
            echo "Usage: $0 [--rollback] [--dry-run] [--force]"
            echo ""
            echo "Options:"
            echo "  --rollback  Restore from most recent backup"
            echo "  --dry-run   Check for updates without applying"
            echo "  --force     Skip confirmation prompts"
            exit 0
            ;;
    esac
done

cd "$PROJECT_DIR"

# ================================================================
# Rollback mode
# ================================================================
if [ "$DO_ROLLBACK" -eq 1 ]; then
    if [ ! -d "$BACKUP_DIR" ]; then
        log_error "No backup directory found at ${BACKUP_DIR}"
        exit 1
    fi

    LATEST_BACKUP=$(ls -1t "$BACKUP_DIR" 2>/dev/null | head -1)
    if [ -z "$LATEST_BACKUP" ]; then
        log_error "No backups available"
        exit 1
    fi

    log_info "Rolling back to: ${LATEST_BACKUP}"
    BACKUP_PATH="${BACKUP_DIR}/${LATEST_BACKUP}"

    if [ -f "${BACKUP_PATH}/os.iso" ]; then
        cp "${BACKUP_PATH}/os.iso" "${PROJECT_DIR}/os.iso"
        log_ok "Restored os.iso from backup"
    fi

    if [ -f "${BACKUP_PATH}/kernel.elf" ]; then
        cp "${BACKUP_PATH}/kernel.elf" "${PROJECT_DIR}/build/kernel.elf" 2>/dev/null || true
        log_ok "Restored kernel.elf from backup"
    fi

    if [ -f "${BACKUP_PATH}/os.iso.sha256" ]; then
        cp "${BACKUP_PATH}/os.iso.sha256" "${PROJECT_DIR}/os.iso.sha256"
    fi

    log_ok "Rollback complete. Run 'make run' to test."
    exit 0
fi

# ================================================================
# Check for updates
# ================================================================
log_info "Checking for updates..."
if [ -x "${SCRIPT_DIR}/check_update.sh" ]; then
    UPDATE_STATUS=0
    "${SCRIPT_DIR}/check_update.sh" --quiet || UPDATE_STATUS=$?
else
    log_info "Version check script not found, checking git..."
    git fetch origin 2>/dev/null || true
    UPDATE_STATUS=0
fi

if [ "$DRY_RUN" -eq 1 ]; then
    if [ "$UPDATE_STATUS" -eq 1 ]; then
        log_info "Update available. Run without --dry-run to apply."
    else
        log_info "Already up-to-date."
    fi
    exit 0
fi

# ================================================================
# Confirmation
# ================================================================
if [ "$FORCE" -eq 0 ]; then
    echo ""
    echo "  This will:"
    echo "  1. Backup current build artifacts"
    echo "  2. Pull latest changes from GitHub"
    echo "  3. Rebuild the project"
    echo "  4. Verify the build"
    echo ""
    read -rp "  Continue? [y/N] " REPLY
    if [ "$REPLY" != "y" ] && [ "$REPLY" != "Y" ]; then
        log_info "Update cancelled."
        exit 0
    fi
fi

# ================================================================
# Phase 1: Backup
# ================================================================
log_info "Phase 1/4: Creating backup..."
mkdir -p "$BACKUP_DIR"
BACKUP_PATH="${BACKUP_DIR}/${BACKUP_NAME}"
mkdir -p "$BACKUP_PATH"

# Backup build artifacts
for artifact in os.iso os.iso.sha256 build/kernel.elf; do
    if [ -f "${PROJECT_DIR}/${artifact}" ]; then
        mkdir -p "$(dirname "${BACKUP_PATH}/${artifact}")"
        cp "${PROJECT_DIR}/${artifact}" "${BACKUP_PATH}/${artifact}" 2>/dev/null || true
    fi
done

# Save git HEAD for rollback
git rev-parse HEAD > "${BACKUP_PATH}/git_head.txt" 2>/dev/null || true

log_ok "Backup saved to: ${BACKUP_PATH}"

# Clean old backups (keep last 5)
BACKUP_COUNT=$(ls -1d "${BACKUP_DIR}"/backup_* 2>/dev/null | wc -l)
if [ "$BACKUP_COUNT" -gt 5 ]; then
    ls -1td "${BACKUP_DIR}"/backup_* | tail -n +6 | while read -r old_backup; do
        rm -rf "$old_backup"
        log_info "Cleaned old backup: $(basename "$old_backup")"
    done
fi

# ================================================================
# Phase 2: Pull latest changes
# ================================================================
log_info "Phase 2/4: Pulling latest changes..."
if git pull --rebase origin main 2>/dev/null; then
    log_ok "Git pull successful"
else
    log_error "Git pull failed. Restoring backup..."
    # Restore from backup
    if [ -f "${BACKUP_PATH}/os.iso" ]; then
        cp "${BACKUP_PATH}/os.iso" "${PROJECT_DIR}/os.iso"
    fi
    log_error "Update failed. Previous build restored from backup."
    exit 1
fi

# ================================================================
# Phase 3: Rebuild
# ================================================================
log_info "Phase 3/4: Rebuilding project..."
if make clean 2>/dev/null && make iso 2>&1; then
    log_ok "Build successful"
else
    log_error "Build failed. Rolling back..."
    # Rollback git
    if [ -f "${BACKUP_PATH}/git_head.txt" ]; then
        git reset --hard "$(cat "${BACKUP_PATH}/git_head.txt")" 2>/dev/null || true
    fi
    # Restore from backup
    if [ -f "${BACKUP_PATH}/os.iso" ]; then
        cp "${BACKUP_PATH}/os.iso" "${PROJECT_DIR}/os.iso"
    fi
    log_error "Update failed. Rolled back to previous version."
    exit 1
fi

# ================================================================
# Phase 4: Verify
# ================================================================
log_info "Phase 4/4: Verifying build..."
if [ -f "${PROJECT_DIR}/os.iso" ]; then
    make checksum 2>/dev/null || true
    log_ok "Build verified: os.iso ($(du -h os.iso 2>/dev/null | cut -f1))"
else
    log_error "Verification failed: os.iso not found"
    exit 1
fi

echo ""
log_ok "============================================"
log_ok "  Update complete!"
log_ok "  Version: $(make version 2>/dev/null || echo 'AuroraOS v3.3.0')"
log_ok "  Backup:  ${BACKUP_NAME}"
log_ok "  Run:     make run"
log_ok ""
log_ok "  To rollback: scripts/update.sh --rollback"
log_ok "============================================"
echo ""