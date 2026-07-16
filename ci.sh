#!/usr/bin/env bash
# CI script for ccode -- runs the full test matrix locally.
# Exit code 0 means all suites passed; nonzero means at least one failed.
set -euo pipefail
cd "$(dirname "$0")"

RESULT=0
echo "=== ccode CI ==="
echo

# ── HTTP-only suite ──
echo "--- HTTP-only test suite ---"
make clean >/dev/null 2>&1 || true
if make HTTP_ONLY=1 test; then
    echo "  PASS: HTTP-only suite"
else
    echo "  FAIL: HTTP-only suite"
    RESULT=1
fi

# ── HTTPS mock suite (when mbedTLS + OpenSSL available) ──
echo
echo "--- HTTPS mock suite ---"
if pkg-config --exists mbedtls && python3 -c "import ssl" 2>/dev/null; then
    make clean >/dev/null 2>&1 || true
    if make && CCODE_TEST_HTTPS=1 bash ./tests/run.sh; then
        echo "  PASS: HTTPS mock suite"
    else
        echo "  FAIL: HTTPS mock suite"
        RESULT=1
    fi
else
    echo "  SKIP: mbedTLS or OpenSSL not available"
fi

# ── ASan/UBSan build (smoke only) ──
echo
echo "--- ASan/UBSan (smoke build) ---"
if make clean >/dev/null 2>&1 && \
   make HTTP_ONLY=1 \
        CFLAGS="-O1 -std=c99 -Wall -Wextra -Wpedantic -m64 -march=x86-64 -mtune=generic -fsanitize=address,undefined -fno-omit-frame-pointer -g" \
        LDFLAGS="-m64 -fsanitize=address,undefined" \
        test-json test-agent 2>/dev/null; then
    echo "  PASS: ASan/UBSan smoke"
else
    echo "  FAIL: ASan/UBSan smoke"
    RESULT=1
fi

# ── Reproducible build (checksum stability) ──
echo
echo "--- Reproducible build ---"
make clean >/dev/null 2>&1 || true
SOURCE_DATE_EPOCH=0 make HTTP_ONLY=1 >/dev/null 2>&1
HASH1=$(sha256sum ccode | cut -d' ' -f1)
make clean >/dev/null 2>&1 || true
SOURCE_DATE_EPOCH=0 make HTTP_ONLY=1 >/dev/null 2>&1
HASH2=$(sha256sum ccode | cut -d' ' -f1)
if [ "$HASH1" = "$HASH2" ] && [ -n "$HASH1" ]; then
    echo "  PASS: reproducible build ($HASH1)"
else
    echo "  FAIL: reproducible build (${HASH1:-none} vs ${HASH2:-none})"
    RESULT=1
fi

echo
if [ "$RESULT" -eq 0 ]; then
    echo "=== CI: all suites passed ==="
else
    echo "=== CI: some suites failed ==="
fi
exit "$RESULT"
