#!/bin/bash
# quickstart.sh - One-command setup and run for AuroraOS
#
# Detects environment, installs dependencies, builds and runs.
# Supports: Ubuntu/Debian, Fedora, Arch, macOS (Homebrew), WSL
#
# Usage:
#   ./scripts/quickstart.sh           # auto-detect and run
#   ./scripts/quickstart.sh --check   # check environment only
#   ./scripts/quickstart.sh --setup   # install dependencies only
#   ./scripts/quickstart.sh --build   # build only
#   ./scripts/quickstart.sh --run     # build and run in QEMU

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

log_ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# Detect OS
detect_os() {
    case "$(uname -s)" in
        Linux*)
            if [ -f /etc/os-release ]; then
                . /etc/os-release
                echo "${ID}"
            elif [ -f /etc/debian_version ]; then
                echo "debian"
            else
                echo "linux"
            fi
            ;;
        Darwin*)  echo "macos" ;;
        MINGW*|MSYS*|CYGWIN*) echo "windows" ;;
        *)        echo "unknown" ;;
    esac
}

OS=$(detect_os)

# ================================================================
# Phase 1: Environment Check
# ================================================================
check_environment() {
    echo ""
    echo "  ============================================"
    echo "  |  AuroraOS Environment Check              |"
    echo "  ============================================"
    echo "  OS: $(uname -s) ($(uname -m))"
    echo ""

    local all_ok=1

    check_tool() {
        local tool="$1"
        local pkg_hint="$2"
        if command -v "$tool" &>/dev/null; then
            log_ok "$tool found: $(command -v "$tool")"
        else
            log_warn "$tool NOT found${pkg_hint:+ (install: $pkg_hint)}"
            all_ok=0
        fi
    }

    check_tool "gcc"       "build-essential / gcc"
    check_tool "ld"        "binutils"
    check_tool "objcopy"   "binutils"
    check_tool "make"      "build-essential / make"
    check_tool "git"       "git"

    # Optional tools
    if command -v qemu-system-x86_64 &>/dev/null; then
        log_ok "qemu-system-x86_64 found"
    else
        log_warn "qemu-system-x86_64 NOT found (needed for 'make run')"
    fi

    if command -v grub-mkrescue &>/dev/null; then
        log_ok "grub-mkrescue found"
    else
        log_warn "grub-mkrescue NOT found (needed for 'make iso')"
    fi

    if command -v x86_64-elf-gcc &>/dev/null; then
        log_ok "x86_64-elf-gcc found (cross-compiler)"
    else
        log_info "x86_64-elf-gcc not found (will use system gcc)"
    fi

    echo ""
    return $all_ok
}

# ================================================================
# Phase 2: Install Dependencies
# ================================================================
install_dependencies() {
    echo ""
    log_info "Installing dependencies for: ${OS}"

    case "$OS" in
        ubuntu|debian)
            echo "  Running: apt-get update && apt-get install -y ..."
            sudo apt-get update -qq
            sudo apt-get install -y -qq \
                build-essential \
                binutils \
                gcc \
                make \
                git \
                grub-pc-bin \
                grub-common \
                xorriso \
                qemu-system-x86 \
                mtools \
                2>/dev/null || log_warn "Some packages may not have installed"
            ;;
        fedora)
            sudo dnf install -y \
                gcc gcc-c++ binutils make git \
                grub2-tools xorriso qemu-system-x86 mtools \
                2>/dev/null || log_warn "Some packages may not have installed"
            ;;
        arch)
            sudo pacman -S --noconfirm \
                base-devel gcc binutils make git \
                grub xorriso qemu-desktop mtools \
                2>/dev/null || log_warn "Some packages may not have installed"
            ;;
        macos)
            if command -v brew &>/dev/null; then
                brew install \
                    x86_64-elf-gcc x86_64-elf-binutils \
                    qemu grub xorriso mtools make \
                    2>/dev/null || log_warn "Some packages may not have installed"
            else
                log_error "Homebrew not found. Please install: https://brew.sh"
                return 1
            fi
            ;;
        windows)
            log_info "On Windows, please use WSL (Windows Subsystem for Linux):"
            echo "  1. Install WSL: wsl --install"
            echo "  2. Run: wsl"
            echo "  3. Then: cd /mnt/d/自制操作系统 && ./scripts/quickstart.sh"
            echo ""
            log_info "Or use the PowerShell setup script:"
            echo "  powershell -File scripts/setup-wsl.ps1"
            return 0
            ;;
        *)
            log_warn "Unknown OS. Please install manually:"
            echo "  - GCC cross-compiler (x86_64-elf-gcc) or system GCC"
            echo "  - GNU Make"
            echo "  - GRUB2 + xorriso (for ISO creation)"
            echo "  - QEMU (for running)"
            return 0
            ;;
    esac

    log_ok "Dependencies installed"
    return 0
}

# ================================================================
# Phase 3: Build
# ================================================================
build_project() {
    echo ""
    log_info "Building AuroraOS..."

    cd "$PROJECT_DIR"

    if [ ! -f Makefile ]; then
        log_error "Makefile not found in ${PROJECT_DIR}"
        return 1
    fi

    # Clean and build
    make clean 2>/dev/null || true
    if make iso 2>&1; then
        log_ok "Build successful"
        if [ -f os.iso ]; then
            log_ok "ISO created: $(du -h os.iso 2>/dev/null | cut -f1)"
        fi
        # Generate checksum
        make checksum 2>/dev/null || true
        return 0
    else
        log_error "Build failed"
        return 1
    fi
}

# ================================================================
# Phase 4: Run
# ================================================================
run_project() {
    echo ""
    log_info "Starting AuroraOS in QEMU..."

    cd "$PROJECT_DIR"

    if [ ! -f os.iso ]; then
        log_error "os.iso not found. Build first with: make iso"
        return 1
    fi

    if command -v qemu-system-x86_64 &>/dev/null; then
        log_ok "Press Ctrl+A then X to exit QEMU"
        echo ""
        qemu-system-x86_64 -m 256M -cdrom os.iso -nographic -no-reboot
    else
        log_error "qemu-system-x86_64 not found. Install QEMU first."
        return 1
    fi
}

# ================================================================
# Main
# ================================================================
main() {
    local do_check=0
    local do_setup=0
    local do_build=0
    local do_run=0

    # Parse arguments
    if [ $# -eq 0 ]; then
        # Default: do everything
        do_check=1
        do_setup=1
        do_build=1
        do_run=1
    else
        for arg in "$@"; do
            case "$arg" in
                --check) do_check=1 ;;
                --setup) do_setup=1 ;;
                --build) do_build=1 ;;
                --run)   do_run=1 ;;
                --help)
                    echo "Usage: $0 [--check] [--setup] [--build] [--run]"
                    echo ""
                    echo "No arguments: full setup + build + run"
                    exit 0
                    ;;
                *) log_error "Unknown option: $arg"; exit 1 ;;
            esac
        done
    fi

    echo ""
    echo "  ============================================"
    echo "  |  AuroraOS Quickstart v3.3.0               |"
    echo "  |  One-command setup, build, and run        |"
    echo "  ============================================"

    # Check environment
    if [ "$do_check" -eq 1 ]; then
        check_environment || log_warn "Some tools are missing. Running setup..."
    fi

    # Install dependencies
    if [ "$do_setup" -eq 1 ]; then
        install_dependencies
    fi

    # Build
    if [ "$do_build" -eq 1 ]; then
        build_project || exit 1
    fi

    # Run
    if [ "$do_run" -eq 1 ]; then
        run_project
    fi

    echo ""
    log_ok "Quickstart complete!"
}

main "$@"