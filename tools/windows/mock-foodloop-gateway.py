"""Mock FoodLoop home gateway for testing the board-side confirmation flow.

Returns a fixed, valid draft (as the real gateway would after a MiMo
two-stage run) so the board can exercise the full
capture -> upload -> draft display -> BOOT confirm -> records.jsonl
flow without a MiMo bridge.

Usage:
    python mock-foodloop-gateway.py [--port 8790]

The board then runs:  foodloop scan <PC_IP> 8790
"""

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


DRAFT_ITEMS = [
    {
        "name": "牛奶",
        "category": "dairy",
        "quantity_hint": "1盒",
        "date_text": "保质期至 2026-08-10",
        "date_type": "expiry",
        "expiry_date": "2026-08-10",
        "storage": "refrigerated",
        "confidence": 0.91,
        "needs_confirmation": False,
        "evidence": ["保质期至 2026-08-10"],
    },
    {
        "name": "酸奶",
        "category": "dairy",
        "quantity_hint": "1瓶",
        "date_text": "生产日期见包装 2026-08-01",
        "date_type": "production",
        "expiry_date": None,
        "storage": "refrigerated",
        "confidence": 0.62,
        "needs_confirmation": True,
        "evidence": ["生产日期见包装 2026-08-01"],
    },
]


class MockHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[mock-gateway] " + fmt % args, flush=True)

    def do_GET(self):
        if self.path == "/health":
            body = b'{"status":"ok","service":"mock-foodloop-gateway"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        if self.path != "/v1/foodloop/analyze-rgb565":
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""
        print("[mock-gateway] received %d raw bytes" % len(raw), flush=True)

        # Debug aid: persist the raw frame for off-board pixel analysis.
        import os
        dump = os.environ.get("MOCK_GATEWAY_DUMP_FRAME")
        if dump and raw:
            with open(dump, "wb") as f:
                f.write(raw)
            print("[mock-gateway] dumped frame to %s" % dump, flush=True)

        payload = {
            "status": "draft",
            "draft": {
                "items": DRAFT_ITEMS,
                "scan_notes": "mock draft for confirmation-flow test",
            },
            "image": {"format": "png", "bytes": 1234, "width": 640, "height": 480},
        }
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        print("[mock-gateway] returned draft with %d items"
              % len(DRAFT_ITEMS), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8790)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), MockHandler)
    print("Mock gateway listening on %s:%d" % (args.host, args.port), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
