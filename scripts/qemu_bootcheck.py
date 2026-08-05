#!/usr/bin/env python3
"""qemu_bootcheck.py - event-driven QEMU boot monitor for BasicLinux.

Why this exists
---------------
qemu_drive.py waits for a few known strings with generous timeouts; when the
guest hangs you only find out after the whole 1200s budget burns down, with
no idea which stage died. This script replaces "sleep and hope" with an
event loop:

  * every known boot stage has a pattern and its OWN timeout;
  * a 1s tick samples guest CPU (halted vs spinning) and QMP status;
  * the VGA text plane is read directly via QMP pmemsave 0xB8000, so we can
    print the live screen at any moment without screendump timing issues;
  * on failure it prints a timeline, live screen, last lines, CPU state and
    a concrete suggestion, then exits with a distinct code.

Exit codes: 0 reached the shell prompt, 1 stage timeout / stuck, 2 QEMU
died or never came up, 3 usage error.

Usage
-----
  python3 scripts/qemu_bootcheck.py [--timeout 600] [--stage-timeout 120]
                                    [--mem 24] [--iso PATH] [--hda PATH]
  python3 scripts/qemu_bootcheck.py --attach /tmp/qmp.sock   # diagnose a live VM
"""
import argparse
import json
import os
import re
import select
import socket
import subprocess
import sys
import time

ANSI_RE = re.compile(
    r'\x1b\[[0-9;?]*[a-zA-Z]|\x1b[()][0-9a-zA-Z]|\x1b[=>]|\x1b\][^\x07]*\x07')

VGA_TEXT_ADDR = 0xb8000      # VGA text plane, 2 bytes per cell
VGA_TEXT_SIZE = 32768        # full 32KB: 8 hardware pages of 80x25

# ── known boot stages: (name, regex, suggestion if this stage stalls) ──
STAGES = [
    ("grub",   re.compile(r'GRUB|grub'), None),
    ("kernel", re.compile(r'Linux version|Decompressing Linux|Booting'),
     "kernel never printed: check ISO/initrd integrity and -drive wiring"),
    ("init",   re.compile(r'init started|rc starting|starting rc|Welcome'),
     "init/rc did not run: inspect rc for an early hang"),
    ("ramdisk", re.compile(r'building RW /etc ramdisk'),
     "rc stalled building the /etc ramdisk: mke2fs /dev/ram2 likely out of "
     "memory at 24MB - raise -m or boot with 'lowmem' (skips ramdisk)"),
    ("swapfile", re.compile(r'Searching for swapfile|attempt to access beyond '
                            r'end of device'),
     "init stalled probing hda1 for swap: 'attempt to access beyond end of "
     "device 03:01' means the partition table in fshd.img is stale (guest "
     "sees a different size). Rebuild fshd.img / fix the partition table"),
    ("mount",  re.compile(r'Mounting root filesystem|mounting|e2fsck'),
     "root fs mount/fsck hung: fsck on a dirty fs.img blocks forever; boot "
     "with the fsck-skipping /etc/rc or run e2fsck on the host"),
    ("login",  re.compile(r'Login as root|login:'),
     "never reached the login prompt"),
    ("shell",  re.compile(r'#\s*$|#>\s*$|#\s+$'),
     "login did not produce a shell prompt"),
]

# The customized ghost-disk rc prints the BL3 welcome banner ("The BL3
# kernel has ...") right after RC-COMPLETE, then drops straight to the
# shell prompt without the ramdisk/login stages the strict stage machine
# expects. Seeing the banner means the guest is fully booted; treat it
# as boot success (the ordered stage machine alone would stall forever
# at 'ramdisk' on this build).
BOOT_BANNER_RE = re.compile(r'The BL3 kernel has')


def strip_ansi(text):
    return ANSI_RE.sub('', text)


def read_vga_text(cmd):
    """Fetch the guest's visible 80x25 screen via pmemsave and decode it.

    Pure text, no OCR needed. NB: the 2.2 kernel scrolls via hardware
    panning, so the visible window is usually NOT at 0xB8000+0 - find
    it via the CRTC start-address registers (0x3D4/0x3D5, idx 0x0C/0x0D).
    Reading only offset 0 yields a stale pre-scroll screen and once
    masqueraded as a "boot hang" (guest was actually idle at a shell
    prompt). See ADAPT_STATUS.md.
    """
    path = '/tmp/qemu_bootcheck_vram.bin'
    try:
        cmd('pmemsave', {'val': VGA_TEXT_ADDR, 'size': VGA_TEXT_SIZE,
                         'filename': path})
    except SystemExit:
        return None
    try:
        with open(path, 'rb') as f:
            data = f.read(VGA_TEXT_SIZE)
    except OSError:
        return None
    if len(data) < 32768:
        return None

    def hmp(c):
        r = cmd('human-monitor-command', {'command-line': c})
        return r if isinstance(r, str) else ''

    def inb(port):
        r = hmp('i /b 0x%x' % port)
        try:
            return int(r.split('=')[1].strip(), 16)
        except (IndexError, ValueError):
            return None

    off = 0
    hmp('o /b 0x3d4 0x0c'); hi = inb(0x3d5)
    hmp('o /b 0x3d4 0x0d'); lo = inb(0x3d5)
    if hi is not None and lo is not None:
        off = ((hi << 8) | lo) * 2
    data = (data + data)[off:off + 4000]

    rows = []
    for row in range(25):
        chars = []
        for col in range(80):
            b = data[(row * 80 + col) * 2]
            if b == 0:
                b = 0x20
            chars.append(chr(b) if 0x20 <= b < 0x7f else ' ')
        rows.append(''.join(chars).rstrip())
    while rows and not rows[-1].strip():
        rows.pop()
    return '\n'.join(rows) if any(r.strip() for r in rows) else None


class QEMU:
    def __init__(self, argv, qmp_sock):
        self.sock_path = qmp_sock
        self.master_fd = None
        self.proc = None
        if argv:
            import pty as ptymod
            master_fd, slave_fd = ptymod.openpty()
            self.master_fd = master_fd
            self.proc = subprocess.Popen(
                argv, stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
                close_fds=True)
            os.close(slave_fd)
        self.sock = None
        for _ in range(150):
            if os.path.exists(qmp_sock):
                break
            time.sleep(0.2)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        for _ in range(150):
            try:
                s.connect(qmp_sock)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        else:
            raise SystemExit(f"qemu_bootcheck: cannot connect to QMP socket {qmp_sock}")
        self.sock = s
        self._recv()  # greeting
        self.cmd('qmp_capabilities')

    def _recv(self):
        buf = b''
        try:
            while b'\n' not in buf:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return None
                buf += chunk
            msg = json.loads(buf.split(b'\n')[0])
            if 'event' in msg:
                return ('event', msg['event'])
            return msg
        except (socket.timeout, json.JSONDecodeError):
            return None

    def cmd(self, execute, arguments=None):
        req = {'execute': execute}
        if arguments is not None:
            req['arguments'] = arguments
        self.sock.sendall(json.dumps(req).encode() + b'\n')
        deadline = time.time() + 5
        while time.time() < deadline:
            msg = self._recv()
            if msg is None:
                continue
            if isinstance(msg, tuple):
                continue  # async event while waiting: ignore
            if 'return' in msg:
                return msg['return']
            if 'error' in msg:
                return {'error': msg['error']}
        return {'error': 'no reply'}

    def cpu_usage(self):
        if not self.proc or self.proc.poll() is not None:
            return None
        try:
            with open(f'/proc/{self.proc.pid}/stat') as f:
                parts = f.read().split()
            utime, stime = int(parts[13]), int(parts[14])
        except (OSError, IndexError, ValueError):
            return None
        now = time.time()
        if not hasattr(self, '_cpu_last'):
            self._cpu_last = (utime + stime, now)
            return 0.0
        (pt, pt0) = self._cpu_last
        elapsed = now - pt0
        self._cpu_last = (utime + stime, now)
        if elapsed <= 0:
            return 0.0
        return 100.0 * (utime + stime - pt) / elapsed

    def quit(self):
        try:
            self.cmd('quit')
        except Exception:
            pass
        if self.proc:
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class Monitor:
    def __init__(self, q, stage_timeout, overall_timeout, silent_limit):
        self.q = q
        self.stage_timeout = stage_timeout
        self.overall_timeout = overall_timeout
        self.silent_limit = silent_limit
        self.t0 = time.time()
        self.lines = []
        self.timeline = []
        self.stage = 0            # index into STAGES
        self.stage_started = self.t0
        self.last_line_at = self.t0
        self.last_silent_warn = 0
        self.last_vga_at = 0
        self.events = []

    def log(self, line):
        self.lines.append(line)
        if len(self.lines) > 400:
            del self.lines[:len(self.lines) - 400]
        self.last_line_at = time.time()

    def note(self, text, ts=None):
        self.timeline.append(f'[{ts or time.time() - self.t0:7.1f}s] {text}')
        print(self.timeline[-1], flush=True)

    def on_line(self, line):
        s = line.strip().rstrip()
        if not s:
            return
        self.log(s)
        if BOOT_BANNER_RE.search(s):
            self.stage = len(STAGES)
            self.stage_started = time.time()
            self.note(f"boot banner: {s[:72]}")
        while self.stage < len(STAGES):
            name, pat, _ = STAGES[self.stage]
            if pat.search(s):
                self.stage += 1
                self.stage_started = time.time()
                self.note(f"stage {name}: {s[:72]}")
            else:
                break

    def stage_name(self):
        return STAGES[self.stage][0] if self.stage < len(STAGES) else 'done'

    def diagnose(self, reason):
        print('\n=== DIAGNOSIS ===', flush=True)
        print(f'reason: {reason}', flush=True)
        now = time.time()
        vga = read_vga_text(self.q.cmd)
        if vga:
            print('\n-- live screen (VGA text plane 0xB8000) --', flush=True)
            print(vga, flush=True)
        else:
            print('\n-- live screen: VGA plane not readable yet --', flush=True)
        cpu = self.q.cpu_usage()
        status = self.q.cmd('query-status')
        print(f'\nqemu cpu: {cpu:.0f}% (guest {"halted" if cpu is not None and cpu < 5 else "busy"})'
              if cpu is not None else 'qemu cpu: n/a', flush=True)
        print(f'qmp status: {status}', flush=True)
        print(f'\n-- last {min(25, len(self.lines))} screen lines --', flush=True)
        for l in self.lines[-25:]:
            print(f'  {l[:110]}', flush=True)
        for i, (name, _, sug) in enumerate(STAGES):
            if i >= self.stage:
                break
        if self.stage < len(STAGES):
            _, _, sug = STAGES[self.stage]
            if sug:
                print(f'\nsuggest: {sug}', flush=True)
        print(f'\n-- timeline --', flush=True)
        for t in self.timeline:
            print(f'  {t}', flush=True)

    def run(self):
        self.note('monitor start (stage=grub, stage-timeout=%ss, '
                  'overall=%ss)' % (self.stage_timeout, self.overall_timeout))
        rlist = []
        if self.q.master_fd is not None:
            rlist.append(self.q.master_fd)
        rlist.append(self.q.sock)
        next_tick = time.time() + 1
        while True:
            now = time.time()
            try:
                readable, _, _ = select.select(rlist, [], [], 0.2)
            except (OSError, ValueError):
                readable = []
            if self.q.master_fd is not None and self.q.master_fd in readable:
                try:
                    chunk = os.read(self.q.master_fd, 4096)
                except OSError:
                    chunk = b''
                if chunk:
                    for raw in chunk.decode('utf-8', 'replace').split('\n'):
                        self.on_line(strip_ansi(raw))
                else:
                    return self.fail('qemu pty closed (process exit)')
            if self.q.sock in readable:
                msg = self.q._recv()
                if isinstance(msg, tuple):
                    self.note(f"qemu event: {msg[1]}")
            if now >= next_tick:
                next_tick = now + 1
                rc = self.tick(now)
                if rc is not None:
                    return rc
            if self.q.proc and self.q.proc.poll() is not None:
                return self.fail(f'qemu exited rc={self.q.proc.returncode}')
            if self.stage >= len(STAGES):
                self.note('boot complete: shell prompt reached')
                return 0

    def tick(self, now):
        elapsed = now - self.t0
        if elapsed > self.overall_timeout:
            return self.fail(f'overall timeout {self.overall_timeout}s')
        if self.stage < len(STAGES) and \
           now - self.stage_started > self.stage_timeout:
            return self.fail(f"stage '{self.stage_name()}' timed out after "
                             f'{self.stage_timeout}s')
        if now - self.last_line_at > self.silent_limit and \
           now - self.last_silent_warn > 15:
            self.last_silent_warn = now
            self.note(f'!! screen silent {now - self.last_line_at:.0f}s '
                      f'(stage {self.stage_name()})')
        if now - self.last_vga_at > 10 and (self.stage < len(STAGES)):
            self.last_vga_at = now
            vga = read_vga_text(self.q.cmd)
            if vga:
                self.note('screen: ' + ' | '.join(
                    l.strip() for l in vga.split('\n') if l.strip())[:120])
        return None

    def fail(self, reason):
        self.diagnose(reason)
        return 1


def main():
    ap = argparse.ArgumentParser(
        description='Event-driven QEMU boot monitor for BasicLinux.')
    ap.add_argument('--attach', metavar='QMP_SOCK',
                    help='diagnose an already-running VM (no qemu launch)')
    ap.add_argument('--timeout', type=int, default=600,
                    help='overall budget before failing (default 600)')
    ap.add_argument('--stage-timeout', type=int, default=120,
                    help='per-stage budget before failing (default 120)')
    ap.add_argument('--silent-limit', type=int, default=30,
                    help='warn when no screen output for this long')
    ap.add_argument('--mem', type=int, default=24)
    ap.add_argument('--iso', default='/home/moyamryia/ccode/blgrub2.iso')
    ap.add_argument('--hda', default='/home/moyamryia/ccode/fshd.img')
    ap.add_argument('--kernel', default='/home/moyamryia/ccode/zimage',
                    help='bzImage; with --kernel we boot -kernel style '
                         '(no GRUB/ISO) instead of -cdrom')
    ap.add_argument('--initrd', default='',
                    help='initrd for -kernel style boot (default: none)')
    ap.add_argument('--append', default='',
                    help='kernel cmdline for -kernel style boot')
    ap.add_argument('--disk-boot', action='store_true',
                    help='boot the -hda disk as a standalone ghost disk '
                         '(MBR+GRUB, no cdrom), QEMU -boot c')
    ap.add_argument('--hostfwd', default='',
                    help='host->guest inbound port forwards, comma separated '
                         '"HOSTPORT-GUESTPORT" (e.g. "2222-22"). '
                         'Default: none.')
    args = ap.parse_args()

    qmp_sock = args.attach or '/tmp/qmp.sock'
    if os.path.exists(qmp_sock):
        print(f'qemu_bootcheck: stale QMP socket {qmp_sock} removed', flush=True)
        try:
            os.unlink(qmp_sock)
        except OSError:
            pass

    netdev = 'user,id=n1'
    for fwd in [f.strip() for f in args.hostfwd.split(',') if f.strip()]:
        netdev += ',hostfwd=tcp::%s' % fwd.replace('-', '-:', 1)

    if args.attach:
        argv = None
    elif args.disk_boot:
        argv = ['qemu-system-i386', '-m', str(args.mem),
                '-hda', args.hda,
                '-netdev', netdev,
                '-device', 'pcnet,netdev=n1',
                '-display', 'curses',
                '-qmp', 'unix:%s,server,nowait' % qmp_sock]
    elif args.kernel:
        argv = ['qemu-system-i386', '-m', str(args.mem),
                '-kernel', args.kernel, '-hda', args.hda,
                '-netdev', netdev,
                '-device', 'pcnet,netdev=n1',
                '-display', 'curses',
                '-qmp', 'unix:%s,server,nowait' % qmp_sock]
        if args.initrd:
            argv += ['-initrd', args.initrd]
        if args.append:
            argv += ['-append', args.append]
    else:
        argv = ['qemu-system-i386', '-m', str(args.mem),
                '-cdrom', args.iso, '-boot', 'd',
                '-hda', args.hda,
                '-display', 'curses',
                '-qmp', 'unix:%s,server,nowait' % qmp_sock]

    try:
        q = QEMU(argv, qmp_sock)
        m = Monitor(q, args.stage_timeout, args.timeout, args.silent_limit)
        rc = m.run()
    except SystemExit as e:
        print(str(e), file=sys.stderr)
        return 3
    finally:
        if not args.attach:
            try:
                q.quit()
            except Exception:
                pass
    return rc


if __name__ == '__main__':
    sys.exit(main())
