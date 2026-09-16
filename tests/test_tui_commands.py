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


def wait_for(check, timeout=5.0):
    """Poll check() until truthy or timeout; returns the last value."""
    deadline = time.time() + timeout
    result = check()
    while not result and time.time() < deadline:
        time.sleep(0.1)
        result = check()
    return result


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
            ("/help", "Slash commands:"),
            ("/history", "Session history (0 prompts):"),
            ("/model", "Current model: test-model"),
            ("/model test-model-2", "Model switched to: test-model-2"),
            ("/model", "Current model: test-model-2"),
            # The mock provider serves no /models endpoint: the TUI must
            # report failure honestly instead of dumping the raw body.
            ("/models", "Could not fetch model list."),
            ("/sessions", "No saved sessions."),
            ("/session new bad", "Invalid session name."),
            ("/compact", "Nothing to compact yet."),
        ]:
            send(cmd, marker)
            if marker not in buf:
                failures.append("missing %r after %r" % (marker, cmd))

        # A bare prompt mints an auto session chain (context inheritance).
        send("hello", "You said: hello")
        if not wait_for(lambda: [f for f in os.listdir(session_dir)
                                 if f.startswith("auto-")]):
            failures.append("auto session file not created on first prompt")

        # A named chain via /session new, then a turn onto it.
        send("/session new tui_cmd_test.json", "New session started.")
        send("again", "You said: again")
        if not wait_for(lambda: os.path.exists(
                os.path.join(session_dir, "tui_cmd_test.json"))):
            failures.append("named session chain file not created")

        send("/history", "[2] again")
        if "[1] hello" not in buf or "[2] again" not in buf:
            failures.append("history missing recorded prompts")

        send("/resume --list", "tui_cmd_test.json")
        if "tui_cmd_test.json" not in buf:
            failures.append("/resume --list missing session")

        # Up/Down recall prompt history: two Ups reach "hello", Enter
        # resubmits it and the provider answers again. Slash commands are not
        # recorded, so the history is exactly [hello, again].
        mark = len(buf)
        os.write(fd, b"\x1b[A\x1b[A")
        time.sleep(0.2)
        os.write(fd, b"\r")
        deadline = time.time() + TIMEOUT
        while time.time() < deadline and "You said: hello" not in buf[mark:]:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                buf += chunk.decode("utf-8", "replace")
        if "You said: hello" not in buf[mark:]:
            failures.append("Up/Down history did not recall the older prompt")

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
