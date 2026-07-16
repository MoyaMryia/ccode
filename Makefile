CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS ?=
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wpedantic
LDFLAGS ?=
override CPPFLAGS += -D_POSIX_C_SOURCE=200112L
override CFLAGS += -m64 -march=x86-64 -mtune=generic
override LDFLAGS += -m64

SRC = src/main.c src/config.c src/tui/tui.c src/tui/term.c src/tui/render.c src/tui/input.c src/tui/messages.c src/tui/status.c src/tui/theme.c src/tui/protocol.c src/markdown.c
TEST_JSON_SRC = tests/test_json.c src/json.c vendor/jsmn/jsmn.c
TEST_AGENT_SRC = tests/test_agent.c src/agent/agent.c src/agent/message.c src/json.c src/http.c src/webfetch.c src/models.c src/tools/tools.c src/permissions/permissions.c src/markdown.c vendor/jsmn/jsmn.c
TEST_PERMISSIONS_SRC = $(wildcard tests/test_permissions.c)
TEST_TUI_SRC = tests/test_tui.c
TEST_MD_SRC = tests/test_markdown.c src/markdown.c
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
BUILD_MODE = https
MBEDTLS_CFLAGS := $(shell $(PKG_CONFIG) --cflags mbedtls 2>/dev/null)
MBEDTLS_LIBS := $(shell $(PKG_CONFIG) --libs mbedtls 2>/dev/null)
override CPPFLAGS += $(MBEDTLS_CFLAGS)
LDLIBS += $(MBEDTLS_LIBS) -lmbedx509 -lmbedcrypto
endif

ifneq ($(TEST_PERMISSIONS_SRC),)
TEST_TARGETS += test-permissions
endif

OBJDIR = .build/$(BUILD_MODE)
OBJ = $(addprefix $(OBJDIR)/,$(SRC:.c=.o))
MODE_BIN = $(OBJDIR)/ccode
CLI_BIN = $(OBJDIR)/ccode-cli
CLI_SRC = src/cli/main.c src/config.c src/http.c src/json.c src/webfetch.c src/models.c src/agent/message.c src/agent/agent.c src/tools/tools.c src/permissions/permissions.c src/markdown.c vendor/jsmn/jsmn.c

ccode: $(MODE_BIN)
	@tmp=$@.$$$$; cp $< $$tmp && mv -f $$tmp $@

ccode-cli: $(CLI_BIN)
	@tmp=$@.$$$$; cp $< $$tmp && mv -f $$tmp $@

$(CLI_BIN): $(CLI_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(CLI_SRC) $(LDFLAGS) $(LDLIBS)

all: ccode ccode-cli

$(MODE_BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

ifeq ($(BUILD_MODE),https)
$(OBJ): | check-mbedtls
endif

check-mbedtls:
	@$(PKG_CONFIG) --exists mbedtls || { echo "mbedTLS development files are required. Install libmbedtls-dev or build with HTTP_ONLY=1 for local testing only." >&2; exit 1; }

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

tests/test_permissions: tests/test_permissions.c src/permissions/permissions.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^
endif

test-http: ccode-cli
	bash ./tests/run.sh

test-tui: tests/test_tui
	./tests/test_tui

tests/test_tui: $(TEST_TUI_SRC) src/tui/input.c src/tui/messages.c src/tui/render.c src/tui/protocol.c src/markdown.c
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

test: $(TEST_TARGETS)

clean:
	rm -rf .build
	rm -f ccode ccode-cli tests/test_json tests/test_agent tests/test_permissions tests/test_tui tests/test_markdown
	rm -rf test-sandbox

# ASan + UBSan build (for debugging/fuzzing). Override CFLAGS to remove
# -O2 (ASan works best with -O0 or -O1) and inject the sanitizer flags.
asan: clean
	@$(MAKE) HTTP_ONLY=1 CFLAGS="-O1 -std=c99 -Wall -Wextra -Wpedantic -m64 -march=x86-64 -mtune=generic -fsanitize=address,undefined -fno-omit-frame-pointer -g" LDFLAGS="-m64 -fsanitize=address,undefined"
	@echo "ASan/UBSan binary ready. Run individual test targets to exercise."

# Reproducible build: honour SOURCE_DATE_EPOCH and strip unstable paths.
repro: clean
	SOURCE_DATE_EPOCH=0 $(MAKE) HTTP_ONLY=1 CFLAGS="-O2 -std=c99 -Wall -Wextra -Wpedantic -m64 -march=x86-64 -mtune=generic -ffile-prefix-map=$(PWD)=."

.PHONY: ccode ccode-cli check-mbedtls clean test test-json test-agent test-http test-permissions test-tui test-markdown test-tty test-e2e test-streaming asan repro test-sandbox
