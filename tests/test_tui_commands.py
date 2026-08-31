#!/usr/bin/env python3
"""Integration test: slash commands of the combined `ccode` binary (in-process
TUI under a PTY).

Starts the mock provider, runs `ccode` in a PTY, drives every in-process slash
command, and verifies the rendered responses plus the session-chain file
(resume/save to the same session across turns)."""

import os
import pty
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CCODE = os.path.join(REPO, "ccode")
MOCK_PROVIDER = os.path.join(REPO, "tests", "mock_provider.py")
PORT = int(os.environ.get("CCODE_TUI_TEST_PORT", "9911"))
TIMEOUT = 15


def start_mock():
    proc = subprocess.Popen(
        [sys.executable, MOCK_PROVIDER, str(PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    if proc.poll() is not None:
        print("FAIL: mock provider failed to start")
        sys.exit(1)
    return proc


def main():
    session_dir = tempfile.mkdtemp(prefix="ccode_tui_cmd_sess_")
    workspace = tempfile.mkdtemp(prefix="ccode_tui_cmd_ws_")
    mock = start_mock()
    failures = []
    try:
        pid, fd = pty.fork()
        if pid == 0:
            env = dict(os.environ)
            env.update({
                "TERM": "xterm",
                "CCODE_API_BASE": "http://127.0.0.1:%d/v1" % PORT,
                "CCODE_API_KEY": "test",
                "CCODE_MODEL": "test-model",
                "CCODE_SESSION_DIR": session_dir,
            })
            os.chdir(workspace)
            os.execve(CCODE, [CCODE], env)

        buf = ""

        def read_until(marker, timeout=TIMEOUT):
            nonlocal buf
            deadline = time.time() + timeout
            while time.time() < deadline:
                if marker in buf:
                    return
                r, _, _ = select.select([fd], [], [], 0.2)
                if r:
                    try:
                        chunk = os.read(fd, 65536)
                    except OSError:
                        return
                    if not chunk:
                        return
                    buf += chunk.decode("utf-8", "replace")

        def send(cmd, marker):
            os.write(fd, cmd.encode() + b"\r")
            read_until(marker)

        read_until("test-model")
        for cmd, marker in [
            ("/help", "Commands: /help /clear /exit /history"),
            ("/history", "Session history (0 prompts):"),
            ("/model", "Current model: test-model"),
            ("/model test-model-2", "Model switched to: test-model-2"),
            ("/model", "Current model: test-model-2"),
            # The mock provider serves no /models endpoint: the TUI must
            # report failure honestly instead of dumping the raw body.
            ("/models", "Could not fetch model list."),
            ("/sessions", "No saved sessions."),
            ("/session new bad", "Invalid session name."),
            ("/session new tui_cmd_test.json", "New session started."),
            ("/compact", "not supported in the in-process TUI"),
        ]:
            send(cmd, marker)
            if marker not in buf:
                failures.append("missing %r after %r" % (marker, cmd))

        # A chained prompt turns into a saved session file (context chain).
        send("hello", "You said: hello")
        time.sleep(0.5)
        chain = os.path.join(session_dir, "tui_cmd_test.json")
        if not os.path.exists(chain):
            failures.append("session chain file not created")

        send("/history", "[1] hello")
        if "[1] hello" not in buf:
            failures.append("history missing recorded prompt")

        send("/resume --list", "tui_cmd_test.json")
        if "tui_cmd_test.json" not in buf:
            failures.append("/resume --list missing session")

        os.write(fd, b"/exit\r")
        exited = False
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            wpid, _ = os.waitpid(pid, os.WNOHANG)
            if wpid != 0:
                exited = True
                break
            r, _, _ = select.select([fd], [], [], 0.3)
            if r:
                try:
                    if not os.read(fd, 65536):
                        exited = True
                        break
                except OSError:
                    exited = True
                    break
        if not exited:
            failures.append("TUI did not exit on /exit")
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
    finally:
        mock.terminate()
        mock.wait()
        shutil.rmtree(session_dir, ignore_errors=True)
        shutil.rmtree(workspace, ignore_errors=True)

    if failures:
        print("FAIL: in-process TUI slash commands")
        for f in failures:
            print("  -", f)
        sys.exit(1)
    print("PASS: in-process TUI slash commands (help/history/model/models/"
          "sessions/session/resume/compact/exit)")


if __name__ == "__main__":
    main()
