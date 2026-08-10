CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wpedantic
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
override CFLAGS  := -O2 $(filter-out -std=c99 -pedantic -march=x86-64 -mtune=generic -Wextra -Wpedantic,$(CFLAGS))
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

# Per-OS platform implementation. Linux shares /proc + Landlock; Darwin
# uses _NSGetExecutablePath + best-effort; the BSDs use sysctl for the exe
# path. See src/platform/platform.h. HOST_MACH (defined near the top) carries
# the vendor token (e.g. *-apple-darwin*, x86_64-portbld-freebsd14.0,
# x86_64--netbsd); version suffixes mean substring match, not word filter.
PLATFORM_SRC = src/platform/platform_linux.c
ifneq ($(findstring apple,$(HOST_MACH)),)
PLATFORM_SRC = src/platform/platform_darwin.c
endif
ifneq (,$(findstring freebsd,$(HOST_MACH))$(findstring dragonfly,$(HOST_MACH))$(findstring netbsd,$(HOST_MACH))$(findstring openbsd,$(HOST_MACH)))
PLATFORM_SRC = src/platform/platform_bsd.c
endif
AGENT_SRC = src/agent/agent.c src/agent/agent_cancel.c src/agent/agent_fs.c src/agent/agent_args.c src/agent/agent_prepare.c src/agent/agent_exec.c src/agent/agent_output.c src/agent/message.c
SRC = src/main.c src/config.c src/tui/tui.c src/tui/term.c src/tui/render.c src/tui/input.c src/tui/messages.c src/tui/status.c src/tui/theme.c src/tui/protocol.c src/markdown.c $(PLATFORM_SRC)
TEST_JSON_SRC = tests/test_json.c src/json.c vendor/jsmn/jsmn.c $(RETRO_SRC)
TEST_AGENT_SRC = tests/test_agent.c $(AGENT_SRC) src/json.c src/http.c src/webfetch.c src/websearch.c src/sandbox.c src/models.c src/tools/tools.c src/permissions/permissions.c src/markdown.c vendor/jsmn/jsmn.c $(PLATFORM_SRC) $(RETRO_SRC)
TEST_PERMISSIONS_SRC = $(wildcard tests/test_permissions.c)
TEST_TUI_SRC = tests/test_tui.c
TEST_MD_SRC = tests/test_markdown.c src/markdown.c $(RETRO_SRC)
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
MODE_BIN = $(OBJDIR)/ccode
CLI_BIN = $(OBJDIR)/ccode-cli
CLI_SRC = src/cli/main.c src/config.c src/http.c src/json.c src/webfetch.c src/websearch.c src/sandbox.c src/models.c $(AGENT_SRC) src/tools/tools.c src/permissions/permissions.c src/markdown.c vendor/jsmn/jsmn.c $(PLATFORM_SRC)

# Vendored PolarSSL 1.3.9 (retro TLS backend; see
# vendor/polarssl-1.3.9/README.ccode.md). Compiled into the binary via the
# generic rule below; only non-empty in the retro / host-polarssl modes.
POLARSSL_SRC = $(wildcard vendor/polarssl-1.3.9/library/*.c)

ifeq ($(BUILD_MODE),https)
# Vendored mbedTLS library sources, compiled into the binary (no -lmbedtls).
# Compiled with relaxed flags: third-party code, keep it quiet.
MBEDTLS_SRC = $(wildcard $(MBEDTLS_DIR)/library/*.c)
MBEDTLS_OBJ = $(addprefix $(OBJDIR)/,$(MBEDTLS_SRC:.c=.o))
MBEDTLS_CFLAGS = -O2 -std=c99 -w $(X86_GNU_FLAGS)
endif

all: ccode ccode-cli
.PHONY: all

ccode: $(MODE_BIN)
	@tmp=$@.$$$$; cp $< $$tmp && mv -f $$tmp $@

ccode-cli: $(CLI_BIN)
	@tmp=$@.$$$$; cp $< $$tmp && mv -f $$tmp $@

$(CLI_BIN): $(CLI_SRC) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(CLI_SRC) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ) $(LDFLAGS) $(LDLIBS)

$(MODE_BIN): $(OBJ) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(RETRO_COMPAT_OBJ) $(MBEDTLS_OBJ) $(POLARSSL_OBJ) $(LDFLAGS) $(LDLIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

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

tests/test_agent: override CPPFLAGS += -DCCODE_UNIT_TEST=1 -DCCODE_HTTP_ONLY=1
tests/test_agent: $(TEST_AGENT_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_AGENT_SRC)

ifneq ($(TEST_PERMISSIONS_SRC),)
test-permissions: tests/test_permissions
	./tests/test_permissions

tests/test_permissions: tests/test_permissions.c src/permissions/permissions.c $(RETRO_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^
endif

test-http: ccode-cli
	bash ./tests/run.sh

test-tui: tests/test_tui
	./tests/test_tui

tests/test_tui: $(TEST_TUI_SRC) src/tui/input.c src/tui/messages.c src/tui/render.c src/tui/protocol.c src/markdown.c $(RETRO_SRC)
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

install: ccode ccode-cli
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 0755 ccode $(DESTDIR)$(BINDIR)/ccode
	install -m 0755 ccode-cli $(DESTDIR)$(BINDIR)/ccode-cli
	install -m 0644 docs/man/ccode.1 $(DESTDIR)$(MANDIR)/ccode.1
	install -m 0644 docs/man/ccode-cli.1 $(DESTDIR)$(MANDIR)/ccode-cli.1

.PHONY: install

clean:
	rm -rf .build
	rm -f ccode ccode-cli tests/test_json tests/test_agent tests/test_permissions tests/test_tui tests/test_markdown
	rm -rf test-sandbox

# ASan + UBSan build (for debugging/fuzzing). Override CFLAGS to remove
# -O2 (ASan works best with -O0 or -O1) and inject the sanitizer flags.
# X86_GNU_FLAGS is empty on non-Linux-x86 hosts so the build stays portable.
asan: clean
	@$(MAKE) HTTP_ONLY=1 CFLAGS="-O1 -std=c99 -Wall -Wextra -Wpedantic $(X86_GNU_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g" LDFLAGS="$(if $(X86_GNU_FLAGS),-m64) -fsanitize=address,undefined"
	@echo "ASan/UBSan binary ready. Run individual test targets to exercise."

# Reproducible build: honour SOURCE_DATE_EPOCH and strip unstable paths.
repro: clean
	SOURCE_DATE_EPOCH=0 $(MAKE) HTTP_ONLY=1 CFLAGS="-O2 -std=c99 -Wall -Wextra -Wpedantic $(X86_GNU_FLAGS) -ffile-prefix-map=$(PWD)=."

.PHONY: ccode ccode-cli clean test test-json test-agent test-http test-permissions test-tui test-markdown test-tty test-e2e test-streaming retro-test asan repro test-sandbox
