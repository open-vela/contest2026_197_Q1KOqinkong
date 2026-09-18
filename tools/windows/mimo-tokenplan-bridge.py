import argparse
import json
import os
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


UPSTREAM = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"
MAX_REQUEST_BYTES = 1024 * 1024


class BridgeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[bridge] " + fmt % args, flush=True)

    def send_body(self, status, body, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path != "/health":
            self.send_body(404, b'{"error":"not found"}')
            return
        self.send_body(200, b'{"status":"ok"}')

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_body(404, b'{"error":"not found"}')
            return

        expected = "Bearer " + self.server.board_token
        if self.headers.get("Authorization") != expected:
            self.send_body(401, b'{"error":"unauthorized"}')
            return

        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_REQUEST_BYTES:
            self.send_body(413, b'{"error":"invalid request size"}')
            return

        body = self.rfile.read(length)
        try:
            payload = json.loads(body)
            if "max_tokens" in payload and "max_completion_tokens" not in payload:
                payload["max_completion_tokens"] = payload.pop("max_tokens")
            body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_body(400, b'{"error":"invalid JSON"}')
            return

        request = urllib.request.Request(
            UPSTREAM,
            data=body,
            headers={
                "Authorization": "Bearer " + self.server.mimo_key,
                "Content-Type": "application/json",
                "User-Agent": "openvela-local-bridge/1.0",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                self.send_body(
                    response.status,
                    response.read(),
                    response.headers.get_content_type(),
                )
        except urllib.error.HTTPError as error:
            self.send_body(
                error.code,
                error.read(),
                error.headers.get_content_type(),
            )
        except Exception as error:
            self.send_body(502, json.dumps({"error": str(error)}).encode("utf-8"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()

    mimo_key = os.environ.get("MIMO_TOKEN_PLAN_KEY")
    board_token = os.environ.get("OPENVELA_BRIDGE_TOKEN")
    if not mimo_key or not board_token:
        raise SystemExit("Missing bridge credentials in process environment.")

    server = ThreadingHTTPServer(("0.0.0.0", args.port), BridgeHandler)
    server.mimo_key = mimo_key
    server.board_token = board_token
    print(f"Bridge listening on port {args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
