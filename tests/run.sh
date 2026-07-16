#!/usr/bin/env bash
set -euo pipefail

PORT=9899
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CCODE="$SCRIPT_DIR/../ccode-cli"
MOCK_PID=""
HTTPS_PID=""
PASS=0
FAIL=0
TIMEOUT=10

cleanup() {
    if [ -n "$MOCK_PID" ]; then
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
    fi
    if [ -n "$HTTPS_PID" ]; then
        kill "$HTTPS_PID" 2>/dev/null || true
        wait "$HTTPS_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

start_mock() {
    python3 "$(dirname "$0")/mock_provider.py" "$PORT" &
    MOCK_PID=$!
    sleep 0.5
    if ! kill -0 "$MOCK_PID" 2>/dev/null; then
        echo "FAIL: mock provider failed to start"
        exit 1
    fi
}

run_test_exit() {
    local name="$1"
    local expected_exit="$2"
    shift 2
    local exit_code=0
    local output
    output=$(timeout "$TIMEOUT" "$CCODE" "$@" 2>&1) || exit_code=$?
    if [ "$exit_code" -eq "$expected_exit" ]; then
        echo "  PASS: $name (exit=$exit_code)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected exit=$expected_exit, got=$exit_code)"
        [ -z "$output" ] || printf '    %s\n' "$output"
        FAIL=$((FAIL + 1))
    fi
}

run_test_exit_clean_env() {
    local name="$1"
    local expected_exit="$2"
    shift 2
    local exit_code=0
    env -u CCODE_API_BASE -u CCODE_API_KEY -u CCODE_MODEL -u CCODE_WRITE_TOOLS \
        timeout "$TIMEOUT" "$CCODE" "$@" >/dev/null 2>&1 || exit_code=$?
    if [ "$exit_code" -eq "$expected_exit" ]; then
        echo "  PASS: $name (exit=$exit_code)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected exit=$expected_exit, got=$exit_code)"
        FAIL=$((FAIL + 1))
    fi
}

if [ "${CCODE_TEST_HTTPS:-0}" = "1" ]; then
    HTTPS_PORT=9900
    python3 "$SCRIPT_DIR/https_mock.py" "$HTTPS_PORT" &
    HTTPS_PID=$!
    sleep 1
    export CCODE_API_BASE="https://localhost:$HTTPS_PORT/v1"
    export CCODE_API_KEY="test-key"
    export CCODE_MODEL="test-model"
    export CCODE_CA_FILE="/tmp/ccode_https_mock/server.pem"

    echo "=== ccode HTTPS Integration Tests ==="
    run_test_exit "TLS response" 0 -p "hello"
    saved_ca_file=$CCODE_CA_FILE
    unset CCODE_CA_FILE
    run_test_exit "untrusted private CA rejected" 1 -p "hello"
    export CCODE_CA_FILE=$saved_ca_file
    started=$SECONDS
    CCODE_REQUEST_TIMEOUT=1 run_test_exit "TLS total read deadline" 1 -p "__ccode_test_tls-delay"
    elapsed=$((SECONDS - started))
    if [ "$elapsed" -le 2 ]; then
        echo "  PASS: TLS deadline elapsed=${elapsed}s"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: TLS deadline elapsed=${elapsed}s"
        FAIL=$((FAIL + 1))
    fi
    echo "=== Results: $PASS passed, $FAIL failed ==="
    exit $FAIL
fi

echo "=== ccode Integration Tests ==="
echo ""

start_mock

export CCODE_API_BASE="http://127.0.0.1:$PORT/v1"
export CCODE_API_KEY="test-key"
export CCODE_MODEL="test-model"

# 1. Normal SSE response
echo "--- Basic connectivity ---"
run_test_exit "normal response" 0 -p "hi"

# 2. Fragmented SSE event
echo "--- Stream fragmentation ---"
run_test_exit \
    "fragmented-sse" 0 \
    -p "__ccode_test_fragmented-sse" \
    --api-base "http://127.0.0.1:$PORT/v1" \
    --api-key "test-key" \
    --model "test-model"

if CCODE_API_BASE="http://127.0.0.1:$PORT/v1" \
   CCODE_API_KEY="test-key" \
   CCODE_MODEL="test-model" \
   timeout 10 "$CCODE" -p "test" > /dev/null 2>&1; then
    echo "  PASS: env-var config"
    PASS=$((PASS + 1))
else
    echo "  FAIL: env-var config"
    FAIL=$((FAIL + 1))
fi

# 3. Non-200 error
echo "--- Error handling ---"
run_test_exit "non-200" 1 -p "__ccode_test_non-200"
output=$(timeout "$TIMEOUT" "$CCODE" -p "__ccode_test_non-200" 2>&1) || true
if echo "$output" | grep -q "Bad request"; then
    echo "  PASS: error.message surfaced"
    PASS=$((PASS + 1))
else
    echo "  FAIL: error.message not surfaced"
    FAIL=$((FAIL + 1))
fi
output=$(timeout "$TIMEOUT" "$CCODE" -p "__ccode_test_non-200-controls" 2>&1) || true
error_line=$(printf '%s\n' "$output" | grep 'Upstream returned a non-200 response' || true)
if [[ "$error_line" == *'Bad\x1B[31m\nmessage\u202E'* ]] &&
   [[ "$error_line" != *$'\033'* ]]; then
    echo "  PASS: provider error terminal controls sanitized"
    PASS=$((PASS + 1))
else
    echo "  FAIL: provider error terminal controls not sanitized"
    [ -z "$output" ] || printf '    %s\n' "$output"
    FAIL=$((FAIL + 1))
fi

# 4. Write tools require an explicit opt-in.
echo "--- Write tools opt-in ---"
output=$(CCODE_WRITE_TOOLS=1 \
         CCODE_API_BASE="http://127.0.0.1:$PORT/v1" \
         CCODE_API_KEY="test-key" \
         CCODE_MODEL="test-model" \
         timeout 10 "$CCODE" -p "test" 2>&1) || true
if echo "$output" | grep -q "You said: test"; then
    echo "  PASS: write tools opt-in accepted"
    PASS=$((PASS + 1))
else
    echo "  FAIL: write tools opt-in rejected"
    FAIL=$((FAIL + 1))
fi

# 5. Missing required args
echo "--- Missing args ---"
run_test_exit_clean_env "no prompt" 2 --api-base "http://x" --api-key "k" --model "m"
run_test_exit_clean_env "no api-base" 2 -p "hi" --api-key "k" --model "m"
run_test_exit_clean_env "no api-key" 2 -p "hi" --api-base "http://x" --model "m"
run_test_exit_clean_env "no model" 2 -p "hi" --api-base "http://x" --api-key "k"

# 6. Help flag
echo "--- Help ---"
run_test_exit "--help" 0 --help

# 7. Chunked transfer encoding with extensions and split SSE events
echo "--- Chunked stream ---"
run_test_exit "chunked" 0 -p "__ccode_test_chunked"
run_test_exit "case-insensitive TE token and chunk trailers" 0 -p "__ccode_test_te-token"

# 8. SSE grammar: optional space after data: and multi-line event data.
echo "--- SSE event framing ---"
run_test_exit "data without space and multi-line event" 0 -p "__ccode_test_sse-format"

# 9. Strict status parsing must not accept a reason phrase containing 200.
echo "--- HTTP parsing ---"
run_test_exit "strict status code" 1 -p "__ccode_test_false-200"
run_test_exit "missing Content-Length rejected" 1 -p "__ccode_test_content-length-missing"
run_test_exit "short Content-Length body rejected" 1 -p "__ccode_test_content-length-short"
run_test_exit "bytes beyond Content-Length rejected" 1 -p "__ccode_test_content-length-extra"
run_test_exit "DONE stops later SSE parsing" 0 -p "__ccode_test_done-terminal"
run_test_exit "trailing JSON root rejected" 1 -p "__ccode_test_trailing-json"

sigpipe_failed=0
for attempt in $(seq 1 20); do
    exit_code=0
    timeout "$TIMEOUT" "$CCODE" -p "sigpipe" \
        --api-base "http://127.0.0.1:$PORT/reset" >/dev/null 2>&1 || exit_code=$?
    if [ "$exit_code" -eq 141 ]; then
        sigpipe_failed=1
        break
    fi
done
if [ "$sigpipe_failed" -eq 0 ]; then
    echo "  PASS: reset during HTTP send does not raise SIGPIPE"
    PASS=$((PASS + 1))
else
    echo "  FAIL: reset during HTTP send raised SIGPIPE"
    FAIL=$((FAIL + 1))
fi

# 10. Header injection and cleartext credential protections.
echo "--- HTTP input validation ---"
run_test_exit "API key CRLF rejected" 1 -p "hi" --api-key $'key\r\nInjected: yes'
run_test_exit "API base CRLF rejected" 1 -p "hi" --api-base $'http://127.0.0.1:9899/v1\r\nInjected: yes'
run_test_exit "HTTP_ONLY remote endpoint rejected" 1 -p "hi" --api-base "http://192.0.2.1/v1"

# 11. Incomplete stream detection
echo "--- Incomplete stream ---"
run_test_exit "incomplete" 1 -p "__ccode_test_incomplete"

# 12. Tool calls must not execute unless an explicit mode is enabled.
echo "--- Tool execution gate ---"
output=$(timeout "$TIMEOUT" "$CCODE" -p "__ccode_test_tool-calls" 2>&1) || exit_code=$?
if echo "$output" | grep -q "tools are not enabled"; then
    echo "  PASS: unenabled tool call denied"
    PASS=$((PASS + 1))
else
    echo "  FAIL: unenabled tool call was not denied"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
