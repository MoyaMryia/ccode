#!/bin/sh
# Build and run the dedicated web_fetch security regressions
# (tests/test_webfetch_security.c). Kept out of the Makefile so it can be run
# on its own; it builds to $TMPDIR so it never races the shared in-tree test
# binaries. Uses only loopback mock servers, never an external target.
set -eu

cd "$(dirname "$0")/.."
CC="${CC:-cc}"
out="${TMPDIR:-/tmp}/test_webfetch_security.$$"
trap 'rm -f "$out"' EXIT

uname_s="$(uname -s 2>/dev/null || echo unknown)"
case "$uname_s" in
    Darwin) plat=src/platform/platform_darwin.c ;;
    FreeBSD|NetBSD|OpenBSD|DragonFly) plat=src/platform/platform_bsd.c ;;
    *) plat=src/platform/platform_linux.c ;;
esac

extra=""
[ "$uname_s" = "Darwin" ] && extra="-D_DARWIN_C_SOURCE"

# shellcheck disable=SC2086
"$CC" -D_POSIX_C_SOURCE=200112L -DCCODE_HTTP_ONLY=1 -Isrc/platform $extra \
    -std=c99 -Wall -Wextra -Wpedantic -Werror \
    tests/test_webfetch_security.c vendor/json/json.c vendor/html/html.c \
    "$plat" -o "$out"
"$out"

# HTTPS->HTTP downgrade end-to-end (needs a TLS build + openssl). The Python
# driver skips itself when either is unavailable.
if command -v python3 >/dev/null 2>&1; then
    python3 tests/test_webfetch_downgrade.py
fi
