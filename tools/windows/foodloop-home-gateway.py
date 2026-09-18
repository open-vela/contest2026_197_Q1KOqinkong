"""FoodLoop M2 development home gateway.

The ESP32-S3-EYE sends one explicit RGB565 scan over the local network. This
gateway converts that frame to PNG in memory, forwards a constrained vision
request through the existing Token Plan bridge, and returns a draft record.
It never persists image data or credentials.

M2 recognition pipeline (all local, in memory):

1. Decode the little-endian RGB565 frame.
2. Optional per-channel contrast stretch (washed-out label photos).
3. Optional integer upscale (default 2x bilinear). The OV2640 board driver
   is hard-coded to QVGA, so a larger image gives the vision encoder more
   pixels for small Chinese label text without any firmware change.
4. Two-stage MiMo prompt: first transcribe every visible character verbatim,
   then structure the transcription into the constrained food draft JSON.
   Stage 2 is text-only, which is cheaper and far more reliable for field
   extraction than asking the model to go straight from image to JSON.

The gateway accepts any even frame size with an RGB565 length that matches,
so a future firmware with a higher-resolution capture mode needs no gateway
change.
"""

import argparse
import base64
import binascii
import json
import os
import re
import struct
import time
import urllib.error
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


MAX_RAW_BYTES = 2048 * 2048 * 2
MIN_FRAME_DIM = 16
MAX_FRAME_DIM = 2048
MAX_RESPONSE_BYTES = 64 * 1024
MIMO_MAX_COMPLETION_TOKENS = 500
MIMO_TRANSCRIBE_MAX_TOKENS = 800
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

FOOD_DRAFT_PROMPT = """This is one user-authorized scan of a food package or ingredient.
Return only compact JSON with this exact top-level shape:
{"items":[{"name":"string","category":"produce|dairy|meat|drink|snack|leftover|other","quantity_hint":"string|null","date_text":"string|null","date_type":"expiry|production|best_before|unknown","expiry_date":"YYYY-MM-DD|null","storage":"room|refrigerated|frozen|unknown","confidence":0.0,"needs_confirmation":true,"evidence":["string"]}],"scan_notes":"string"}
Rules: identify only what the image supports; use null for unreadable values;
never infer a calendar expiry date from a production date or a vague shelf-life;
nutrition and quantity require user confirmation; evidence must state visible
packaging, text, or object cues. The output is a draft, not a final record."""

TRANSCRIBE_PROMPT = """You are an OCR assistant for a kitchen food label. This is one
explicit user-authorized photo of a food package or ingredient label.
Transcribe ALL visible text on the label, verbatim, in reading order. Preserve the
original characters exactly, including Chinese characters, Latin letters, digits and
punctuation. Output plain text lines only: no JSON, no commentary, no markdown. If a
part of the text is unreadable, write [unreadable] in its place. Do not infer, correct,
translate, or add anything that is not visible in the image."""

STRUCTURE_PROMPT = """You are the food-record extractor for FoodLoop. Below is the verbatim
OCR text of a food package or ingredient label captured by the device. Some lines may
contain [unreadable] where the OCR could not resolve a character; treat those as null.
Return only compact JSON with this exact top-level shape:
{"items":[{"name":"string","category":"produce|dairy|meat|drink|snack|leftover|other","quantity_hint":"string|null","date_text":"string|null","date_type":"expiry|production|best_before|unknown","expiry_date":"YYYY-MM-DD|null","storage":"room|refrigerated|frozen|unknown","confidence":0.0,"needs_confirmation":true,"evidence":["string"]}],"scan_notes":"string"}
Rules: use null for any value the text does not support; never infer a calendar expiry
date from a production date or a vague shelf-life; distinguish expiry vs production vs
best-before dates; nutrition and quantity require user confirmation; evidence must quote
the exact OCR line that supports each value. The output is a draft, not a final record.

OCR text:
__TRANSCRIPTION__"""


def png_chunk(kind, data):
    return (
        struct.pack(">I", len(data))
        + kind
        + data
        + struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF)
    )


def rgb565_to_rgb(raw, width, height):
    """Decode one little-endian RGB565 frame into a flat RGB bytearray."""
    expected = width * height * 2
    if len(raw) != expected:
        raise ValueError("RGB565 length does not match dimensions")

    rgb = bytearray(width * height * 3)
    index = 0
    output = 0
    for _ in range(width * height):
        pixel = raw[index] | (raw[index + 1] << 8)
        index += 2
        rgb[output] = ((pixel >> 11) & 0x1F) * 255 // 31
        rgb[output + 1] = ((pixel >> 5) & 0x3F) * 255 // 63
        rgb[output + 2] = (pixel & 0x1F) * 255 // 31
        output += 3
    return rgb


def rgb_to_png(rgb, width, height):
    rows = bytearray()
    for row in range(height):
        rows.append(0)
        start = row * width * 3
        rows += rgb[start:start + width * 3]

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return PNG_SIGNATURE + png_chunk(b"IHDR", header) + png_chunk(
        b"IDAT", zlib.compress(bytes(rows), level=9)
    ) + png_chunk(b"IEND", b"")


def rgb565_to_png(raw, width, height):
    """Backwards-compatible single-call helper."""
    return rgb_to_png(rgb565_to_rgb(raw, width, height), width, height)


def enhance_contrast(rgb, width, height):
    """Per-channel min/max stretch for washed-out label photos."""
    count = width * height * 3
    channel_min = [255, 255, 255]
    channel_max = [0, 0, 0]
    for index in range(0, count, 3):
        for channel in range(3):
            value = rgb[index + channel]
            if value < channel_min[channel]:
                channel_min[channel] = value
            if value > channel_max[channel]:
                channel_max[channel] = value

    out = bytearray(rgb)
    for index in range(0, count, 3):
        for channel in range(3):
            low = channel_min[channel]
            high = channel_max[channel]
            if high > low:
                out[index + channel] = (
                    (rgb[index + channel] - low) * 255 // (high - low)
                )
    return out


def upscale_rgb(rgb, width, height, scale):
    """Integer bilinear upscale. scale=1 returns the input unchanged."""
    if scale <= 1 or scale != int(scale):
        return rgb, width, height

    scale = int(scale)
    new_width = width * scale
    new_height = height * scale
    out = bytearray(new_width * new_height * 3)
    output = 0

    for y in range(new_height):
        src_y = (y + 0.5) / scale - 0.5
        y0 = int(src_y)
        y1 = y0 + 1
        if y0 < 0:
            y0 = 0
            y1 = 0
        elif y1 >= height:
            y0 = height - 1
            y1 = height - 1
        fy = min(max(src_y - y0, 0.0), 1.0)

        for x in range(new_width):
            src_x = (x + 0.5) / scale - 0.5
            x0 = int(src_x)
            x1 = x0 + 1
            if x0 < 0:
                x0 = 0
                x1 = 0
            elif x1 >= width:
                x0 = width - 1
                x1 = width - 1
            fx = min(max(src_x - x0, 0.0), 1.0)

            base00 = (y0 * width + x0) * 3
            base01 = (y0 * width + x1) * 3
            base10 = (y1 * width + x0) * 3
            base11 = (y1 * width + x1) * 3
            for channel in range(3):
                top = rgb[base00 + channel] * (1.0 - fx) + rgb[base01 + channel] * fx
                bottom = rgb[base10 + channel] * (1.0 - fx) + rgb[base11 + channel] * fx
                out[output] = int(top * (1.0 - fy) + bottom * fy + 0.5)
                output += 1

    return out, new_width, new_height


def extract_response_text(payload):
    try:
        content = payload["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as error:
        raise ValueError("MiMo response does not contain message content") from error

    if isinstance(content, str):
        if content.strip():
            return content
        raise ValueError("MiMo returned no final content; increase completion token budget")
    if isinstance(content, list):
        return "".join(
            part.get("text", "") for part in content if isinstance(part, dict)
        )
    raise ValueError("MiMo response content is not text")


def parse_food_draft(text):
    candidate = text.strip()
    if candidate.startswith("```"):
        candidate = candidate.split("\n", 1)[1] if "\n" in candidate else ""
        if candidate.rstrip().endswith("```"):
            candidate = candidate.rstrip()[:-3].rstrip()

    parsed = json.loads(candidate)
    if not isinstance(parsed, dict) or not isinstance(parsed.get("items"), list):
        raise ValueError("MiMo draft must contain an items array")

    valid_categories = {"produce", "dairy", "meat", "drink", "snack", "leftover", "other"}
    valid_date_types = {"expiry", "production", "best_before", "unknown"}
    valid_storage = {"room", "refrigerated", "frozen", "unknown"}
    normalized_items = []

    for item in parsed["items"][:8]:
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            continue
        name = item["name"].strip()[:80]
        if not name:
            continue

        expiry_date = item.get("expiry_date")
        if not isinstance(expiry_date, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", expiry_date):
            expiry_date = None

        confidence = item.get("confidence", 0.0)
        if not isinstance(confidence, (int, float)):
            confidence = 0.0

        evidence = item.get("evidence", [])
        if not isinstance(evidence, list):
            evidence = []

        normalized_items.append(
            {
                "name": name,
                "category": item.get("category") if item.get("category") in valid_categories else "other",
                "quantity_hint": item.get("quantity_hint") if isinstance(item.get("quantity_hint"), str) else None,
                "date_text": item.get("date_text") if isinstance(item.get("date_text"), str) else None,
                "date_type": item.get("date_type") if item.get("date_type") in valid_date_types else "unknown",
                "expiry_date": expiry_date,
                "storage": item.get("storage") if item.get("storage") in valid_storage else "unknown",
                "confidence": max(0.0, min(float(confidence), 1.0)),
                "needs_confirmation": True,
                "evidence": [value[:120] for value in evidence if isinstance(value, str)][:4],
            }
        )

    notes = parsed.get("scan_notes", "")
    return {
        "items": normalized_items,
        "scan_notes": notes[:240] if isinstance(notes, str) else "",
    }


class FoodLoopGatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[foodloop-gateway] " + fmt % args, flush=True)

    def send_json(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode(
            "utf-8"
        )
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path != "/health":
            self.send_json(404, {"error": "not found"})
            return
        self.send_json(200, {"status": "ok", "service": "foodloop-home-gateway"})

    def is_trusted_client(self):
        return self.client_address[0] == self.server.board_ip or (
            self.server.allow_local_test and self.client_address[0] in ("127.0.0.1", "::1")
        )

    def do_POST(self):
        if self.path != "/v1/foodloop/analyze-rgb565":
            self.send_json(404, {"error": "not found"})
            return
        if not self.is_trusted_client():
            self.send_json(403, {"error": "untrusted client"})
            return

        try:
            width = int(self.headers.get("X-FoodLoop-Width", "0"))
            height = int(self.headers.get("X-FoodLoop-Height", "0"))
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_json(400, {"error": "invalid dimensions or content length"})
            return

        expected = width * height * 2
        if (width < MIN_FRAME_DIM or height < MIN_FRAME_DIM or
                width > MAX_FRAME_DIM or height > MAX_FRAME_DIM or
                length != expected or expected > MAX_RAW_BYTES):
            self.send_json(400, {"error": "expected one RGB565 frame with matching size"})
            return

        raw = self.rfile.read(length)
        if len(raw) != length:
            self.send_json(400, {"error": "incomplete frame"})
            return

        started_at = time.monotonic()
        try:
            rgb = rgb565_to_rgb(raw, width, height)
            if self.server.enhance:
                rgb = enhance_contrast(rgb, width, height)
            png_width, png_height = width, height
            if self.server.upscale > 1:
                rgb, png_width, png_height = upscale_rgb(
                    rgb, width, height, self.server.upscale
                )
            png = rgb_to_png(rgb, png_width, png_height)
            draft = self.request_mimo_draft(png)
            print(
                "[foodloop-gateway] draft: {} item(s), {:.1f}s".format(
                    len(draft["items"]), time.monotonic() - started_at
                ),
                flush=True,
            )
            self.send_json(
                200,
                {
                    "status": "draft",
                    "draft": draft,
                    "image": {
                        "format": "png",
                        "bytes": len(png),
                        "width": png_width,
                        "height": png_height,
                    },
                },
            )
        except urllib.error.HTTPError as error:
            self.send_json(error.code, {"error": error.read().decode("utf-8", "replace")[:512]})
        except (OSError, ValueError, urllib.error.URLError) as error:
            self.send_json(502, {"error": str(error)[:512]})

    def _post_chat(self, payload):
        bridge = json.loads(Path(self.server.bridge_state).read_text(encoding="ascii"))
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        if len(body) > self.server.max_request_bytes:
            raise ValueError("encoded vision request exceeds local gateway limit")

        request = urllib.request.Request(
            "http://127.0.0.1:{}/v1/chat/completions".format(bridge["port"]),
            data=body,
            headers={
                "Authorization": "Bearer " + bridge["board_token"],
                "Content-Type": "application/json",
                "User-Agent": "foodloop-home-gateway/1.0",
            },
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=180) as response:
            response_body = response.read(MAX_RESPONSE_BYTES + 1)
        if len(response_body) > MAX_RESPONSE_BYTES:
            raise ValueError("MiMo response exceeds gateway limit")
        return extract_response_text(json.loads(response_body))

    def request_mimo_draft(self, png):
        image_url = "data:image/png;base64," + base64.b64encode(png).decode("ascii")

        if self.server.two_stage:
            transcription = self._request_transcription(image_url)
            if transcription and transcription.strip():
                return self._request_structured(transcription)

        # Fallback: single-shot image -> draft.
        payload = {
            "model": self.server.model,
            "thinking": {"type": "disabled"},
            "max_completion_tokens": MIMO_MAX_COMPLETION_TOKENS,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": FOOD_DRAFT_PROMPT},
                        {"type": "image_url", "image_url": {"url": image_url}},
                    ],
                }
            ],
        }
        return parse_food_draft(self._post_chat(payload))

    def _request_transcription(self, image_url):
        payload = {
            "model": self.server.model,
            "thinking": {"type": "disabled"},
            "max_completion_tokens": MIMO_TRANSCRIBE_MAX_TOKENS,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": TRANSCRIBE_PROMPT},
                        {"type": "image_url", "image_url": {"url": image_url}},
                    ],
                }
            ],
        }
        return self._post_chat(payload)

    def _request_structured(self, transcription):
        payload = {
            "model": self.server.model,
            "thinking": {"type": "disabled"},
            "max_completion_tokens": MIMO_MAX_COMPLETION_TOKENS,
            "messages": [
                {
                    "role": "user",
                    "content": STRUCTURE_PROMPT.replace(
                        "__TRANSCRIPTION__", transcription
                    ),
                }
            ],
        }
        return parse_food_draft(self._post_chat(payload))


def main():
    default_state = Path(os.environ.get("LOCALAPPDATA", ".")) / "openvela" / "mimo-bridge.json"
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8789)
    parser.add_argument("--board-ip", required=True)
    parser.add_argument("--bridge-state", default=str(default_state))
    parser.add_argument("--model", default="mimo-v2.5")
    parser.add_argument("--upscale", type=int, default=2,
                        help="integer bilinear upscale factor for the label photo (1 disables)")
    parser.add_argument("--no-enhance", action="store_true",
                        help="disable per-channel contrast stretch")
    parser.add_argument("--no-two-stage", action="store_true",
                        help="disable transcription->structure prompting")
    parser.add_argument("--allow-local-test", action="store_true")
    parser.add_argument("--max-request-bytes", type=int, default=1024 * 1024)
    args = parser.parse_args()

    if args.upscale < 1 or args.upscale > 4:
        raise SystemExit("--upscale must be an integer between 1 and 4")

    if not Path(args.bridge_state).is_file():
        raise SystemExit("Missing MiMo bridge state. Start the Token Plan bridge first.")

    server = ThreadingHTTPServer((args.host, args.port), FoodLoopGatewayHandler)
    server.board_ip = args.board_ip
    server.bridge_state = args.bridge_state
    server.model = args.model
    server.allow_local_test = args.allow_local_test
    server.max_request_bytes = args.max_request_bytes
    server.upscale = args.upscale
    server.enhance = not args.no_enhance
    server.two_stage = not args.no_two_stage
    print(
        "FoodLoop gateway listening on {}:{} for board {}".format(
            args.host, args.port, args.board_ip
        ),
        flush=True,
    )
    print(
        "Pipeline: {}x upscale, contrast {}, two-stage prompting {}".format(
            args.upscale,
            "on" if server.enhance else "off",
            "on" if server.two_stage else "off",
        ),
        flush=True,
    )
    server.serve_forever()


if __name__ == "__main__":
    main()
