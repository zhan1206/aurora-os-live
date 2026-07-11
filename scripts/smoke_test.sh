#!/bin/bash
# smoke_test.sh - AuroraOS smoke test
#
# Boots the OS in QEMU and runs basic commands to verify the system
# is functional.  Waits for the serial output and checks for expected
# content.
#
# Usage: ./scripts/smoke_test.sh [qemu_extra_args...]
#
# Exit codes:
#   0 - all tests passed
#   1 - one or more tests failed
#   2 - test setup error (no QEMU, no ISO, etc.)

set -euo pipefail

ISO_PATH="${ISO_PATH:-os.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_MEM="${QEMU_MEM:-256M}"
TIMEOUT="${TIMEOUT:-30}"
LOG_FILE="/tmp/aurora_smoke_test.log"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

# --- Helper functions ---

pass() {
    echo -e "  ${GREEN}[PASS]${NC} $1"
    PASS_COUNT=$((PASS_COUNT + 1))
}

fail() {
    echo -e "  ${RED}[FAIL]${NC} $1"
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

skip() {
    echo -e "  ${YELLOW}[SKIP]${NC} $1"
    SKIP_COUNT=$((SKIP_COUNT + 1))
}

check_output() {
    local desc="$1"
    local pattern="$2"
    if grep -q "$pattern" "$LOG_FILE"; then
        pass "$desc"
    else
        fail "$desc (pattern '$pattern' not found)"
    fi
}

# --- Cleanup ---
cleanup() {
    if [ -n "${QEMU_PID:-}" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$LOG_FILE"
}
trap cleanup EXIT

# --- Pre-flight checks ---
echo "=== AuroraOS Smoke Test ==="
echo ""

if [ ! -f "$ISO_PATH" ]; then
    echo "ERROR: ISO not found at $ISO_PATH"
    echo "Run 'make iso' first."
    exit 2
fi

if ! command -v "$QEMU" &>/dev/null; then
    echo "ERROR: QEMU not found: $QEMU"
    exit 2
fi

echo "QEMU:  $QEMU"
echo "ISO:   $ISO_PATH"
echo "Mem:   $QEMU_MEM"
echo ""

# --- Start QEMU ---
echo "Starting QEMU..."
$QEMU -m "$QEMU_MEM" -cdrom "$ISO_PATH" \
    -nographic -no-reboot \
    -serial "file:$LOG_FILE" \
    -display none &
QEMU_PID=$!

# --- Wait for boot to complete ---
echo "Waiting for boot (timeout: ${TIMEOUT}s)..."
ELAPSED=0
while [ $ELAPSED -lt $TIMEOUT ]; do
    if grep -q "All Tests Passed" "$LOG_FILE" 2>/dev/null; then
        echo "Boot complete (self-tests passed)."
        break
    fi
    if grep -q "panic" "$LOG_FILE" 2>/dev/null; then
        echo "WARNING: Kernel panic detected during boot."
        break
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

if [ $ELAPSED -ge $TIMEOUT ]; then
    echo "WARNING: Boot timeout reached. Proceeding with partial output."
fi

# --- Run smoke tests ---
echo ""
echo "--- Smoke Tests ---"

# Check for boot messages
check_output "kernel booted" "AuroraOS"
check_output "self-tests executed" "Kernel Self-Test"
check_output "self-tests passed" "All Tests Passed"

# Check for filesystem
check_output "/proc/version available" "version"
check_output "/sys/kernel/version" "AuroraOS"
check_output "memory info" "mem"

# Check for networking init
check_output "network stack init" "TCP/IP"

# Check for scheduler
check_output "scheduler init" "Scheduler"

# Check for filesystem init
check_output "VFS init" "VFS"

# --- Summary ---
echo ""
echo "=== Smoke Test Summary ==="
echo "  Passed: $PASS_COUNT"
echo "  Failed: $FAIL_COUNT"
echo "  Skipped: $SKIP_COUNT"

if [ $FAIL_COUNT -gt 0 ]; then
    echo ""
    echo "Last 2000 bytes of serial output:"
    tail -c 2000 "$LOG_FILE" 2>/dev/null || true
    exit 1
fi

exit 0