#!/usr/bin/env python3
"""guest_build.py - one-shot BasicLinux guest build/test driver (foreground).

Flow:

  host:  tar the ccode sources -> debugfs into the ext2 partition image
         -> reassemble the ghost disk
  guest: boot -> wait for shell -> per step: run "<cmd> > NAME.log 2>&1;
         echo $? > NAME.rc" -> sync -> quit
  host:  debugfs-extract every NAME.log / NAME.rc into --logdir and
         judge the result from those FILES.

Design rules (learned the hard way, see ADAPT_STATUS.md):

  * The VGA screen is a LIVENESS SIGNAL ONLY. The 2.2 kernel scrolls via
    hardware panning, so the visible window is not at 0xB8000+0; we read
    the full 32KB of text memory and take the 25 rows ending at the last
    non-blank row. Even then, never judge results from the screen.
  * All command output and exit codes travel as FILES inside the guest,
    extracted with debugfs after sync - same channel used to inject the
    sources. Complete, no OCR/multimodal needed, works for text-only
    agents.
  * hmp quit does NOT sync the guest fs. Always sync first.
  * sendkey can drop Enter (command sits unexecuted and looks like a
    hang). After typing, if the screen did not change, press ret again.

Usage:
  python3 scripts/guest_build.py [--disk vm/bl3-disk.img]
                                 [--fs vm/fs7.img]
                                 [--cc gcc-egcs-1.1.2]
                                 [--targets 'ccode ccode-cli']
                                 [--logdir logs/guest]
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time

KEYMAP = {
    ' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash', '_': 'shift-minus',
    ':': 'shift-semicolon', ';': 'semicolon', '>': 'shift-dot',
    '<': 'shift-comma', '|': 'shift-backslash', '&': 'shift-7',
    '$': 'shift-4', '#': 'shift-3', '=': 'equal', '+': 'shift-equal',
    '"': 'shift-apostrophe', "'": 'apostrophe', '(': 'shift-9',
    ')': 'shift-0', '!': 'shift-1', '[': 'bracket_left',
    ']': 'bracket_right', '{': 'shift-bracket_left', '}': 'shift-bracket_right',
    '*': 'shift-8', '\\': 'backslash', '`': 'grave_accent',
    '~': 'shift-grave_accent', '?': 'shift-slash', '%': 'shift-5',
    '@': 'shift-2', '^': 'shift-6', ',': 'comma', '\t': 'tab', '\r': 'ret',
}


def key_for(ch):
    if 'a' <= ch <= 'z':
        return ch
    if 'A' <= ch <= 'Z':
        return 'shift-' + ch.lower()
    if '0' <= ch <= '9':
        return ch
    return KEYMAP.get(ch, ch)


class QEMU:
    VRAM = '/tmp/guest_build_vram.bin'

    def __init__(self, argv, qmp_sock, display='gtk'):
        self.sock_path = qmp_sock
        self.lines = []
        self._screen = ''
        self._cmd_lock = threading.Lock()
        if display and os.environ.get('DISPLAY'):
            argv = argv + ['-display', display]
        else:
            argv = argv + ['-display', 'none']
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            close_fds=True)
        for _ in range(200):
            if os.path.exists(qmp_sock):
                break
            time.sleep(0.2)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        for _ in range(200):
            try:
                s.connect(qmp_sock)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        self.sock = s
        self._recv()
        self.cmd('qmp_capabilities')
        threading.Thread(target=self._poller, daemon=True).start()

    def screen_text(self):
        """Visible 80x25 screen decoded from the full 32KB VGA text memory.

        The visible window is the 25 rows ending at the last non-blank
        row of the linear buffer (the kernel writes new lines at
        increasing offsets while hardware-panning). Liveness only.
        """
        r = self.cmd('pmemsave', {'val': 0xb8000, 'size': 32768,
                                  'filename': self.VRAM})
        if r is not None and r != {}:
            return ''
        try:
            with open(self.VRAM, 'rb') as f:
                data = f.read(32768)
        except OSError:
            return ''
        if len(data) < 32768:
            return ''
        rows = []
        for row in range(len(data) // 160):
            rows.append(''.join(
                chr(data[(row * 80 + c) * 2])
                if 0x20 <= data[(row * 80 + c) * 2] < 0x7f else ' '
                for c in range(80)).rstrip())
        last = -1
        for i, line in enumerate(rows):
            if line.strip():
                last = i
        if last < 0:
            return ''
        return '\n'.join(rows[max(0, last - 24):last + 1])

    def _poller(self):
        while True:
            try:
                t = self.screen_text()
            except (OSError, ConnectionError, AttributeError):
                return
            if t:
                self._screen = t
                self.lines = t.split('\n')
            time.sleep(0.3)

    def _recv(self):
        buf = b''
        while b'\n' not in buf:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                return None
            buf += chunk
        try:
            return json.loads(buf.split(b'\n')[0])
        except json.JSONDecodeError:
            return None

    def cmd(self, execute, arguments=None):
        req = {'execute': execute}
        if arguments is not None:
            req['arguments'] = arguments
        with self._cmd_lock:
            self.sock.sendall(json.dumps(req).encode() + b'\n')
            while True:
                msg = self._recv()
                if msg is None:
                    return None
                if 'return' in msg:
                    return msg['return']
                if 'error' in msg:
                    return msg['error']

    def hmp(self, c):
        return self.cmd('human-monitor-command', {'command-line': c})

    def sendkey(self, key):
        self.hmp('sendkey %s' % key)

    def type_text(self, text):
        for ch in text:
            self.sendkey(key_for(ch))
            time.sleep(0.07)

    def wait_screen(self, needle=None, timeout=60, stable=3.0):
        """Best-effort liveness wait: needle appears, or screen stops
        changing for `stable` seconds."""
        t0 = time.time()
        last = self._screen
        last_change = t0
        while time.time() - t0 < timeout:
            time.sleep(0.3)
            if needle and needle in self._screen:
                return True
            if self._screen != last:
                last = self._screen
                last_change = time.time()
            elif not needle and time.time() - last_change >= stable:
                return True
        return False

    def run_cmd(self, cmdline, settle_timeout=30):
        """Type a command and wait until the screen settles.

        Output is expected to be redirected to a file by the caller;
        this only waits for the guest to become idle again."""
        before = self._screen
        self.type_text(cmdline)
        self.sendkey('ret')
        time.sleep(3)
        if self._screen == before:
            self.sendkey('ret')
            time.sleep(2)
        if self._screen == before:
            self.sendkey('ret')
        self.wait_screen(needle=None, timeout=settle_timeout, stable=4.0)

    def quit(self):
        try:
            self.hmp('quit')
        except Exception:
            pass
        try:
            self.proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def prep_sources(workdir, fs_img, disk_img, p2_start_sectors=133120):
    """tar ccode sources and inject into the ext2 partition image."""
    tar = '%s/ccode-src.tar' % workdir
    subprocess.run(
        ['tar', 'cf', tar, '-C', workdir, 'Makefile',
         'src', 'vendor'], check=True)
    subprocess.run(['debugfs', '-w', '-R',
                    'rm /root/ccode-src.tar', fs_img],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    r = subprocess.run(['debugfs', '-w', '-R',
                        'write %s /root/ccode-src.tar' % tar, fs_img],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        sys.exit('guest_build: debugfs write failed')
    subprocess.run(['dd', 'if=%s' % fs_img, 'of=%s' % disk_img,
                    'bs=512', 'seek=%d' % p2_start_sectors, 'conv=notrunc'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print('sources staged: %s -> %s' % (tar, disk_img), flush=True)


def extract_files(disk_img, names, logdir, p2_start_sectors=133120,
                  p2_sectors=718847):
    """Dump the whole guest root partition to logdir/p2.img (browse it
    later with `debugfs -R 'ls /root' <p2.img>` etc.) and pull each
    /root/<name> out of it."""
    os.makedirs(logdir, exist_ok=True)
    tmp = os.path.join(logdir, 'p2.img')
    subprocess.run(['dd', 'if=%s' % disk_img, 'of=%s' % tmp,
                    'bs=512', 'skip=%d' % p2_start_sectors,
                    'count=%d' % p2_sectors],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = {}
    for name in names:
        r = subprocess.run(['debugfs', '-R', 'cat /root/%s' % name, tmp],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        data = r.stdout.replace(b'\0', b'')
        with open(os.path.join(logdir, name), 'wb') as f:
            f.write(data)
        out[name] = data
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disk', default=os.path.expanduser(
        '~/ccode/vm/bl3-disk.img'))
    ap.add_argument('--fs', default=os.path.expanduser('~/ccode/vm/fs7.img'))
    ap.add_argument('--workdir', default=os.path.expanduser('~/ccode'))
    ap.add_argument('--cc', default='gcc-egcs-1.1.2')
    ap.add_argument('--mem', type=int, default=64)
    ap.add_argument('--targets', default='ccode ccode-cli',
                    help='make targets built in the guest (default: both '
                         'binaries, TUI + CLI)')
    ap.add_argument('--logdir', default=os.path.expanduser(
        '~/ccode/logs/guest'))
    ap.add_argument('--skip-prep', action='store_true',
                    help='skip source staging (reuse current disk)')
    ap.add_argument('--hostfwd', default='',
                    help='host->guest inbound port forwards, comma separated '
                         '"HOSTPORT-GUESTPORT" (e.g. "2222-22,8080-80" maps '
                         'host:2222 -> guest:22). Default: none.')
    args = ap.parse_args()

    if not args.skip_prep:
        prep_sources(args.workdir, args.fs, args.disk)

    netdev = 'user,id=n1'
    for fwd in [f.strip() for f in args.hostfwd.split(',') if f.strip()]:
        netdev += ',hostfwd=tcp::%s' % fwd.replace('-', '-:', 1)

    qmp = '/tmp/qmp.sock'
    if os.path.exists(qmp):
        os.unlink(qmp)
    q = QEMU(['qemu-system-i386', '-m', str(args.mem),
              '-hda', args.disk,
              '-netdev', netdev,
              '-device', 'pcnet,netdev=n1',
              '-qmp', 'unix:%s,server,nowait' % qmp], qmp)
    print('qemu up, waiting for boot...', flush=True)

    if not q.wait_screen(needle='/<#', timeout=180):
        print('BOOT FAILED. last screen:')
        print('\n'.join(q.lines[-20:]))
        q.quit()
        return 1
    print('shell up', flush=True)
    time.sleep(3)
    q.run_cmd('cd /root && echo warmup > warmup.log 2>&1; '
              'echo $? > warmup.rc', settle_timeout=15)

    # Every step redirects its output to /root/NAME.log and its exit
    # code to /root/NAME.rc. The screen is only used to notice when the
    # guest is idle again.
    steps = [
        ('probe', 'cd /root && which gcc gcc-egcs-1.1.2 gcc-2.7.2.3 '
                  'make tar sh ar', 30),
        ('unpack', 'cd /root && tar xf ccode-src.tar && ls Makefile src', 60),
        ('tls', 'cd /root && make -C vendor/polarssl-1.3.9/library '
                'CC=gcc-egcs-1.1.2 CFLAGS="-O2 -I../include '
                '-include /root/src/compat/compat.h -I/root/src/compat"', 900),
        ('build', 'cd /root && make RETRO=1 RETRO_NATIVE=1 '
                  'CC=%s %s' % (args.cc, args.targets), 900),
        ('help', 'cd /root && ./ccode-cli --help', 60),
    ]
    for name, cmd, settle in steps:
        full = '%s > %s.log 2>&1; echo $? > %s.rc; echo DONE_%s' % (
            cmd, name, name, name)
        for attempt in (1, 2):
            print('>> %s (try %d): %s' % (name, attempt, full), flush=True)
            q.run_cmd(full, settle_timeout=1)
            if q.wait_screen(needle='DONE_%s' % name, timeout=settle):
                break
            print('!! %s: completion marker not seen, %s' %
                  (name, 'retrying' if attempt == 1 else 'giving up'),
                  flush=True)

    # hmp quit does NOT sync the guest fs - sync or all files above vanish.
    print('>> sync', flush=True)
    q.run_cmd('sync', settle_timeout=60)
    time.sleep(2)
    q.quit()

    names = []
    for name, _, _ in steps:
        names += ['%s.log' % name, '%s.rc' % name]
    files = extract_files(args.disk, names, args.logdir)
    print('logs extracted -> %s' % args.logdir, flush=True)

    rc_all = 0
    for name, _, _ in steps:
        raw = files.get('%s.rc' % name, b'').decode(
            'utf-8', 'replace').strip()
        log = files.get('%s.log' % name, b'').decode(
            'utf-8', 'replace').splitlines()
        print('== %s: rc=%s, log %d lines (full: %s/%s.log)' %
              (name, raw or '?', len(log), args.logdir, name), flush=True)
        for l in log[-5:]:
            print('   %s' % l[:100], flush=True)
        if raw != '0':
            rc_all = 1
    print('=== GUEST BUILD DONE rc=%d ===' % rc_all, flush=True)
    return rc_all


if __name__ == '__main__':
    sys.exit(main())
