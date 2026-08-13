#!/usr/bin/env python3
"""ccode macOS TUI diagnostic. Run in the directory containing ./ccode and
./ccode-cli on the affected Mac:

    python3 diag_macos_tui.py

Writes diag.txt; send that file back for analysis. No network access needed.
"""
import os, pty, select, struct, subprocess, sys, time, fcntl, termios

report = []

def log(title, body):
    report.append("=== %s ===\n%s\n" % (title, body))

def run(cmd, stdin_data=None, timeout=10):
    try:
        p = subprocess.run(cmd, input=stdin_data, capture_output=True,
                           timeout=timeout)
        return "rc=%d\nstdout: %r\nstderr: %r" % (p.returncode, p.stdout,
                                                  p.stderr)
    except Exception as e:
        return "EXC: %r" % (e,)

# 1. Environment
env_info = []
for k in ("TERM", "TERM_PROGRAM", "LC_ALL", "LANG", "CCODE_BACKEND"):
    env_info.append("%s=%s" % (k, os.environ.get(k)))
try:
    env_info.append("stty size: " + os.popen("stty size").read().strip())
except Exception as e:
    env_info.append("stty size failed: %r" % e)
for c in ("sw_vers", "uname -a", "cc -dumpmachine"):
    env_info.append("$ %s -> %s" % (c, os.popen(c + " 2>&1").read().strip()))
env_info.append("ccode exists: %s" % os.path.exists("./ccode"))
env_info.append("ccode-cli exists: %s" % os.path.exists("./ccode-cli"))
log("environment", "\n".join(env_info))

# 2. Backend standalone (JSON mode handshake)
log("backend standalone",
    run(["./ccode-cli", "--json"],
        stdin_data=b'{"type":"hello","model":"x","workspace":"."}\n'))

# 3. PTY capture of the real TUI
def pty_capture():
    env = dict(os.environ)
    env["CCODE_BACKEND"] = os.path.abspath("./ccode-cli")
    pid, fd = pty.fork()
    if pid == 0:
        os.execvpe("./ccode", ["./ccode"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    out = b""
    def drain(t):
        nonlocal out
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                out += data
    drain(2.0)
    os.write(fd, b"hi\n")
    drain(2.0)
    os.write(fd, b"/exit\n")
    drain(1.0)
    status = None
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        _, status = os.waitpid(pid, 0)
    except OSError:
        pass
    return out, status

try:
    raw, status = pty_capture()
    body = "waitpid status: %r\nbytes: %d\n\nraw output:\n%r" % (
        status, len(raw), raw)
    checks = {
        "alt_screen_entered": b"\x1b[?1049h" in raw,
        "alt_screen_restored_on_exit": b"\x1b[?1049l" in raw,
        "status_bar_drawn": b"v0.1" in raw,
        "input_row_23_targeted": b"\x1b[23;" in raw,
        "backend_ready_received": b"backend connected" in raw,
        "backend_error_event": b'"type":"error"' in raw
            or b"error" in raw.lower(),
        "cursor_shown_again": b"\x1b[?25h" in raw,
    }
    body += "\n\nchecks:\n" + "\n".join(
        "  %s: %s" % (k, v) for k, v in checks.items())
    log("pty capture", body)
except Exception as e:
    log("pty capture", "EXC: %r" % (e,))

with open("diag.txt", "w") as f:
    f.write("\n".join(report))
print("wrote diag.txt (%d sections)" % len(report))
