#!/usr/bin/env python3
"""Real-scenario TUI integration tests under a PTY.

Drives both TUI frontends the way a user would, against the mock provider:

  - ccode-tui  (fork backend over the JSON Lines protocol)
  - ccode      (combined binary: in-process TUI)

Covered: streaming turns, CJK width handling, reasoning display, markdown
multi-line rendering, tool permission allow/deny, provider error resilience,
terminal resize (SIGWINCH) and permission-prompt Ctrl-C cancellation.

Requires ./ccode-tui and ./ccode to be built (`make ccode ccode-tui`).
Run explicitly; not part of the default `make test` because those targets
must not build the paused ccode binaries."""

import fcntl
import codecs
import os
import pty
import re
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TUI_BIN = os.path.join(REPO, "ccode-tui")
CCODE_BIN = os.path.join(REPO, "ccode")
CLI_BIN = os.path.join(REPO, "ccode-cli")
MOCK_PROVIDER = os.path.join(REPO, "tests", "mock_provider.py")
PORT = int(os.environ.get("CCODE_TUI_REAL_PORT", "9917"))
TIMEOUT = 20

ANSI_RE = re.compile(r"\x1b(?:\[[0-9;?]*[@-~]|O[@-~]|.)")


def strip_ansi(s):
    return ANSI_RE.sub("", s)


def start_mock():
    import subprocess
    proc = subprocess.Popen(
        [sys.executable, MOCK_PROVIDER, str(PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    if proc.poll() is not None:
        print("FAIL: mock provider failed to start")
        sys.exit(1)
    return proc


class Tui:
    """One TUI process under a PTY with marker-based expectations."""

    def __init__(self, binary, workspace, session_dir, env_extra=None,
                 rows=24, cols=80):
        env = dict(os.environ)
        env.update({
            "TERM": "xterm",
            "CCODE_API_BASE": "http://127.0.0.1:%d/v1" % PORT,
            "CCODE_API_KEY": "test-key",
            "CCODE_MODEL": "test-model",
            "CCODE_SESSION_DIR": session_dir,
        })
        if env_extra:
            env.update(env_extra)
        self.rows, self.cols = rows, cols
        self.alive = True
        # Incremental decoder: a UTF-8 sequence split across two os.read
        # chunks must not turn into U+FFFD artifacts in our own buffer.
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(workspace)
            os.execve(binary, [binary], env)
        self.pid, self.fd = pid, fd
        self._set_winsize(rows, cols)
        self.buf = ""

    def _set_winsize(self, rows, cols):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))

    def read_until(self, marker, timeout=TIMEOUT, mark=None):
        """Read until `marker` shows up in ANSI-stripped output.

        The TUI redraws the full message list every frame, so anything that
        appeared once stays in the buffer forever. Pass `mark` (a buffer
        offset captured before the action under test) to only match fresh
        output — required for repeated markers like the permission prompt."""
        start = len(self.buf) if mark is None else mark
        deadline = time.time() + timeout
        while time.time() < deadline:
            if marker in strip_ansi(self.buf[start:]):
                return True
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    self.alive = False
                    return False
                if not chunk:
                    self.alive = False
                    return False
                self.buf += self.decoder.decode(chunk)
        return False

    def settle(self, quiet=1.2, max_wait=TIMEOUT):
        """Drain until no new output for `quiet` seconds (turn fully done)."""
        deadline = time.time() + max_wait
        last = time.time()
        while time.time() < deadline:
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    self.alive = False
                    return
                if not chunk:
                    self.alive = False
                    return
                self.buf += self.decoder.decode(chunk)
                last = time.time()
            elif time.time() - last >= quiet:
                return

    def send(self, data):
        os.write(self.fd, data)

    def submit(self, line):
        self.send(line.encode() + b"\r")

    def resize(self, rows, cols):
        self.rows, self.cols = rows, cols
        self._set_winsize(rows, cols)
        # TIOCSWINSZ already signals the foreground group; an explicit
        # SIGWINCH makes the race with handler installation harmless.
        try:
            os.kill(self.pid, signal.SIGWINCH)
        except ProcessLookupError:
            pass

    def quit(self):
        """Exit via /exit; kill as a fallback. Returns True on clean exit."""
        try:
            self.submit("/exit")
        except OSError:
            pass
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            try:
                wpid, _ = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                return True
            if wpid != 0:
                return True
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    if not os.read(self.fd, 65536):
                        return True
                except OSError:
                    return True
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except (ProcessLookupError, ChildProcessError):
            return True
        return False


def wait_for_file(path, timeout=TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.1)
    return False


def test_fork_tui(workspace, session_dir, failures):
    """ccode-tui: fork backend over the JSON Lines protocol."""
    if not os.path.exists(TUI_BIN) or not os.path.exists(CLI_BIN):
        failures.append("ccode-tui/ccode-cli not built; make ccode-tui ccode-cli")
        return
    tui = Tui(TUI_BIN, workspace, session_dir,
              env_extra={"CCODE_BACKEND": CLI_BIN,
                         "CCODE_WRITE_TOOLS": "1"})
    try:
        # Boot: status bar, message header, hint line.
        if not tui.read_until("test-model"):
            failures.append("fork: status bar (model) never rendered")
        for marker in ("Messages", "/help /thinking /clear /exit"):
            if marker not in strip_ansi(tui.buf):
                failures.append("fork: boot screen missing %r" % marker)

        # Streaming turn: three deltas appended into one message.
        tui.submit("__ccode_test_tui-stream-delta")
        if not tui.read_until("You said: hello world"):
            failures.append("fork: streamed deltas not assembled")

        # CJK turn: full-width text renders without mojibake.
        tui.submit("你好世界")
        if not tui.read_until("You said: 你好世界"):
            failures.append("fork: CJK echo missing")
        if "\ufffd" in tui.buf:
            failures.append("fork: CJK produced replacement chars (split "
                            "multi-byte sequence?)")

        # Reasoning: /thinking on, then a fixture that streams a
        # chain-of-thought before the answer.
        tui.submit("/thinking on")
        if not tui.read_until("Thinking enabled"):
            failures.append("fork: /thinking on not confirmed")
        tui.submit("__ccode_test_reasoning-newlines")
        if not tui.read_until("final answer"):
            failures.append("fork: reasoning turn never finished")
        if "think line one" not in strip_ansi(tui.buf):
            failures.append("fork: reasoning content not displayed")

        # Markdown: fence stays open across lines; markers are not shown.
        tui.submit("__ccode_test_tui-markdown")
        if not tui.read_until("added safely"):
            failures.append("fork: markdown turn never finished")
        md = strip_ansi(tui.buf)
        if "int add(int a, int b)" not in md or "return a + b" not in md:
            failures.append("fork: fenced code block not rendered")
        if "**" in md.split("__ccode_test_tui-markdown")[-1]:
            failures.append("fork: markdown bold markers leaked raw")

        # Permission allow: write_file approved -> tool runs -> file exists.
        mark = len(tui.buf)
        tui.submit("__ccode_test_write-calls")
        if not tui.read_until("Allow?", mark=mark):
            failures.append("fork: permission prompt never shown")
        tui.send(b"y")
        if not tui.read_until("Write result received."):
            failures.append("fork: turn after allow never finished")
        if not wait_for_file(os.path.join(workspace, "integration-write.txt")):
            failures.append("fork: approved write_file did not run")

        # Permission deny: both requests refused, no side effects. Each
        # prompt must be matched with a fresh mark: the redraw loop keeps
        # every earlier "Allow?" frame in the buffer forever.
        mark = len(tui.buf)
        tui.submit("__ccode_test_deny-no-side-effects")
        if not tui.read_until("Allow?", mark=mark):
            failures.append("fork: deny prompt never shown")
        tui.send(b"n")
        mark = len(tui.buf)
        if not tui.read_until("Allow?", mark=mark):
            failures.append("fork: second deny prompt never shown")
        tui.send(b"n")
        if not tui.read_until("All tool requests denied."):
            failures.append("fork: denial result not reported")
        if (os.path.exists(os.path.join(workspace, "must_not_exist.txt")) or
                os.path.exists(os.path.join(workspace,
                                            "must_not_exist_marker.txt"))):
            failures.append("fork: denied tools had side effects")

        # Provider error: surfaced, TUI stays alive for the next turn.
        tui.submit("__ccode_test_non-200")
        if not tui.read_until("Bad request"):
            failures.append("fork: provider error not surfaced")
        tui.submit("after-error")
        if not tui.read_until("You said: after-error"):
            failures.append("fork: TUI unusable after provider error")

        # Resize: SIGWINCH must keep the TUI working on the new geometry.
        tui.resize(30, 100)
        tui.submit("after-resize")
        if not tui.read_until("You said: after-resize"):
            failures.append("fork: turn after resize failed")

        # /clear: the backend answers the JSON "clear" event with a
        # "cleared" event (lowercase text).
        tui.submit("/clear")
        if not tui.read_until("conversation cleared"):
            failures.append("fork: /clear not confirmed")
    finally:
        if not tui.quit():
            failures.append("fork: TUI did not exit on /exit")


def test_inproc_tui(workspace, session_dir, failures):
    """ccode: combined binary, in-process TUI."""
    if not os.path.exists(CCODE_BIN):
        failures.append("ccode not built; make ccode")
        return
    tui = Tui(CCODE_BIN, workspace, session_dir,
              env_extra={"CCODE_WRITE_TOOLS": "1"})
    try:
        if not tui.read_until("test-model"):
            failures.append("inproc: status bar never rendered")

        # CJK turn with session chain minted from the first prompt.
        tui.submit("你好世界")
        if not tui.read_until("You said: 你好世界"):
            failures.append("inproc: CJK echo missing")
        if "\ufffd" in tui.buf:
            failures.append("inproc: CJK produced replacement chars")

        # Permission prompt dismissed with Ctrl-C: the in-process agent's
        # SIGINT handler raises the cancel flag; the prompt must deny,
        # abort the turn and return to a working input row. Recovery is
        # asserted via /history — every later model turn would still route
        # to the write-calls fixture (the last prefixed prompt wins), so a
        # plain echo assertion cannot prove recovery here.
        mark = len(tui.buf)
        tui.submit("__ccode_test_write-calls")
        if not tui.read_until("Allow?", mark=mark):
            failures.append("inproc: permission prompt never shown")
        tui.send(b"\x03")
        # The cancelled turn ends asynchronously (denial recorded, session
        # saved); wait for the screen to go quiet before probing recovery.
        tui.settle()
        mark = len(tui.buf)
        tui.submit("/history")
        if not tui.read_until("[2] __ccode_test_write-calls", mark=mark):
            failures.append("inproc: TUI stuck after Ctrl-C on prompt")
        if os.path.exists(os.path.join(workspace, "integration-write.txt")):
            failures.append("inproc: Ctrl-C approved the write (should deny)")

        # Auto session chain exists for the prompts above.
        if not any(f.startswith("auto-") for f in os.listdir(session_dir)):
            failures.append("inproc: auto session chain not minted")
    finally:
        if not tui.quit():
            failures.append("inproc: TUI did not exit on /exit")


def main():
    mock = start_mock()
    failures = []
    workspaces = []
    try:
        for name, fn in (("fork TUI", test_fork_tui),
                         ("in-process TUI", test_inproc_tui)):
            workspace = tempfile.mkdtemp(prefix="ccode_tui_real_ws_")
            session_dir = tempfile.mkdtemp(prefix="ccode_tui_real_sess_")
            workspaces += [workspace, session_dir]
            fn(workspace, session_dir, failures)
            print("  ran: %s" % name)
    finally:
        mock.terminate()
        mock.wait()
        for w in workspaces:
            shutil.rmtree(w, ignore_errors=True)

    if failures:
        print("FAIL: real-scenario TUI tests")
        for f in failures:
            print("  -", f)
        sys.exit(1)
    print("PASS: real-scenario TUI tests (fork + in-process: streaming, "
          "CJK, reasoning, markdown, permissions, errors, resize, cancel)")


if __name__ == "__main__":
    main()
