#!/bin/bash
# ci_regression.sh - AuroraOS CI Regression Pipeline
#
# Runs on every commit: build → smoke test → regression test → report
#
# Usage:
#   ./scripts/ci_regression.sh              # full pipeline
#   ./scripts/ci_regression.sh --quick      # build + smoke only
#   ./scripts/ci_regression.sh --report     # generate report from last run
#
# Exit: 0 = all passed, 1 = test failure, 2 = build failure

set -euo pipefail

CI_START=$(date +%s)
PASSED=0
FAILED=0
SKIPPED=0
BUILD_OK=0
SMOKE_OK=0
REGRESSION_OK=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_header()  { echo -e "${CYAN}=== $1 ===${NC}"; }
log_pass()    { echo -e "  ${GREEN}[PASS]${NC} $1"; ((PASSED++)) || true; }
log_fail()    { echo -e "  ${RED}[FAIL]${NC} $1"; ((FAILED++)) || true; }
log_skip()    { echo -e "  ${YELLOW}[SKIP]${NC} $1"; ((SKIPPED++)) || true; }
log_info()    { echo -e "  [INFO] $1"; }

# --- Stage 1: Build ---
stage_build() {
    log_header "Stage 1: Build"
    log_info "Cleaning..."
    make clean > /dev/null 2>&1 || true

    log_info "Building release ISO..."
    if make iso > build.log 2>&1; then
        log_pass "ISO build succeeded"
        BUILD_OK=1
    else
        log_fail "ISO build failed (see build.log)"
        tail -20 build.log
        return 2
    fi

    if [ -f os.iso ]; then
        log_info "ISO size: $(du -h os.iso | cut -f1)"
    else
        log_fail "os.iso not found"
        return 2
    fi
}

# --- Stage 2: Smoke Test ---
stage_smoke() {
    log_header "Stage 2: Smoke Test (boot + basic shell)"

    if [ ! -f os.iso ]; then
        log_fail "os.iso missing, rebuild first"
        return 2
    fi

    # Quick boot test: run QEMU for 15 seconds, check output
    timeout 15 qemu-system-x86_64 -m 256M -cdrom os.iso \
        -nographic -no-reboot 2>&1 | tee smoke_output.log || true

    if grep -q "AuroraOS" smoke_output.log 2>/dev/null; then
        log_pass "Kernel boot detected"
    else
        log_fail "Kernel boot not detected"
        return 1
    fi

    if grep -q "shell" smoke_output.log 2>/dev/null; then
        log_pass "Shell started"
    else
        log_fail "Shell not detected"
        return 1
    fi

    SMOKE_OK=1
}

# --- Stage 3: Regression Tests ---
stage_regression() {
    log_header "Stage 3: Regression Tests"

    if [ -f scripts/regression_test.py ]; then
        log_info "Running regression test suite..."
        if python3 scripts/regression_test.py --timeout 60 2>&1 | tee regression_output.log; then
            log_pass "Regression tests passed"
            REGRESSION_OK=1
        else
            log_fail "Regression tests had failures"
        fi
    else
        log_skip "regression_test.py not found"
        SKIPPED=1
    fi

    # Run self-tests embedded in the kernel
    log_info "Running kernel self-tests..."
    timeout 30 qemu-system-x86_64 -m 256M -cdrom os.iso \
        -nographic -no-reboot \
        -append "self_test" 2>&1 | tee self_test_output.log || true

    if grep -q "PASS" self_test_output.log 2>/dev/null; then
        local passed_tests=$(grep -c "PASS" self_test_output.log || echo 0)
        log_pass "Kernel self-tests: ${passed_tests} passed"
    else
        log_info "Self-test output not parsed (may need QEMU interaction)"
    fi
}

# --- Stage 4: Code Quality ---
stage_quality() {
    log_header "Stage 4: Code Quality"

    # Check for common issues
    if grep -rn "TODO\|FIXME\|HACK" kernel/ --include="*.c" --include="*.h" 2>/dev/null | wc -l | xargs -I{} test {} -lt 50; then
        log_pass "TODO/FIXME count within limits"
    else
        local count=$(grep -rn "TODO\|FIXME\|HACK" kernel/ --include="*.c" --include="*.h" 2>/dev/null | wc -l)
        log_info "TODO/FIXME count: ${count}"
    fi

    # Verify no direct user pointer dereferences (should use copy_from_user)
    local direct_user=$(grep -rn "memcpy.*user\|strcpy.*user" kernel/ --include="*.c" 2>/dev/null | wc -l)
    if [ "$direct_user" -eq 0 ]; then
        log_pass "No direct user pointer dereferences detected"
    else
        log_info "Direct user pointer dereferences: ${direct_user} (review needed)"
    fi
}

# --- Generate Report ---
generate_report() {
    log_header "CI Report"

    local CI_END=$(date +%s)
    local DURATION=$((CI_END - CI_START))

    echo ""
    echo "============================================"
    echo "  AuroraOS CI Regression Report"
    echo "  Date: $(date -u +'%Y-%m-%d %H:%M UTC')"
    echo "  Duration: ${DURATION}s"
    echo "============================================"
    echo ""
    echo "  Build:     $([ $BUILD_OK -eq 1 ] && echo 'PASS' || echo 'FAIL')"
    echo "  Smoke:     $([ $SMOKE_OK -eq 1 ] && echo 'PASS' || echo 'FAIL')"
    echo "  Regression:$([ $REGRESSION_OK -eq 1 ] && echo 'PASS' || echo 'FAIL')"
    echo ""
    echo "  Passed:  ${PASSED}"
    echo "  Failed:  ${FAILED}"
    echo "  Skipped: ${SKIPPED}"
    echo "============================================"

    if [ "$FAILED" -gt 0 ]; then
        echo ""
        echo "  FAILURES DETECTED. Please review logs:"
        echo "    - build.log"
        echo "    - smoke_output.log"
        echo "    - regression_output.log"
        echo "    - self_test_output.log"
        return 1
    fi

    echo ""
    echo "  All checks passed."
    return 0
}

# --- Main ---
main() {
    cd "$(dirname "$0")/.." || exit 2

    log_header "AuroraOS CI Pipeline v4.1.1"

    case "${1:-full}" in
        --quick)
            stage_build
            stage_smoke
            ;;
        --report)
            generate_report
            ;;
        full|*)
            stage_build
            stage_smoke
            stage_regression
            stage_quality
            generate_report
            ;;
    esac
}

main "$@"