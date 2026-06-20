#!/bin/bash
# audit_self_developed.sh - AuroraOS 自研审计自动化脚本
#
# 扫描项目源代码，检测潜在的第三方代码引用问题。
# 用于 CI/CD 流水线和版本发布前的自动审计。
#
# Usage: bash scripts/audit_self_developed.sh [--strict]

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STRICT_MODE=0
EXIT_CODE=0
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

if [[ "${1:-}" == "--strict" ]]; then
    STRICT_MODE=1
fi

echo "============================================"
echo " AuroraOS 自研审计扫描"
echo " 日期: $(date '+%Y-%m-%d %H:%M:%S')"
echo " 模式: $([ $STRICT_MODE -eq 1 ] && echo '严格' || echo '标准')"
echo "============================================"
echo ""

# ================================================================
# 1. 检测禁止的代码来源短语
# ================================================================
echo "[1/6] 扫描禁止的代码来源短语..."
FORBIDDEN_PATTERNS=(
    "borrowed from"
    "adapted from"
    "ported from"
    "taken from"
    "copied from"
    "stolen from"
    "ripped from"
)

for pattern in "${FORBIDDEN_PATTERNS[@]}"; do
    RESULTS=$(grep -rin --include='*.c' --include='*.h' --include='*.S' "$pattern" "$PROJECT_ROOT" 2>/dev/null || true)
    if [ -n "$RESULTS" ]; then
        echo -e "  ${RED}[FAIL]${NC} 发现禁止短语: '$pattern'"
        echo "$RESULTS" | while read -r line; do
            echo "    $line"
        done
        EXIT_CODE=1
    fi
done

# "derived from" needs special handling: only flag when it refers to code
# derivation, not when it's used in general commentary (e.g. "capabilities
# are derived from open flags").
DERIVED_RESULTS=$(grep -rin --include='*.c' --include='*.h' --include='*.S' \
    -iE 'derived from.*(project|code|source|implementation|library|kernel|OS|operating system)' \
    "$PROJECT_ROOT" 2>/dev/null || true)
if [ -n "$DERIVED_RESULTS" ]; then
    echo -e "  ${RED}[FAIL]${NC} 发现禁止短语: 'derived from' (代码来源上下文)"
    echo "$DERIVED_RESULTS" | while read -r line; do
        echo "    $line"
    done
    EXIT_CODE=1
fi

if [ $EXIT_CODE -eq 0 ]; then
    echo -e "  ${GREEN}[PASS]${NC} 未发现禁止的代码来源短语"
fi

# ================================================================
# 2. 检测外部版权声明
# ================================================================
echo ""
echo "[2/6] 扫描外部版权声明..."
# 查找 Copyright 但排除自身声明
EXT_COPYRIGHT=$(grep -rin --include='*.c' --include='*.h' --include='*.S' \
    -E 'Copyright.*[0-9]{4}.*[^A]' "$PROJECT_ROOT" 2>/dev/null | \
    grep -v 'AuroraOS' | grep -v 'aurora-os' || true)

# 在严格模式下，将非 AuroraOS 的版权声明报告为警告
if [ -n "$EXT_COPYRIGHT" ]; then
    if [ $STRICT_MODE -eq 1 ]; then
        echo -e "  ${RED}[FAIL]${NC} 发现非 AuroraOS 版权声明:"
        echo "$EXT_COPYRIGHT" | while read -r line; do
            echo "    $line"
        done
        EXIT_CODE=1
    else
        echo -e "  ${YELLOW}[WARN]${NC} 发现非 AuroraOS 版权声明 (非严格模式):"
        echo "$EXT_COPYRIGHT" | while read -r line; do
            echo "    $line"
        done
    fi
else
    echo -e "  ${GREEN}[PASS]${NC} 所有版权声明均为 AuroraOS 自身"
fi

# ================================================================
# 3. 检测外部许可证引用
# ================================================================
echo ""
echo "[3/6] 扫描外部许可证引用..."
# 查找 GPL、BSD、Apache 等，但排除自身 MIT 声明和 MIT 工具链引用
EXT_LICENSES=$(grep -rin --include='*.c' --include='*.h' --include='*.S' \
    -E '\bGPL\b|\bLGPL\b|\bBSD[ -]|\bApache\b|\bMPL\b' "$PROJECT_ROOT" 2>/dev/null || true)

if [ -n "$EXT_LICENSES" ]; then
    echo -e "  ${RED}[FAIL]${NC} 发现外部许可证引用:"
    echo "$EXT_LICENSES" | while read -r line; do
        echo "    $line"
    done
    EXIT_CODE=1
else
    echo -e "  ${GREEN}[PASS]${NC} 未发现外部许可证引用"
fi

# ================================================================
# 4. 检测外部仓库 URL 引用
# ================================================================
echo ""
echo "[4/6] 扫描外部仓库引用..."
# 查找 github.com 等，但排除自身仓库
EXT_REPOS=$(grep -rin --include='*.c' --include='*.h' --include='*.S' --include='*.md' \
    -E 'github\.com|gitlab\.com|bitbucket\.org|sourceforge\.net' "$PROJECT_ROOT" 2>/dev/null | \
    grep -v 'zhan1206/aurora-os' | grep -v 'cpos.plos-clan.org' | \
    grep -v 'wiki.osdev.org' | grep -v 'asciinema' | \
    grep -v 'YOUR_USERNAME' | grep -v 'self_development_audit.md' | \
    grep -v 'github.com/zhan1206' || true)

if [ -n "$EXT_REPOS" ]; then
    if [ $STRICT_MODE -eq 1 ]; then
        echo -e "  ${RED}[FAIL]${NC} 发现非白名单外部仓库引用:"
        echo "$EXT_REPOS" | while read -r line; do
            echo "    $line"
        done
        EXIT_CODE=1
    else
        echo -e "  ${YELLOW}[WARN]${NC} 发现非白名单外部仓库引用 (非严格模式):"
        echo "$EXT_REPOS" | while read -r line; do
            echo "    $line"
        done
    fi
else
    echo -e "  ${GREEN}[PASS]${NC} 未发现非白名单外部仓库引用"
fi

# ================================================================
# 5. 检测模糊的归属声明
# ================================================================
echo ""
echo "[5/6] 扫描模糊的归属声明..."
VAGUE_REFERENCES=$(grep -rin --include='*.c' --include='*.h' --include='*.S' \
    -E 'refactored based on|rewritten from|modified from|updated from' "$PROJECT_ROOT" 2>/dev/null || true)

if [ -n "$VAGUE_REFERENCES" ]; then
    echo -e "  ${YELLOW}[WARN]${NC} 发现模糊的归属声明:"
    echo "$VAGUE_REFERENCES" | while read -r line; do
        echo "    $line"
    done
    if [ $STRICT_MODE -eq 1 ]; then
        EXIT_CODE=1
    fi
else
    echo -e "  ${GREEN}[PASS]${NC} 未发现模糊的归属声明"
fi

# ================================================================
# 6. 检测灵感归属的正确标注格式
# ================================================================
echo ""
echo "[6/6] 验证灵感归属标注格式..."
INSPIRED_REFS=$(grep -rin --include='*.c' --include='*.h' --include='*.S' \
    -i 'inspired by' "$PROJECT_ROOT" 2>/dev/null || true)

if [ -n "$INSPIRED_REFS" ]; then
    COUNT=$(echo "$INSPIRED_REFS" | wc -l)
    echo -e "  ${GREEN}[INFO]${NC} 发现 $COUNT 处灵感归属标注 (合法格式)"
    if [ $STRICT_MODE -eq 1 ]; then
        echo "$INSPIRED_REFS" | while read -r line; do
            echo "    $line"
        done
    fi
else
    echo -e "  ${GREEN}[INFO]${NC} 未发现灵感归属标注"
fi

# ================================================================
# 最终报告
# ================================================================
echo ""
echo "============================================"
if [ $EXIT_CODE -eq 0 ]; then
    echo -e " ${GREEN}审计通过: 未发现第三方代码引用${NC}"
else
    echo -e " ${RED}审计失败: 发现 $EXIT_CODE 类问题${NC}"
fi
echo "============================================"

exit $EXIT_CODE