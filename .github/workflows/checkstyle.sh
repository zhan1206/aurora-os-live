#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
#  AuroraOS Code Style Checker
#  ─────────────────────────────────────────────────────────────────────
#  Checks:
#    1. Function naming convention (snake_case)
#    2. Struct naming convention (snake_case)
#    3. Macro naming convention (UPPER_CASE)
#    4. 4-space indentation (no tabs)
#    5. Line width <= 100 characters
#    6. Header file guards (#pragma once or #ifndef)
#    7. No trailing whitespace
#    8. No mixed tabs/spaces
#
#  Outputs GitHub Actions annotations (::warning / ::error format).
#  Exit 0 if all checks pass, 1 if issues found.
# ─────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────
SRC_DIRS=("kernel" "arch" "boot" "userspace")
FILE_EXTS=("*.c" "*.h" "*.S")
MAX_LINE_WIDTH=100
ISSUES=0
IS_GITHUB_CI="${GITHUB_ACTIONS:-false}"

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

# Build a list of all source files to check
build_file_list() {
    local files=()
    for dir in "${SRC_DIRS[@]}"; do
        if [ -d "$dir" ]; then
            for ext in "${FILE_EXTS[@]}"; do
                local found
                found=$(find "$dir" -type f -name "$ext" 2>/dev/null || true)
                if [ -n "$found" ]; then
                    while IFS= read -r f; do
                        files+=("$f")
                    done <<< "$found"
                fi
            done
        fi
    done
    echo "${files[@]}"
}

echo "=============================================="
echo " AuroraOS Code Style Check"
echo "=============================================="
echo ""

# ── 1. Tab characters check ──────────────────────────────────────────
echo "::group::[1/8] Checking for tab characters..."

TAB_FILES=$(grep -rlP '\t' "${SRC_DIRS[@]}" 2>/dev/null || true)
if [ -n "$TAB_FILES" ]; then
    while IFS= read -r f; do
        # Find line numbers with tabs
        while IFS= read -r linenum; do
            github_error "$f" "$linenum" "Tab character found (use 4 spaces for indentation)"
            ISSUES=$((ISSUES + 1))
        done < <(grep -nP '\t' "$f" 2>/dev/null | cut -d: -f1 || true)
    done <<< "$TAB_FILES"
    echo "  FAIL: Tab characters found in $(echo "$TAB_FILES" | wc -l) file(s)"
else
    echo "  PASS: No tab characters found"
fi
echo "::endgroup::"

# ── 2. Trailing whitespace ───────────────────────────────────────────
echo "::group::[2/8] Checking for trailing whitespace..."

TRAIL_WS=$(grep -rlP '[ \t]+$' "${SRC_DIRS[@]}" 2>/dev/null || true)
if [ -n "$TRAIL_WS" ]; then
    while IFS= read -r f; do
        while IFS= read -r linenum; do
            github_warning "$f" "$linenum" "Trailing whitespace"
            ISSUES=$((ISSUES + 1))
        done < <(grep -nP '[ \t]+$' "$f" 2>/dev/null | cut -d: -f1 || true)
    done <<< "$TRAIL_WS"
    echo "  WARN: Trailing whitespace found in $(echo "$TRAIL_WS" | wc -l) file(s)"
else
    echo "  PASS: No trailing whitespace"
fi
echo "::endgroup::"

# ── 3. Mixed tabs/spaces ─────────────────────────────────────────────
echo "::group::[3/8] Checking for mixed tabs/spaces..."

MIXED_FILES=$(grep -rlP '^(\t+ +| +\t+)' "${SRC_DIRS[@]}" 2>/dev/null || true)
if [ -n "$MIXED_FILES" ]; then
    while IFS= read -r f; do
        while IFS= read -r linenum; do
            github_error "$f" "$linenum" "Mixed tabs and spaces on line"
            ISSUES=$((ISSUES + 1))
        done < <(grep -nP '^(\t+ +| +\t+)' "$f" 2>/dev/null | cut -d: -f1 || true)
    done <<< "$MIXED_FILES"
    echo "  FAIL: Mixed tabs/spaces found in $(echo "$MIXED_FILES" | wc -l) file(s)"
else
    echo "  PASS: No mixed tabs/spaces"
fi
echo "::endgroup::"

# ── 4. Line width check ──────────────────────────────────────────────
echo "::group::[4/8] Checking line width (max ${MAX_LINE_WIDTH} chars)..."

LONG_LINES=0
for f in $(build_file_list); do
    while IFS= read -r linenum; do
        github_warning "$f" "$linenum" "Line exceeds ${MAX_LINE_WIDTH} characters"
        LONG_LINES=$((LONG_LINES + 1))
    done < <(awk "length > ${MAX_LINE_WIDTH} {print NR}" "$f" 2>/dev/null || true)
done

if [ "$LONG_LINES" -gt 0 ]; then
    echo "  WARN: $LONG_LINES long line(s) found"
    ISSUES=$((ISSUES + LONG_LINES))
else
    echo "  PASS: All lines within ${MAX_LINE_WIDTH} characters"
fi
echo "::endgroup::"

# ── 5. Function naming (snake_case) ──────────────────────────────────
echo "::group::[5/8] Checking function naming convention (snake_case)..."

# Look for function definitions: return_type name(  (not a keyword, not ending with _t)
# This regex matches C function definitions: type [*]function_name(
FUNC_VIOLATIONS=0
for f in $(build_file_list); do
    while IFS= read -r line_info; do
        linenum=$(echo "$line_info" | cut -d: -f1)
        func_name=$(echo "$line_info" | cut -d: -f2)
        # Skip known macro patterns, assembly labels, and common patterns
        if [[ "$func_name" =~ ^[A-Z_]+$ ]]; then
            continue  # All-caps names are fine (likely macros/constants in context)
        fi
        # Check for non-snake_case: contains uppercase letters (except at start for special cases)
        if [[ "$func_name" =~ [A-Z] ]] && ! [[ "$func_name" =~ ^_?[a-z][a-z0-9_]*$ ]]; then
            github_warning "$f" "$linenum" "Function '$func_name' should use snake_case naming"
            FUNC_VIOLATIONS=$((FUNC_VIOLATIONS + 1))
        fi
    done < <(grep -nP '^\s*(?:static\s+)?(?:inline\s+)?(?:__attribute__\s*\(\([^)]*\)\)\s+)?(?:const\s+)?(?:volatile\s+)?(?:unsigned\s+)?(?:signed\s+)?(?:struct\s+)?[a-zA-Z_][a-zA-Z0-9_]*\s*\**\s+\b([a-zA-Z_][a-zA-Z0-9_]*)\s*\([^)]*\)\s*\{?' "$f" 2>/dev/null | sed -E 's/^([0-9]+):.*\b([a-zA-Z_][a-zA-Z0-9_]*)\s*\([^)]*\)\s*\{?/\1:\2/' || true)
done

if [ "$FUNC_VIOLATIONS" -gt 0 ]; then
    echo "  WARN: $FUNC_VIOLATIONS function(s) with non-snake_case naming"
    ISSUES=$((ISSUES + FUNC_VIOLATIONS))
else
    echo "  PASS: Function naming is snake_case"
fi
echo "::endgroup::"

# ── 6. Struct naming (snake_case) ────────────────────────────────────
echo "::group::[6/8] Checking struct naming convention (snake_case)..."

STRUCT_VIOLATIONS=0
for f in $(build_file_list); do
    while IFS= read -r line_info; do
        linenum=$(echo "$line_info" | cut -d: -f1)
        struct_name=$(echo "$line_info" | cut -d: -f2)
        # _t suffix is acceptable for typedef'd structs
        if [[ "$struct_name" =~ [A-Z] ]] && ! [[ "$struct_name" =~ ^[A-Z_][A-Z0-9_]*$ ]]; then
            # Allow UPPER_CASE names (they are fine)
            if [[ "$struct_name" =~ ^[A-Z_]+$ ]]; then
                continue
            fi
            github_warning "$f" "$linenum" "Struct '$struct_name' should use snake_case naming"
            STRUCT_VIOLATIONS=$((STRUCT_VIOLATIONS + 1))
        fi
    done < <(grep -nP '^\s*(?:typedef\s+)?struct\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\{?' "$f" 2>/dev/null | sed -E 's/^([0-9]+):.*struct\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\{?/\1:\2/' || true)
done

if [ "$STRUCT_VIOLATIONS" -gt 0 ]; then
    echo "  WARN: $STRUCT_VIOLATIONS struct(s) with non-snake_case naming"
    ISSUES=$((ISSUES + STRUCT_VIOLATIONS))
else
    echo "  PASS: Struct naming is snake_case"
fi
echo "::endgroup::"

# ── 7. Macro naming (UPPER_CASE) ─────────────────────────────────────
echo "::group::[7/8] Checking macro naming convention (UPPER_CASE)..."

MACRO_VIOLATIONS=0
for f in $(build_file_list); do
    while IFS= read -r line_info; do
        linenum=$(echo "$line_info" | cut -d: -f1)
        macro_name=$(echo "$line_info" | cut -d: -f2)
        # Skip known lowercase function-like macros (min, max, offsetof, etc.)
        if [[ "$macro_name" =~ ^(min|max|offsetof|container_of|likely|unlikely|ARRAY_SIZE|countof|nil|DEBUG|console_color|VGA_ENTRY|DEFAULT_VGA_ATTR|BOOT_|STATUS_|SHELL_|PS_|MEM_|LOGIN_|PANIC_|PTE_|PAGE_|LOG_|INBUF_|MAX_|PROT_|SIG|SYS_|SYS_|MAP_|O_|SEEK_|SEEK_|DT_|ELF|ELF64|VERSION|STB_|STT_|SHN_|SHT_|PF_|R_|GRUB|MULTIBOOT|UEFI|EFI) ]]; then
            continue
        fi
        # Check if it contains lowercase letters
        if [[ "$macro_name" =~ [a-z] ]]; then
            github_warning "$f" "$linenum" "Macro '#define $macro_name' should use UPPER_CASE naming"
            MACRO_VIOLATIONS=$((MACRO_VIOLATIONS + 1))
        fi
    done < <(grep -nP '^\s*#\s*define\s+([a-zA-Z_][a-zA-Z0-9_]*)\b(?!\s*\()' "$f" 2>/dev/null | sed -E 's/^([0-9]+):\s*#\s*define\s+([a-zA-Z_][a-zA-Z0-9_]*)\b.*/\1:\2/' || true)
done

if [ "$MACRO_VIOLATIONS" -gt 0 ]; then
    echo "  WARN: $MACRO_VIOLATIONS macro(s) with non-UPPER_CASE naming"
    ISSUES=$((ISSUES + MACRO_VIOLATIONS))
else
    echo "  PASS: Macro naming is UPPER_CASE"
fi
echo "::endgroup::"

# ── 8. Header guards ─────────────────────────────────────────────────
echo "::group::[8/8] Checking header guards (#pragma once or #ifndef)..."

GUARD_VIOLATIONS=0
for f in $(build_file_list); do
    if [[ "$f" != *.h ]]; then
        continue
    fi
    if grep -qP '^\s*#pragma\s+once' "$f" 2>/dev/null; then
        continue  # OK
    fi
    if grep -qP '^\s*#ifndef\s+_[A-Z]' "$f" 2>/dev/null; then
        continue  # OK
    fi
    # Check if it's a header with actual C declarations (not just comments or empty)
    if grep -qP '^\s*(typedef|struct|enum|extern|void|int|char|uint|size_t|#define|static\s+inline|__attribute__)' "$f" 2>/dev/null; then
        github_error "$f" "1" "Header file missing include guard (#pragma once or #ifndef)"
        GUARD_VIOLATIONS=$((GUARD_VIOLATIONS + 1))
    fi
done

if [ "$GUARD_VIOLATIONS" -gt 0 ]; then
    echo "  FAIL: $GUARD_VIOLATIONS header(s) missing guard"
    ISSUES=$((ISSUES + GUARD_VIOLATIONS))
else
    echo "  PASS: All headers have guards"
fi
echo "::endgroup::"

# ── Summary ──────────────────────────────────────────────────────────
echo ""
echo "=============================================="
if [ "$ISSUES" -gt 0 ]; then
    echo " RESULT: $ISSUES issue(s) found"
    echo "=============================================="
    echo ""
    echo "::warning::$ISSUES code style issue(s) found"
    if [ "$IS_GITHUB_CI" = "true" ]; then
        echo "## Code Style Check" >> "$GITHUB_STEP_SUMMARY"
        echo "" >> "$GITHUB_STEP_SUMMARY"
        echo "| Check | Result |" >> "$GITHUB_STEP_SUMMARY"
        echo "|-------|--------|" >> "$GITHUB_STEP_SUMMARY"
        echo "| Code Style | :warning: $ISSUES issue(s) |" >> "$GITHUB_STEP_SUMMARY"
    fi
    exit 1
else
    echo " RESULT: All checks passed"
    echo "=============================================="
    if [ "$IS_GITHUB_CI" = "true" ]; then
        echo "## Code Style Check" >> "$GITHUB_STEP_SUMMARY"
        echo "" >> "$GITHUB_STEP_SUMMARY"
        echo "| Check | Result |" >> "$GITHUB_STEP_SUMMARY"
        echo "|-------|--------|" >> "$GITHUB_STEP_SUMMARY"
        echo "| Code Style | :white_check_mark: PASS |" >> "$GITHUB_STEP_SUMMARY"
    fi
    exit 0
fi