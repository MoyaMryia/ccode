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

# Session directory: --session-dir must be honoured and created recursively.
echo "--- Session directory ---"
sdir_root="/tmp/ccode_sdir_test_$$"
sdir="$sdir_root/nested/sessions"
rm -rf "$sdir_root"
printf '/session new smoke.json\n/exit\n' | timeout "$TIMEOUT" \
    "$CCODE" --interactive --session-dir "$sdir" >/dev/null 2>&1 || true
if [ -f "$sdir/smoke.json" ]; then
    echo "  PASS: --session-dir created recursively and used"
    PASS=$((PASS + 1))
else
    echo "  FAIL: --session-dir not honoured/created"
    FAIL=$((FAIL + 1))
fi
rm -rf "$sdir_root"

# /exit must leave a resumable auto-named session for interactive runs that
# never named one (regression: only named sessions used to be saved).
echo "--- /exit auto-saves a resumable session ---"
adir="/tmp/ccode_auto_session_$$"
rm -rf "$adir"
printf 'Hello auto session\n/exit\n' | timeout "$TIMEOUT" \
    "$CCODE" --interactive --session-dir "$adir" >/dev/null 2>&1 || true
auto_session=$(ls -t "$adir"/auto-*.json 2>/dev/null | head -1 || true)
if [ -n "$auto_session" ] && [ -s "$auto_session" ]; then
    echo "  PASS: /exit saved auto session"
    PASS=$((PASS + 1))
else
    echo "  FAIL: /exit did not save an auto session"
    FAIL=$((FAIL + 1))
fi
rm -rf "$adir"

# Resuming must print the loaded transcript and /exit must write back to the
# same session file (no stray new auto-* chain).
echo "--- resume prints transcript and saves back in place ---"
rdir="/tmp/ccode_resume_test_$$"
rm -rf "$rdir"
printf 'Hello resume probe\n/exit\n' | timeout "$TIMEOUT" \
    "$CCODE" --interactive --session-dir "$rdir" >/dev/null 2>&1 || true
rfile=$(ls -1 "$rdir"/auto-*.json 2>/dev/null | head -1 || true)
if [ -n "$rfile" ]; then
    rout=$(printf '/exit\n' | timeout "$TIMEOUT" \
        "$CCODE" --interactive --session-dir "$rdir" --resume "$rfile" \
        2>&1) || true
    if echo "$rout" | grep -q "session transcript" &&
       echo "$rout" | grep -q "Hello resume probe"; then
        echo "  PASS: resumed session printed its transcript"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: resumed session did not print transcript"
        FAIL=$((FAIL + 1))
    fi
    rcount=$(ls -1 "$rdir"/auto-*.json 2>/dev/null | wc -l)
    if [ "$rcount" -eq 1 ]; then
        echo "  PASS: /exit saved back to the resumed session"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: /exit created a stray session (count=$rcount)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: could not create a session to resume"
    FAIL=$((FAIL + 1))
fi
rm -rf "$rdir"

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
error_line=$(printf '%s\n' "$output" | grep 'Upstream returned HTTP 400' || true)
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

# 12. Default mode is read-only: read tools execute, write tools are denied
# unless --write (or CCODE_WRITE_TOOLS=1) is given.
echo "--- Tool execution gate ---"
output=$(timeout "$TIMEOUT" "$CCODE" -p "__ccode_test_tool-calls" 2>&1) || true
if echo "$output" | grep -q "Tool result received."; then
    echo "  PASS: default read-only mode executes read tools"
    PASS=$((PASS + 1))
else
    echo "  FAIL: default read-only mode did not execute read tools"
    [ -z "$output" ] || printf '    %s\n' "$output"
    FAIL=$((FAIL + 1))
fi
output=$(timeout "$TIMEOUT" "$CCODE" -p "__ccode_test_tool-calls-write" 2>&1) || true
if echo "$output" | grep -q "Tool is unavailable"; then
    echo "  PASS: default read-only mode denies write tools"
    PASS=$((PASS + 1))
else
    echo "  FAIL: default read-only mode did not deny write tools"
    [ -z "$output" ] || printf '    %s\n' "$output"
    FAIL=$((FAIL + 1))
fi

echo "--- Oversized tool output is archived and retrievable ---"
spill_dir="/tmp/ccode_spill_$$"
rm -rf "$spill_dir"
mkdir -p "$spill_dir"
output=$(CCODE_WRITE_TOOLS=1 timeout "$TIMEOUT" "$CCODE" \
    --write --auto-approve \
    --save-session "$spill_dir/spill.json" \
    -p "__ccode_test_result-spill-fixture" 2>&1) || true
if echo "$output" | grep -q "spill ENDMARK9999"; then
    echo "  PASS: truncated tool result retrieved from archive"
    PASS=$((PASS + 1))
else
    echo "  FAIL: archived tool result not retrievable"
    [ -z "$output" ] || printf '    %s\n' "$output" | head -20 || true
    FAIL=$((FAIL + 1))
fi
if [ -d "$spill_dir/spill.json.results" ] && \
   [ -n "$(ls -A "$spill_dir/spill.json.results" 2>/dev/null)" ]; then
    echo "  PASS: archive directory created next to the session"
    PASS=$((PASS + 1))
else
    echo "  FAIL: archive directory missing next to the session"
    FAIL=$((FAIL + 1))
fi
output=$(CCODE_WRITE_TOOLS=1 timeout "$TIMEOUT" "$CCODE" \
    --write --auto-approve --resume "$spill_dir/spill.json" \
    -p "__ccode_test_result-spill-fixture resume" 2>&1) || true
if echo "$output" | grep -q "spill ENDMARK9999"; then
    echo "  PASS: archived result retrieved after --resume"
    PASS=$((PASS + 1))
else
    echo "  FAIL: archived result lost on --resume"
    [ -z "$output" ] || printf '    %s\n' "$output" | head -20 || true
    FAIL=$((FAIL + 1))
fi
rm -rf "$spill_dir"

echo "--- Oversized stderr is archived and retrievable ---"
spill_dir="/tmp/ccode_spill_err_$$"
rm -rf "$spill_dir"
mkdir -p "$spill_dir"
output=$(CCODE_WRITE_TOOLS=1 timeout "$TIMEOUT" "$CCODE" \
    --write --auto-approve --save-session "$spill_dir/err.json" \
    -p "__ccode_test_result-spill-stderr" 2>&1) || true
if echo "$output" | grep -q "stderr-spill ENDERR2"; then
    echo "  PASS: truncated stderr retrieved via stream=stderr"
    PASS=$((PASS + 1))
else
    echo "  FAIL: archived stderr not retrievable"
    [ -z "$output" ] || printf '    %s\n' "$output" | head -20 || true
    FAIL=$((FAIL + 1))
fi
rm -rf "$spill_dir"

echo "--- Thinking + tools must echo reasoning_content ---"
# DeepSeek thinking mode with tools returns HTTP 400 unless every historical
# assistant message carries its reasoning_content back. The mock enforces the
# rule, so this only succeeds when ccode persists and replays it.
output=$(timeout "$TIMEOUT" "$CCODE" -p "__ccode_test_thinking-tools" 2>&1) || true
if echo "$output" | grep -q "thinking-tools done"; then
    echo "  PASS: reasoning_content echoed across tool-call turns"
    PASS=$((PASS + 1))
else
    echo "  FAIL: reasoning_content not echoed (upstream would 400)"
    [ -z "$output" ] || printf '    %s\n' "$output"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
