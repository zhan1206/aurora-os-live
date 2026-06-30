#!/usr/bin/env bash
# =============================================================================
#  AuroraOS Convenience Build Script
#  ─────────────────────────────────────────────────────────────────────────────
#  Inspired by CoolPotOS build.sh, provides one-command build & run.
#
#  Usage:
#    ./build.sh              - Release build ISO + run in QEMU
#    ./build.sh debug        - Debug build ISO + run in QEMU
#    ./build.sh iso          - Build ISO only
#    ./build.sh clean        - Clean build artifacts
#    ./build.sh format       - Format source code
#    ./build.sh test         - Build and run self-tests in QEMU
#    ./build.sh docker       - Build using Docker
# =============================================================================

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────────────
QEMU_MEM="${QEMU_MEM:-256M}"
QEMU_EXTRA="${QEMU_EXTRA:--no-reboot -nographic}"
ISO_FILE="${ISO_FILE:-os.iso}"
PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"

# ── Color helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*"; }

# ── Check dependencies ───────────────────────────────────────────────────────
check_deps() {
    local missing=()
    for cmd in make qemu-system-x86_64; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done
    if [ ${#missing[@]} -gt 0 ]; then
        err "Missing dependencies: ${missing[*]}"
        echo "Install with: sudo apt install build-essential xorriso grub-pc-bin mtools qemu-system-x86"
        exit 1
    fi
    ok "All dependencies satisfied"
}

# ── Build commands ───────────────────────────────────────────────────────────
do_build_release() {
    info "Building Release..."
    make clean 2>/dev/null || true
    make iso
    ok "Release build complete: ${ISO_FILE}"
}

do_build_debug() {
    info "Building Debug..."
    make clean 2>/dev/null || true
    make debug iso
    ok "Debug build complete: ${ISO_FILE}"
}

do_iso() {
    info "Building ISO..."
    make iso
    ok "ISO build complete: ${ISO_FILE}"
}

do_clean() {
    info "Cleaning build artifacts..."
    make clean
    rm -f "${ISO_FILE}"
    rm -rf build/
    ok "Clean complete"
}

do_format() {
    info "Formatting source code..."
    if [ -f "${PROJECT_ROOT}/format.sh" ]; then
        bash "${PROJECT_ROOT}/format.sh"
    else
        warn "format.sh not found, skipping"
    fi
}

do_run() {
    if [ ! -f "${ISO_FILE}" ]; then
        do_build_release
    fi
    info "Starting QEMU (${QEMU_MEM} RAM)..."
    qemu-system-x86_64 -m "${QEMU_MEM}" -cdrom "${ISO_FILE}" ${QEMU_EXTRA}
}

do_test() {
    info "Building and running self-tests..."
    make clean 2>/dev/null || true
    make debug iso
    info "Running QEMU with self-test verification..."
    qemu-system-x86_64 -m "${QEMU_MEM}" -cdrom "${ISO_FILE}" \
        ${QEMU_EXTRA} -d cpu_reset 2>&1 | tee qemu_test.log
    if grep -q "All Tests Passed" qemu_test.log; then
        ok "All self-tests passed!"
    else
        warn "Self-test verification inconclusive (check qemu_test.log)"
    fi
}

do_docker() {
    info "Building with Docker..."
    if ! command -v docker &>/dev/null; then
        err "Docker not found. Install Docker first."
        exit 1
    fi
    docker build -t aurora-os .
    docker run --rm -v "$(pwd)/output:/output" aurora-os
    ok "Docker build complete (check output/ directory)"
}

# ── Main ─────────────────────────────────────────────────────────────────────
check_deps

case "${1:-}" in
    debug)
        do_build_debug
        do_run
        ;;
    iso)
        do_iso
        ;;
    clean)
        do_clean
        ;;
    format)
        do_format
        ;;
    test)
        do_test
        ;;
    docker)
        do_docker
        ;;
    help|--help|-h)
        echo "AuroraOS Build Script"
        echo ""
        echo "Usage: $0 [command]"
        echo ""
        echo "Commands:"
        echo "  (none)        Release build + run in QEMU"
        echo "  debug         Debug build + run in QEMU"
        echo "  iso           Build ISO only"
        echo "  clean         Clean build artifacts"
        echo "  format        Format source code"
        echo "  test          Build and run self-tests in QEMU"
        echo "  docker        Build using Docker"
        echo "  help          Show this help"
        ;;
    *)
        do_build_release
        do_run
        ;;
esac