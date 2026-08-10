#!/usr/bin/env bash
# CI script for ccode -- runs the full test matrix locally.
# Exit code 0 means all suites passed; nonzero means at least one failed.
set -euo pipefail
cd "$(dirname "$0")"

# sha256sum on GNU; shasum -a 256 on macOS / BSD. Probe once.
if command -v sha256sum >/dev/null 2>&1; then
    SHA256="sha256sum"
else
    SHA256="shasum -a 256"
fi

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

# ── HTTPS mock suite (mbedTLS is vendored; needs Python ssl) ──
echo
echo "--- HTTPS mock suite ---"
if python3 -c "import ssl" 2>/dev/null; then
    make clean >/dev/null 2>&1 || true
    if make && CCODE_TEST_HTTPS=1 bash ./tests/run.sh; then
        echo "  PASS: HTTPS mock suite"
    else
        echo "  FAIL: HTTPS mock suite"
        RESULT=1
    fi
else
    echo "  SKIP: Python ssl not available"
fi

# ── ASan/UBSan build (smoke only) ──
echo
echo "--- ASan/UBSan (smoke build) ---"
# GNU x86 flags only apply on Linux-x86; Apple clang rejects -march=x86-64.
X86_FLAGS=""
case "$(uname -s 2>/dev/null)" in
    Linux)
        case "$(uname -m 2>/dev/null)" in
            x86_64) X86_FLAGS="-m64 -march=x86-64 -mtune=generic" ;;
        esac
        ;;
esac
if make clean >/dev/null 2>&1 && \
   make HTTP_ONLY=1 \
        CFLAGS="-O1 -std=c99 -Wall -Wextra -Wpedantic $X86_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g" \
        LDFLAGS="${X86_FLAGS:+-m64 }-fsanitize=address,undefined" \
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
HASH1=$("$SHA256" ccode | cut -d' ' -f1)
make clean >/dev/null 2>&1 || true
SOURCE_DATE_EPOCH=0 make HTTP_ONLY=1 >/dev/null 2>&1
HASH2=$("$SHA256" ccode | cut -d' ' -f1)
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
