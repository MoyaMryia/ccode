CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -Os -std=c99 -Wall -Wextra -Wpedantic
LDFLAGS ?=
override CPPFLAGS += -D_POSIX_C_SOURCE=200112L

# Host triplet from the toolchain (e.g. x86_64-linux-gnu, x86_64-apple-darwin23,
# aarch64-apple-darwin23, x86_64-portbld-freebsd14, riscv64-linux-gnu). Used to
# pick the platform implementation and gate x86/Apple-specific flags below.
HOST_MACH := $(shell $(CC) -dumpmachine 2>/dev/null)

# Darwin: _POSIX_C_SOURCE alone hides BSD/POSIX.1-2008 extensions that the
# sources use (strcasestr, memmem, fdopendir). _GNU_SOURCE (a glibc-only
# macro) does not rescue them on Darwin, so define _DARWIN_C_SOURCE which
# re-enables the full Darwin namespace. Harmless on other platforms.
ifneq ($(findstring apple,$(HOST_MACH)),)
override CPPFLAGS += -D_DARWIN_C_SOURCE
endif

# ── Retro i386 / BasicLinux 3.5.1 (libc5 / kernel 2.2.26) build mode ──
# Activated by RETRO=1. Targets i586 and force-includes src/compat/compat.h
# to shim openat, fstatat, O_CLOEXEC, getaddrinfo, clock_gettime and stdint.h.
# TLS: mbedTLS 2.28 cannot build on gcc 2.7/egcs 1.1.2, so the retro build
# links the vendored PolarSSL 1.3.9 backend (CCODE_TLS_POLARSSL) instead of
# forcing HTTP_ONLY; see src/tls_backend.h.
# Old gcc (2.7.2.3 / egcs 1.1.2) lacks -Wextra/-Wpedantic; -pedantic would
# choke on the GNU-extension `long long`, so it is filtered too.
# See docs/BASICLINUX.md for the full target matrix and QEMU verification.
ifeq ($(RETRO),1)
override CPPFLAGS += -include src/compat/compat.h
# compat/ holds shim headers (poll.h, stdint.h) for libc5.
override CPPFLAGS += -Isrc/compat
# gcc 2.7.2.3 / egcs 1.1.2 predate -std=c9x and -Wextra/-Wpedantic. The
# sources are C89 (mid-block declarations were hoisted by scripts/c89ify.py;
# `long long` remains as a GNU extension), so only the flags need dropping:
# -std=c99 (unsupported by gcc 2.7) and -pedantic (chokes on `long long`).
override CFLAGS  := -O2 $(filter-out -std=c99 -pedantic -march=x86-64 -mtune=generic -Wextra -Wpedantic -Os,$(CFLAGS))
# -m32/-march/-mtune are gcc 3.x+ multilib flags. A native i386 toolchain
# (gcc 2.7.2.3 on BasicLinux) is already 32-bit and uses -m486/-mcpu=
# instead, so set RETRO_NATIVE=1 there to skip them. The host smoke-test
# build leaves RETRO_NATIVE unset and keeps the flags.
ifneq ($(RETRO_NATIVE),1)
override CFLAGS  += -m32 -march=i586 -mtune=i586
# Host multilib (gcc -m32 on x86-64) needs the 32-bit-capable include dir
# on PATH before /usr/include. Harmless on a native i386 toolchain.
override CFLAGS  += -isystem /usr/include/x86_64-linux-gnu
override LDFLAGS := -m32 $(filter-out -m64,$(LDFLAGS))
else
override LDFLAGS := $(filter-out -m64 -m32,$(LDFLAGS))
endif
else
# x86-specific flags: only when targeting x86_64 with a GNU/Linux toolchain.
# -m64/-march=x86-64/-mtune=generic pin the Linux x86-64-v1 baseline; they are
# meaningless or rejected elsewhere:
#   - Apple clang (Intel or Apple Silicon arm64) rejects -march=x86-64 and
#     errors on -m64 under arm64; the arch is default anyway.
#   - BSD / Cygwin use their own baselines.
#   - Cross builds (CC=riscv64-linux-gnu-gcc) must not inherit them.
# X86_GNU_FLAGS is reused by the mbedTLS rule and the asan/repro convenience
# targets so they stay in sync (empty on non-Linux-x86 hosts).
X86_GNU_FLAGS =
ifneq ($(findstring x86_64,$(HOST_MACH)),)
ifneq ($(findstring linux,$(HOST_MACH)),)
X86_GNU_FLAGS = -m64 -march=x86-64 -mtune=generic
override CFLAGS += -m64 -march=x86-64 -mtune=generic
override LDFLAGS += -m64
endif
endif
endif

# Per-OS platform implementation. Linux uses /proc + Landlock; Darwin uses
# _NSGetExecutablePath + best-effort; the BSDs use sysctl for the exe path;
# Haiku/Hurd/Solaris/MINIX/Win32(Cygwin) have dedicated files (see below).
# See src/platform/platform.h. HOST_MACH (defined near the top) carries the
# vendor token (e.g. *-apple-darwin*, x86_64-portbld-freebsd14.0,
# x86_64--netbsd, x86_64-unknown-haiku, i686-gnu, x86_64-pc-solaris2.11,
# i686-minix, x86_64-pc-cygwin); version suffixes mean substring match, not
# word filter.
PLATFORM_SRC = src/platform/platform_linux.c
ifneq ($(findstring apple,$(HOST_MACH)),)
PLATFORM_SRC = src/platform/platform_darwin.c
endif
ifneq (,$(findstring freebsd,$(HOST_MACH))$(findstring dragonfly,$(HOST_MACH))$(findstring netbsd,$(HOST_MACH))$(findstring openbsd,$(HOST_MACH)))
PLATFORM_SRC = src/platform/platform_bsd.c
endif
# Haiku (BeOS descendant): find_path(B_APP_IMAGE_SYMBOL) exe resolution.
ifneq ($(findstring haiku,$(HOST_MACH)),)
PLATFORM_SRC = src/platform/platform_haiku.c
endif
# GNU Hurd: /proc/self/exe via procfs translator + MSG_NOSIGNAL (glibc).
# The canonical Hurd triplet is *-gnu (e.g. i686-gnu, x86_64-gnu) with no
# "linux" or "mingw" token; exclude *-linux-gnu (glibc Linux) and
# *-w64-mingw32-gnu (MinGW), which also carry the gnu substring.
ifeq ($(findstring linux,$(HOST_MACH)),)
ifeq ($(findstring mingw,$(HOST_MACH)),)
ifneq (,$(filter %-gnu,$(HOST_MACH)))
PLATFORM_SRC = src/platform/platform_hurd.c
endif
endif
endif
# illumos / Solaris / OpenSolaris: getexecname() + /proc/self/path/a.out.
ifneq (,$(findstring solaris,$(HOST_MACH))$(findstring illumos,$(HOST_MACH)))
PLATFORM_SRC = src/platform/platform_solaris.c
endif
# MINIX 3: NetBSD libc sysctl exe path + SO_NOSIGPIPE.
ifneq ($(findstring minix,$(HOST_MACH)),)
PLATFORM_SRC = src/platform/platform_minix.c
endif
# Windows via Cygwin / MSYS2: /proc/self/exe + MSG_NOSIGNAL (Cygwin).
ifneq (,$(findstring cygwin,$(HOST_MACH))$(findstring msys,$(HOST_MACH)))
PLATFORM_SRC = src/platform/platform_win32.c
endif
AGENT_SRC = src/agent/agent.c src/agent/agent_cancel.c src/agent/agent_fs.c src/agent/agent_args.c src/agent/agent_prepare.c src/agent/agent_exec.c src/agent/agent_output.c src/agent/message.c
SRC = src/main.c src/config.c src/tui/tui.c src/tui/term.c src/tui/render.c src/tui/input.c src/tui/messages.c src/tui/status.c src/tui/theme.c src/tui/protocol.c src/markdown.c src/json.c vendor/jsmn/jsmn.c $(PLATFORM_SRC)
TEST_JSON_SRC = tests/test_json.c src/json.c vendor/jsmn/jsmn.c $(RETRO_SRC)
TEST_AGENT_SRC = tests/test_agent.c $(AGENT_SRC) src/json.c src/http.c src/webfetch.c src/websearch.c src/sandbox.c src/models.c src/tools/tools.c src/permissions/permissions.c src/markdown.c vendor/jsmn/jsmn.c $(PLATFORM_SRC) $(RETRO_SRC)
TEST_PERMISSIONS_SRC = $(wildcard tests/test_permissions.c)
TEST_TUI_SRC = tests/test_tui.c
TEST_MD_SRC = tests/test_markdown.c src/markdown.c src/json.c vendor/jsmn/jsmn.c $(RETRO_SRC)
TTY_TEST := $(shell python3 -c "import pty" 2>/dev/null && echo 1)
TEST_TARGETS = test-json test-agent test-http
TEST_TARGETS += test-tui
TEST_TARGETS += test-markdown
ifneq ($(TTY_TEST),)
TEST_TARGETS += test-tty
TEST_TARGETS += test-e2e
TEST_TARGETS += test-streaming
endif

ifeq ($(HTTP_ONLY),1)
BUILD_MODE = http
override CPPFLAGS += -DCCODE_HTTP_ONLY=1
else
ifeq ($(TLS),polarssl)
# Host-side PolarSSL build: fast verification of the retro TLS backend
# against the Python ssl mock. No compat shims needed on glibc.
BUILD_MODE = polarssl
override CPPFLAGS += -DCCODE_TLS_BACKEND=2 -Ivendor/polarssl-1.3.9/include
POLARSSL_OBJ = $(addprefix $(OBJDIR)/,$(POLARSSL_SRC:.c=.o))
else
BUILD_MODE = https
# mbedTLS 2.28.9 (LTS) vendored in vendor/mbedtls: no system libmbedtls needed.
MBEDTLS_DIR = vendor/mbedtls
override CPPFLAGS += -I$(MBEDTLS_DIR)/include
endif
endif

# RETRO=1 wins over HTTP_ONLY/mbedTLS: force the libc5 / 2.2.26 build.
ifeq ($(RETRO),1)
BUILD_MODE = retro
# NB: do NOT define CCODE_HTTP_ONLY here - it would win the CCODE_TLS_BACKEND
# derivation in src/tls_backend.h (http-only instead of the PolarSSL backend).
override CPPFLAGS += -DCCODE_RETRO=1
# Vendored PolarSSL 1.3.9 provides the TLS backend on the retro toolchain
# (gcc 2.7/egcs 1.1.2 cannot build mbedTLS 2.28); compat shims give it
# stdint.h/inttypes.h via -Isrc/compat. CCODE_TLS_BACKEND is derived in
# src/tls_backend.h from CCODE_HTTP_ONLY/CCODE_RETRO.
override CPPFLAGS += -Ivendor/polarssl-1.3.9/include
POLARSSL_OBJ = $(addprefix $(OBJDIR)/,$(POLARSSL_SRC:.c=.o))
# On a glibc host, enable the shim so the retro path compiles and links
# against a modern libc for smoke-testing. A native libc5 toolchain has
# no __GLIBC__ and picks the shim up automatically, so don't define this
# there (it would suppress the libc5-only stdint/socklen typedefs).
ifneq ($(RETRO_NATIVE),1)
override CPPFLAGS += -DCCODE_RETRO_HOST_TEST=1
endif
RETRO_COMPAT_OBJ = $(OBJDIR)/src/compat/compat.o
RETRO_SRC = src/compat/compat.c
CLI_SRC += src/compat/compat.c
else
RETRO_SRC =
endif

ifneq ($(TEST_PERMISSIONS_SRC),)
TEST_TARGETS += test-permissions
endif

OBJDIR = .build/$(BUILD_MODE)
OBJ = $(addprefix $(OBJDIR)/,$(SRC:.c=.o))
TUI_BIN = $(OBJDIR)/ccode-tui
CLI_BIN = $(OBJDIR)/ccode-cli
COMBINED_BIN = $(OBJDIR)/ccode
CLI_SRC = src/cli/main.c src/config.c src/http.c src/json.c src/webfetch.c src/websearch.c src/sandbox.c src/models.c $(AGENT_SRC) src/tools/tools.c src/permissions/permissions.c src/markdown.c vendor/jsmn/jsmn.c $(PLATFORM_SRC)
# 单体 ccode：进程内 TUI + CLI 合一个二进制。$(sort) 去掉 SRC/CLI_SRC
# 里重叠的 config.c/markdown.c/platform；main.c 与 cli/main.c 的 main() 在
# -DCCODE_COMBINED 下被排除，由 combined_main.c 提供分发入口。
COMBINED_SRC = $(sort src/combined_main.c src/main.c src/cli/main.c $(SRC) $(CLI_SRC))

# ── Binary size optimization ──
# Split every function/data into its own section and let the linker drop
# whatever is unreferenced. The vendored mbedTLS/PolarSSL compile all of
# their library/*.c but only a small TLS-client subset is used, so this is
# the single largest size win. -s strips the symbol table at link time.
#
# Per-linker dead-code elimination and stripping:
#   - GNU ld (Linux):  -Wl,--gc-sections + -s.
#   - Apple ld (Darwin): no --gc-sections; -Wl,-dead_strip is the direct
#     equivalent. Do NOT pass -s to Apple ld: Xcode 15's linker declared it
#     obsolete, and current toolchains reject it (ld error). Strip post-link
#     with strip(1) instead (POST_LINK_STRIP).
#   - Other linkers (BSD/Haiku/...): -s only.
#
# Retro also shrinks, but conservatively:
#   - host smoke build (RETRO=1, modern gcc -m32): full set.
#   - guest native (RETRO_NATIVE=1, egcs 1.1.2 / gcc 2.7.2.3): only -s.
#     Old gcc lacks -fdata-sections, and --gc-sections over a libc5 static
#     link with old binutils is not worth risking.
POST_LINK_STRIP = true
ifeq ($(RETRO),1)
ifneq ($(RETRO_NATIVE),1)
SIZE_CFLAGS = -ffunction-sections -fdata-sections
SIZE_LDFLAGS = -Wl,--gc-sections -s
else
SIZE_CFLAGS =
SIZE_LDFLAGS = -s
endif
else
SIZE_CFLAGS = -ffunction-sections -fdata-sections
ifneq ($(findstring apple,$(HOST_MACH)),)
SIZE_LDFLAGS = -Wl,-dead_strip
POST_LINK_STRIP = strip
else
ifeq ($(findstring linux,$(HOST_MACH)),)
SIZE_LDFLAGS = -s
else
SIZE_LDFLAGS = -Wl,--gc-sections -s
endif
endif
endif

# Vendored PolarSSL 1.3.9 (retro TLS backend; see
# vendor/polarssl-1.3.9/README.ccode.md). Compiled into the binary via the
# generic rule below; only non-empty in the retro / host-polarssl modes.
POLARSSL_SRC = $(wildcard vendor/polarssl-1.3.9/library/*.c)

ifeq ($(BUILD_MODE),https)
# Vendored mbedTLS library sources, compiled into the binary (no -lmbedtls).
# Compiled with relaxed flags: third-party code, keep it quiet. Function/data
# sections let --gc-sections drop the unused algorithms at link time.
MBEDTLS_SRC = $(wildcard $(MBEDTLS_DIR)/library/*.c)
MBEDTLS_OBJ = $(addprefix $(OBJDIR)/,$(MBEDTLS_SRC:.c=.o))
MBEDTLS_CFLAGS = -Os -std=c99 -w $(SIZE_CFLAGS) $(X86_GNU_FLAGS)
endif

all: ccode ccode-cli ccode-tui
.PHONY: all

# 单体二进制：自带 TUI + CLI（进程内运行，不 fork 后端）。
ccode: $(COMBINED_BIN)
	@tmp=$@.$$$$; cp $< $$tmp && mv -f $$tmp $@

# 分离的 TUI 前端（fork ccode-cli 当后端，走 JSON Lines 协议）。
ccode-tui: $(TUI_BIN)
	@tmp=$@.$$$$; cp $< $$tmp && mv -f $$tmp $@

ccode-cli: $(CLI_BIN)
	@tmp=$@.$$$$; cp $< $$tmp && mv -f $$tmp $@

$(CLI_BIN): $(CLI_SRC) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SIZE_CFLAGS) -o $@ $(CLI_SRC) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ) $(SIZE_LDFLAGS) $(LDFLAGS) $(LDLIBS)
	@$(POST_LINK_STRIP) $@

$(COMBINED_BIN): $(COMBINED_SRC) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SIZE_CFLAGS) -DCCODE_COMBINED=1 -o $@ $(COMBINED_SRC) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ) $(SIZE_LDFLAGS) $(LDFLAGS) $(LDLIBS)
	@$(POST_LINK_STRIP) $@

$(TUI_BIN): $(OBJ) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ) $(SIZE_LDFLAGS) $(LDFLAGS) $(LDLIBS)
	@$(POST_LINK_STRIP) $@

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SIZE_CFLAGS) -c -o $@ $<

ifeq ($(BUILD_MODE),https)
$(OBJDIR)/$(MBEDTLS_DIR)/%.o: $(MBEDTLS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(MBEDTLS_CFLAGS) -c -o $@ $<
endif

# 自动复制 HTTPS 版本到 test-sandbox
test-sandbox: ccode
	@mkdir -p test-sandbox
	@cp ccode test-sandbox/ccode
	@echo "HTTPS binary copied to test-sandbox/ccode"

test-json: tests/test_json
	./tests/test_json

tests/test_json: $(TEST_JSON_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_JSON_SRC)

test-agent: tests/test_agent
	./tests/test_agent

# RETRO: tls_backend.h lets CCODE_RETRO win the backend derivation, so the
# agent unit tests build against the PolarSSL (HTTPS) backend and must link
# the vendored PolarSSL objects; other modes keep the tests HTTP-only
# (CCODE_HTTP_ONLY -> CCODE_TLS_NONE) so no TLS objects are needed.
ifeq ($(RETRO),1)
tests/test_agent: override CPPFLAGS += -DCCODE_UNIT_TEST=1
tests/test_agent: $(TEST_AGENT_SRC) $(POLARSSL_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_AGENT_SRC) $(POLARSSL_OBJ)
else
tests/test_agent: override CPPFLAGS += -DCCODE_UNIT_TEST=1 -DCCODE_HTTP_ONLY=1
tests/test_agent: $(TEST_AGENT_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_AGENT_SRC)
endif

ifneq ($(TEST_PERMISSIONS_SRC),)
test-permissions: tests/test_permissions
	./tests/test_permissions

tests/test_permissions: tests/test_permissions.c src/permissions/permissions.c src/json.c vendor/jsmn/jsmn.c $(RETRO_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^
endif

test-http: ccode-cli
	bash ./tests/run.sh

test-tui: tests/test_tui
	./tests/test_tui

tests/test_tui: $(TEST_TUI_SRC) src/tui/input.c src/tui/messages.c src/tui/render.c src/tui/protocol.c src/markdown.c src/json.c vendor/jsmn/jsmn.c $(RETRO_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test-markdown: tests/test_markdown
	./tests/test_markdown

tests/test_markdown: $(TEST_MD_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test-tty: ccode-cli
	python3 ./tests/test_tty_agent.py

test-e2e: ccode-cli
	python3 ./tests/test_e2e_fixture.py

test-streaming: ccode-cli
	python3 ./tests/test_streaming.py

# ── Retro guest smoke (QEMU/BasicLinux): build ccode inside the guest ──
# One-shot full pipeline: tar sources -> inject into vm/bl3-disk.img ->
# boot QEMU (gtk window) -> make RETRO=1 RETRO_NATIVE=1 -> sync ->
# extract logs to logs/guest/ and judge from those files.
# Requires vm/bl3-disk.img (bash scripts/make_ghost_disk.sh) and
# qemu-system-i386. Slow (~15 min) and mutates the disk, so NOT part of
# the default `test` target. Fixed defaults only: smoke means smoke.
retro-test:
	python3 ./scripts/guest_build.py

test: $(TEST_TARGETS)

# Install binaries and man pages under $(DESTDIR)/$(PREFIX).
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

install: ccode ccode-cli ccode-tui
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 0755 ccode $(DESTDIR)$(BINDIR)/ccode
	install -m 0755 ccode-tui $(DESTDIR)$(BINDIR)/ccode-tui
	install -m 0755 ccode-cli $(DESTDIR)$(BINDIR)/ccode-cli
	install -m 0644 docs/man/ccode.1 $(DESTDIR)$(MANDIR)/ccode.1
	install -m 0644 docs/man/ccode-cli.1 $(DESTDIR)$(MANDIR)/ccode-cli.1

.PHONY: install

clean:
	rm -rf .build
	rm -f ccode ccode-tui ccode-cli tests/test_json tests/test_agent tests/test_permissions tests/test_tui tests/test_markdown
	rm -rf test-sandbox

# ASan + UBSan build (for debugging/fuzzing). Override CFLAGS to remove
# -O2 (ASan works best with -O0 or -O1) and inject the sanitizer flags.
# X86_GNU_FLAGS is empty on non-Linux-x86 hosts so the build stays portable.
asan: clean
	@$(MAKE) HTTP_ONLY=1 SIZE_CFLAGS= SIZE_LDFLAGS= CFLAGS="-O1 -std=c99 -Wall -Wextra -Wpedantic $(X86_GNU_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g" LDFLAGS="$(if $(X86_GNU_FLAGS),-m64) -fsanitize=address,undefined"
	@echo "ASan/UBSan binary ready. Run individual test targets to exercise."

# Reproducible build: honour SOURCE_DATE_EPOCH and strip unstable paths.
repro: clean
	SOURCE_DATE_EPOCH=0 $(MAKE) HTTP_ONLY=1 SIZE_CFLAGS= SIZE_LDFLAGS= CFLAGS="-O2 -std=c99 -Wall -Wextra -Wpedantic $(X86_GNU_FLAGS) -ffile-prefix-map=$(PWD)=."

.PHONY: ccode ccode-tui ccode-cli clean test test-json test-agent test-http test-permissions test-tui test-markdown test-tty test-e2e test-streaming retro-test asan repro test-sandbox
