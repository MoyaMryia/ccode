#!/usr/bin/env python3
"""HTTPS mock provider for ccode tests with a self-signed certificate.

Usage:
    python3 tests/https_mock.py PORT

Writes a temporary cert and key under /tmp/ccode_https_mock/.
The PEM file path is printed on stderr; pass it via CCODE_CA_FILE.
"""

import http.server, json, ssl, os, sys

cert_dir = "/tmp/ccode_https_mock"
os.makedirs(cert_dir, exist_ok=True)
key_file = os.path.join(cert_dir, "server.key")
cert_file = os.path.join(cert_dir, "server.pem")

if not os.path.exists(cert_file):
    from subprocess import run
    run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
         "-keyout", key_file, "-out", cert_file,
         "-days", "30", "-nodes",
         "-subj", "/CN=localhost"], check=True)
    os.chmod(key_file, 0o600)

RESPONSES = {}

def load_responses(fixtures_dir):
    if os.path.isdir(fixtures_dir):
        for fname in os.listdir(fixtures_dir):
            if fname.endswith('.json'):
                path = os.path.join(fixtures_dir, fname)
                with open(path) as f:
                    data = json.load(f)
                test_name = fname.replace('.json', '')
                RESPONSES[test_name] = data

class MockHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len).decode("utf-8") if content_len else ""
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            self.send_error(401, "Missing or invalid Authorization header")
            return
        try:
            req = json.loads(body)
        except json.JSONDecodeError:
            self.send_error(400, "Invalid JSON body")
            return
        prompt = ""
        for msg in req.get("messages", []):
            if msg.get("role") == "user":
                prompt = msg.get("content", "")
                break
        if prompt == "__ccode_test_tls-delay":
            import time
            time.sleep(3)
        response_text = "HTTPS reply: {}".format(prompt)
        sse = "data: {}\n\ndata: [DONE]\n\n".format(
            json.dumps({
                "id": "https-mock",
                "object": "chat.completion.chunk",
                "choices": [{"index": 0, "delta": {"content": response_text},
                             "finish_reason": "stop"}]
            }))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(sse)))
        self.end_headers()
        self.wfile.write(sse.encode("utf-8"))

    def log_message(self, format, *args):
        if os.environ.get("MOCK_VERBOSE"):
            super().log_message(format, *args)

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8443
    fixtures_dir = os.path.join(os.path.dirname(__file__), "fixtures")
    load_responses(fixtures_dir)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert_file, key_file)
    server = http.server.HTTPServer(("127.0.0.1", port), MockHandler)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)
    print("Mock HTTPS listening on 127.0.0.1:{}".format(port), file=sys.stderr)
    print("CCODE_CA_FILE={}".format(cert_file), file=sys.stderr)
    server.serve_forever()

if __name__ == "__main__":
    main()
