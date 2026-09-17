#!/usr/bin/env python3
"""Stress harness for ccode's command risk classifier
(src/security/sandbox.c).

Pipes commands to `tests/test_agent --filter-probe`, which calls
ccode_command_classify() and reports the class (0 ALLOW, 1 TIER1, 2 TIER2,
3 TIER3, 4 ESCALATE, 5 REFUSE). The goal is to surface:

  * false positives -- legitimate developer commands that get hard-refused
    or needlessly escalated to a typed confirmation;
  * false negatives -- clearly malicious commands that slip through ALLOW;
  * the workspace-tolerance bypass: any command that merely mentions the
    workspace root suppresses all soft-pattern checks.

Usage:
  tests/fuzz_command_paths.py [--probe PATH] [--workspace DIR]
                              [--count N] [--seed S] [--show K]
Exit codes: 0 (report produced), 2 (harness failure).
"""

import argparse
import random
import struct
import subprocess
import sys

WS = "/home/dev/proj"

# Commands that a developer legitimately runs and that should NOT be refused.
# (workspace, command, note)
BENIGN = [
    (WS, "ls -la", "basic"),
    (WS, "pwd", "basic"),
    (WS, "make test", "build"),
    (WS, "make clean", "build"),
    (WS, "gcc -O2 -o app src/main.c", "build"),
    (WS, "clang -c src/foo.c", "build"),
    (WS, "git status", "git"),
    (WS, "git diff --stat", "git"),
    (WS, "git log --oneline -20", "git"),
    (WS, "git add . && git commit -m 'fix'", "git"),
    (WS, "git config --global user.email dev@example.com", "git"),
    (WS, "grep -rn TODO src", "search"),
    (WS, "find . -name '*.c'", "search"),
    (WS, "sed -i 's/foo/bar/' src/f.c", "edit"),
    (WS, "chmod +x scripts/run.sh", "edit"),
    (WS, "python3 -m pytest -q", "test"),
    (WS, "python3 scripts/build.py", "test"),
    (WS, "npm install", "js"),
    (WS, "npm test", "js"),
    (WS, "cargo build --release", "rust"),
    (WS, "go test ./...", "go"),
    (WS, "docker build -t app .", "docker"),
    (WS, "docker compose up -d", "docker"),
    (WS, "curl -sS https://example.com/health", "net"),
    (WS, "tar -czf build/out.tgz src", "pack"),
    (WS, "unzip dist/app.zip -d build", "pack"),
    (WS, "ssh deploy@example.com 'systemctl restart app'", "ssh"),
    (WS, "scp build/app.tgz deploy@example.com:/tmp/", "scp"),
    (WS, "rm -rf build", "tmp"),
    (WS, "rm -rf /tmp/ccode-build", "tmp"),
    (WS, "cat /etc/passwd", "system-read"),
    (WS, "cat /etc/hosts", "system-read"),
    (WS, "cat /proc/meminfo", "system-read"),
    (WS, "cat /proc/cpuinfo", "system-read"),
    (WS, "uname -a", "system-read"),
    (WS, "df -h", "system-read"),
    (WS, "ps aux", "system-read"),
    (WS, "cat /usr/share/doc/gcc/README", "system-read"),
    (WS, "echo $PATH", "env"),

    # workspace-relative absolute paths must be tolerated
    (WS, "cat /home/dev/proj/src/main.c", "ws-abs"),
    (WS, "grep -rn foo /home/dev/proj/src", "ws-abs"),
    (WS, "python3 /home/dev/proj/tools/build.py", "ws-abs"),
    (WS, "make -C /home/dev/proj", "ws-abs"),
    (WS, "rm -rf /home/dev/proj/build", "ws-abs"),
    ("/root", "ls /root", "ws-root"),
    ("/root", "cat /root/proj/main.c", "ws-root"),
    ("/root", "make -C /root/proj", "ws-root"),

    # legitimate files whose names merely contain sensitive substrings
    (WS, "cat tests/fixtures/known_hosts_sample.txt", "substr-bait"),
    (WS, "grep -rn known_hosts docs/", "substr-bait"),
    (WS, "python3 tests/test_authorized_keys.py", "substr-bait"),
    (WS, "cat docs/id_rsa_format.md", "substr-bait"),
    (WS, "cat vendor/aws-sdk/NOTES.txt", "substr-bait"),
    (WS, "cat .gitconfig.example", "substr-bait"),
    (WS, "git log -- .gitconfig.example", "substr-bait"),
    (WS, "cat src/netrc_parser.c", "substr-bait"),
    (WS, "cat config/npmrc.txt", "substr-bait"),
    (WS, "ls docker/", "substr-bait"),
    (WS, "cat /proc/self/status", "proc-self"),

    # more collisions a real dev hits
    (WS, "cat /etc/ssh/ssh_config", "ssh-sysconf"),
    (WS, "ssh -F ~/.ssh/config host", "ssh-config"),
    (WS, "cat ~/.gitconfig", "git-config"),
    (WS, "cat /home/dev/.gitconfig", "git-config"),
    (WS, "cat /home/dev/.netrc.example", "substr-bait"),
    (WS, "cat config/.npmrc.example", "substr-bait"),
    (WS, "cat docs/.pypirc.sample", "substr-bait"),
    (WS, "cat .dockerignore", "substr-bait"),
    (WS, "cat /home/dev/.aws/config", "cloud-config"),
    (WS, "cat /proc/self/mountinfo", "proc-self"),
    (WS, "cat /proc/self/cgroup", "proc-self"),
    (WS, "ls /var/mail", "mail"),
    (WS, "ls /home", "home-list"),
    (WS, "cat /home/dev/.local/share/app/log", "soft-home"),
    (WS, "journalctl -u app -n 50", "logs"),
    (WS, "systemctl --user status app", "systemd"),
    (WS, "ssh-keygen -l -f ./test_key.pub", "keygen-scan"),
    (WS, "git -C /home/dev/other status", "other-repo"),

    # per-user config is a soft pattern; editing your own is legitimate
    (WS, "cat /home/dev/.config/nvim/init.lua", "soft-config"),
    (WS, "ls ~/.ssh", "ssh-no-slash"),

    # other common dev domains (should all be allowed)
    (WS, "kubectl get pods -A", "k8s"),
    (WS, "kubectl apply -f deploy/manifest.yaml", "k8s"),
    (WS, "helm upgrade --install app ./chart", "k8s"),
    (WS, "terraform plan -out=tfplan", "iac"),
    (WS, "terraform apply tfplan", "iac"),
    (WS, "ansible-playbook -i hosts site.yml", "iac"),
    (WS, "psql -h localhost -U app -f schema.sql", "db"),
    (WS, "mysql -u root -p app < dump.sql", "db"),
    (WS, "sqlite3 data.db '.schema'", "db"),
    (WS, "redis-cli --scan --pattern 'cache:*'", "db"),
    (WS, "aws sts get-caller-identity", "cloud"),
    (WS, "aws s3 ls s3://my-bucket/", "cloud"),
    (WS, "gcloud projects list", "cloud"),
    (WS, "az account show", "cloud"),
    (WS, "pip install -r requirements.txt", "pkg"),
    (WS, "poetry install --no-root", "pkg"),
    (WS, "apt-get update", "pkg"),
    (WS, "vim src/main.c", "editor"),
    (WS, "nano README.md", "editor"),
    (WS, "black --check src", "lint"),
    (WS, "ruff check .", "lint"),
    (WS, "eslint src --ext .ts", "lint"),
    (WS, "prettier --write 'src/**/*.ts'", "lint"),
    (WS, "jest --coverage", "test"),
    (WS, "vitest run", "test"),
    (WS, "git worktree add ../wt feature", "git"),
    (WS, "git submodule update --init --recursive", "git"),
    (WS, "git stash push -m wip", "git"),
    (WS, "git tag -a v1.0 -m release", "git"),
    (WS, "git remote -v", "git"),
    (WS, "tail -f /var/log/app.log", "logs"),
    (WS, "cat /etc/os-release", "system-read"),
    (WS, "cat /etc/resolv.conf", "system-read"),
    (WS, "cat /etc/hosts.deny", "system-read"),
    (WS, "cat /etc/fstab", "system-read"),
    (WS, "ls /etc/ssl/certs", "system-read"),
    (WS, "cat /proc/self/cmdline", "proc-self"),
    (WS, "cat /proc/self/fd/0", "proc-self"),
    (WS, "cat ~/.bashrc", "dotfiles"),
    (WS, "cat ~/.zshrc", "dotfiles"),
    (WS, "cat ~/.profile", "dotfiles"),
    (WS, "cat ~/.vimrc", "dotfiles"),
    (WS, "cat ~/.tmux.conf", "dotfiles"),
    (WS, "cat ~/.ssh/known_hosts", "ssh-known-hosts"),
    (WS, "cat ~/.docker/config.json", "docker-cfg"),
    (WS, "cat ~/.npmrc", "npm-cfg"),
    (WS, "ls /var/mail", "mail"),
    (WS, "ls /var/tmp", "tmp"),
    (WS, "fd -e c src", "search"),
    (WS, "rg --files -g '*.py'", "search"),
    (WS, "jq '.name' package.json", "json"),
    (WS, "yq '.spec' manifest.yaml", "yaml"),
    (WS, "openssl x509 -in cert.pem -noout -text", "tls"),
    (WS, "ssh-keyscan github.com", "tls"),
    (WS, "systemd-run --user --scope sleep 1", "systemd"),
]

# Commands that must be refused.
MALICIOUS = [
    (WS, "cat /etc/shadow", "shadow"),
    (WS, "cat /etc/gshadow", "shadow"),
    (WS, "cat /etc/sudoers", "sudoers"),
    (WS, "cat ~/.ssh/id_rsa", "key"),
    (WS, "cat ~/.ssh/id_rsa/", "key"),
    (WS, "cat /home/bob/.aws/credentials", "cloud"),
    (WS, "cat /root/.gnupg/secring.gpg", "gnupg"),
    (WS, "cat /proc/self/environ", "proc"),
    (WS, "rm -rf /", "rm-root"),
    (WS, "rm -fr /*", "rm-root"),
    (WS, "ssh-keygen -f /root/.ssh/id_ed25519", "key"),
    (WS, "cat ~/.git-credentials", "git-creds"),
    (WS, "cat /var/spool/cron/root", "spool"),
    (WS, "mkfs.ext4 /dev/sda1", "destructive"),
    (WS, "dd if=/dev/zero of=/dev/sda bs=1M", "destructive"),
    (WS, "shutdown -h now", "destructive"),
    (WS, "chown -R root:root /etc", "destructive"),
    (WS, "cat /home/bob/.config/secret", "outside-soft"),
]

# Should be refused, but may pass because mentioning the workspace anywhere
# suppresses the soft-pattern checks (arg_touches_workspace).
BYPASS = [
    (WS, "cat /home/dev/proj/README /home/bob/.config/secret", "ws-mention-bypass"),
    (WS, "cp /home/dev/proj/x /home/bob/.config/secret", "ws-mention-bypass"),
    (WS, "grep -rn x /home/dev/proj /home/other/.config/creds", "ws-mention-bypass"),
]

HARD = [
    "/etc/shadow", "~/.ssh/id_rsa", "/home/bob/.aws/credentials",
    "/proc/self/environ", "/root/.gnupg/secring.gpg", "/var/spool/cron/root",
    "/etc/ssh/ssh_host_rsa_key", "/.netrc",
]
SOFT_OUTSIDE = [
    "/home/other/.config/secret", "/home/bob/.aws/keys.txt",
    "/other/.config/token",
]
VERBS = ["cat", "ls -la", "head -1", "grep -rn x", "python3", "cp",
         "chmod +x", "rm -rf", "tar -czf o.tgz", "ssh u@h"]


def frame(ws, text):
    wb = ws.encode("utf-8")
    tb = text.encode("utf-8")
    return (struct.pack("<I", len(wb)) + wb +
            struct.pack("<I", len(tb)) + tb)


def rand_benign(rnd, ws):
    verb = rnd.choice(VERBS)
    paths = ["src/main.c", "build", ".", "tests", "README.md",
             "scripts/run.sh", "/tmp/x", "/usr/include/stdio.h"]
    return ws, "%s %s" % (verb, rnd.choice(paths)), "rand-benign"


def rand_evil(rnd, ws):
    return ws, "%s %s" % (rnd.choice(VERBS), rnd.choice(HARD)), "rand-evil"


def rand_bypass(rnd, ws):
    return ws, "%s %s/x %s" % (rnd.choice(VERBS), ws,
                               rnd.choice(SOFT_OUTSIDE)), "rand-bypass"


def run_probe(probe, cases, timeout):
    blob = b"".join(frame(c[0], c[1]) for c in cases)
    try:
        proc = subprocess.Popen([probe, "--filter-probe"],
                                stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        out, err = proc.communicate(blob, timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        return None, "timeout", ""
    except OSError as exc:
        return None, "spawn failed: %s" % exc, ""
    return out.decode("utf-8", "replace").splitlines(), proc.returncode, \
        err.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--probe", default="./tests/test_agent")
    ap.add_argument("--workspace", default=WS,
                    help="workspace passed to the filter for generated cases")
    ap.add_argument("--count", type=int, default=0,
                    help="extra randomized cases per generator")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=60)
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    ws = args.workspace
    rnd = random.Random(args.seed)
    cases = []
    for bws, cmd, note in BENIGN:
        cases.append((bws, cmd, "allow", note))
    for mws, cmd, note in MALICIOUS:
        cases.append((mws, cmd, "refuse", note))
    for pws, cmd, note in BYPASS:
        cases.append((pws, cmd, "refuse", note))
    for _ in range(args.count):
        b = rand_benign(rnd, ws)
        cases.append((b[0], b[1], "allow", b[2]))
        e = rand_evil(rnd, ws)
        cases.append((e[0], e[1], "refuse", e[2]))
        p = rand_bypass(rnd, ws)
        cases.append((p[0], p[1], "refuse", p[2]))

    lines, rc, err = run_probe(args.probe, cases, args.timeout)
    if lines is None:
        print("FAIL: %s" % rc)
        return 2
    if rc < 0:
        print("FAIL: probe died with signal %d after %d/%d cases"
              % (-rc, len(lines), len(cases)))
        return 1

    false_pos = []   # expected allow but refused
    misses = []      # expected refuse but allowed
    for i, (ws, cmd, expect, note) in enumerate(cases):
        if i >= len(lines):
            print("FAIL: no output for case %d" % i)
            return 1
        parts = lines[i].split("\t", 1)
        cls = int(parts[0])
        reason = parts[1] if len(parts) > 1 else ""
        # Classifier values: 0 ALLOW, 1 TIER1, 2 TIER2, 3 TIER3,
        # 4 ESCALATE, 5 REFUSE. ESCALATE/REFUSE are hard-refused; anything
        # above ALLOW at least forces a human confirmation.
        hard_refused = cls >= 4
        gated = cls != 0
        if expect == "allow" and hard_refused:
            false_pos.append((ws, cmd, note, reason))
        elif expect == "refuse" and not gated:
            misses.append((ws, cmd, note))

    if err.strip():
        print("(probe stderr)\n%s" % err.strip())

    print("cases=%d  false-positives=%d  misses=%d"
          % (len(cases), len(false_pos), len(misses)))

    def group(rows, key):
        g = {}
        for r in rows:
            g.setdefault(key(r), []).append(r)
        return g

    if false_pos:
        print("\n== FALSE POSITIVES (legit command refused): %d ==" % len(false_pos))
        by_reason = group(false_pos, lambda r: r[3])
        for reason, rows in sorted(by_reason.items(), key=lambda kv: -len(kv[1])):
            print("  [%d] %s" % (len(rows), reason or "(no reason)"))
            for ws, cmd, note, _ in rows[:args.show]:
                print("      %-14s %s" % (note, cmd))
            if len(rows) > args.show:
                print("      ... %d more" % (len(rows) - args.show))

    if misses:
        print("\n== MISSES (should be refused but allowed): %d ==" % len(misses))
        by_note = group(misses, lambda r: r[2])
        for note, rows in sorted(by_note.items(), key=lambda kv: -len(kv[1])):
            print("  [%d] %s" % (len(rows), note))
            for ws, cmd, _ in rows[:args.show]:
                print("      ws=%s  %s" % (ws, cmd))
            if len(rows) > args.show:
                print("      ... %d more" % (len(rows) - args.show))

    if false_pos or misses:
        print("FAIL: %d false positive(s), %d miss(es)" % (len(false_pos),
                                                           len(misses)))
        return 1
    print("PASS: command-path filter corpus stayed within policy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
