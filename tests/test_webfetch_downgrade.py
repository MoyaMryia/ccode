#!/usr/bin/env python3
"""End-to-end regression for the HTTPS->HTTP redirect downgrade guard.

Drives the real ccode_web_fetch() path (via the --fetch-url probe built from
tests/test_webfetch_security.c) against two loopback mock servers:

  A: HTTPS (self-signed cert, CN=localhost) answering 302 with
     Location: http://127.0.0.1:<B>/
  B: plain HTTP recording whether it was reached and which Authorization
     header it saw.

Asserts:
  * default: the redirect is refused with "insecure downgrade" and B is
    never contacted;
  * with CCODE_WEB_FETCH_ALLOW_INSECURE_REDIRECT=1: the redirect is followed,
    but the Authorization header is stripped because the origin changed
    (scheme + host + port all differ).

Only loopback targets are used. The script exits 0 (SKIP) when openssl, a
TLS-capable build of the probe, or a supported platform is unavailable.
"""

import glob
import http.server
import os
import platform
import ssl
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE = os.path.join(tempfile.gettempdir(), "test_webfetch_security_probe")

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL: %s" % msg, file=sys.stderr)


def skip(msg):
    print("test_webfetch_downgrade: SKIP (%s)" % msg)
    sys.exit(0)


def platform_src():
    system = platform.system()
    if system == "Linux":
        return "platform_linux.c"
    if system == "Darwin":
        return "platform_darwin.c"
    if system in ("FreeBSD", "NetBSD", "OpenBSD", "DragonFly"):
        return "platform_bsd.c"
    return None


def build_probe(plat):
    objs = sorted(glob.glob(os.path.join(
        ROOT, ".build", "https", "vendor", "mbedtls", "library", "*.o")))
    if not objs:
        skip("no prebuilt mbedTLS objects under .build/https; "
             "run `make` once to populate them")
    cmd = [
        os.environ.get("CC", "cc"),
        "-D_POSIX_C_SOURCE=200112L",
        "-I" + os.path.join(ROOT, "vendor", "mbedtls", "include"),
        "-I" + os.path.join(ROOT, "src", "platform"),
        "-O1", "-g",
        os.path.join(ROOT, "tests", "test_webfetch_security.c"),
        os.path.join(ROOT, "vendor", "json", "json.c"),
        os.path.join(ROOT, "vendor", "html", "html.c"),
        os.path.join(ROOT, "src", "platform", plat),
        "-o", PROBE,
    ] + objs
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        skip("probe build failed: %s" %
             (r.stderr.strip().splitlines()[-1] if r.stderr else "?"))


def make_cert():
    certdir = tempfile.mkdtemp(prefix="ccode_wf_downgrade_")
    key = os.path.join(certdir, "server.key")
    cert = os.path.join(certdir, "server.pem")
    try:
        r = subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", key, "-out", cert, "-days", "2", "-nodes",
            "-subj", "/CN=localhost",
        ], capture_output=True)
    except FileNotFoundError:
        skip("openssl not found")
    if r.returncode != 0:
        skip("openssl could not generate a certificate")
    return key, cert


recorded = {"hit": False, "auth": None, "path": None}
redirect_target = {"location": ""}


class RecordHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        recorded["hit"] = True
        recorded["auth"] = self.headers.get("Authorization")
        recorded["path"] = self.path
        body = b"reached"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class RedirectHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(302)
        self.send_header("Location", redirect_target["location"])
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *args):
        pass


def run_probe(cert, url, extra_env=None):
    env = dict(os.environ)
    env["CCODE_CA_FILE"] = cert
    env["CCODE_WEB_FETCH_ALLOW_PRIVATE"] = "1"
    env["CCODE_WEB_FETCH_PROBE_AUTH"] = "Bearer DOWNGRADE_SECRET"
    if extra_env:
        env.update(extra_env)
    return subprocess.run([PROBE, "--fetch-url", url],
                          capture_output=True, text=True, env=env, timeout=30)


def main():
    plat = platform_src()
    if plat is None:
        skip("unsupported platform %s" % platform.system())
    build_probe(plat)
    key, cert = make_cert()

    # Server B: plain HTTP, records the redirect landing.
    server_b = http.server.HTTPServer(("127.0.0.1", 0), RecordHandler)
    port_b = server_b.server_address[1]
    threading.Thread(target=server_b.serve_forever, daemon=True).start()

    # Server A: HTTPS, always redirects to B.
    redirect_target["location"] = "http://127.0.0.1:%d/leak" % port_b
    server_a = http.server.HTTPServer(("127.0.0.1", 0), RedirectHandler)
    port_a = server_a.server_address[1]
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    server_a.socket = ctx.wrap_socket(server_a.socket, server_side=True)
    threading.Thread(target=server_a.serve_forever, daemon=True).start()

    url = "https://localhost:%d/start" % port_a

    # 1. Default: refuse the downgrade and never contact B.
    recorded["hit"] = False
    recorded["auth"] = None
    r = run_probe(cert, url)
    check("insecure downgrade" in r.stdout,
          "expected downgrade refusal, got: %r" % r.stdout.strip())
    check(recorded["hit"] is False,
          "plaintext server B was contacted despite the downgrade refusal")

    # 2. Explicit override: follow it, but strip the credential.
    recorded["hit"] = False
    recorded["auth"] = None
    r = run_probe(cert, url,
                  {"CCODE_WEB_FETCH_ALLOW_INSECURE_REDIRECT": "1"})
    check(recorded["hit"] is True,
          "override did not follow the redirect (got: %r)" % r.stdout.strip())
    check('"status":200' in r.stdout,
          "followed redirect did not reach B (got: %r)" % r.stdout.strip())
    auth = recorded["auth"]
    check(auth is None or "DOWNGRADE_SECRET" not in auth,
          "Authorization leaked across the HTTPS->HTTP downgrade: %r" % auth)

    server_a.shutdown()
    server_b.shutdown()

    if failures:
        print("test_webfetch_downgrade: %d check(s) FAILED" % len(failures),
              file=sys.stderr)
        return 1
    print("test_webfetch_downgrade: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
