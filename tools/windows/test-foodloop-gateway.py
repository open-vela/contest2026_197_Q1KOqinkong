"""Minimal local-only vision transport test for FoodLoop M2.

Sends one synthetic RGB565 frame to the local home gateway and prints the
draft summary plus the preprocessed image metadata (the gateway upscales and
optionally enhances the frame before forwarding it to MiMo).

Usage:
    python test-foodloop-gateway.py [--port 8789] [--width 320] [--height 240]
"""

import argparse
import json
import urllib.error
import urllib.request


def build_gradient_frame(width, height):
    """A deterministic red-green-blue gradient that exercises the pipeline."""
    frame = bytearray()
    for y in range(height):
        for x in range(width):
            red = x * 31 // (width - 1)
            green = y * 63 // (height - 1)
            blue = 8
            pixel = (red << 11) | (green << 5) | blue
            frame.extend((pixel & 0xFF, pixel >> 8))
    return bytes(frame)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8789)
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    args = parser.parse_args()

    frame = build_gradient_frame(args.width, args.height)

    request = urllib.request.Request(
        "http://127.0.0.1:{}/v1/foodloop/analyze-rgb565".format(args.port),
        data=frame,
        headers={
            "Content-Type": "application/octet-stream",
            "X-FoodLoop-Width": str(args.width),
            "X-FoodLoop-Height": str(args.height),
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=190) as response:
            payload = json.loads(response.read())
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", "replace")
        raise SystemExit("gateway HTTP {}: {}".format(error.code, detail))

    print("status={}".format(payload.get("status")))
    image = payload.get("image") or {}
    print(
        "image={}x{} format={} bytes={}".format(
            image.get("width"), image.get("height"),
            image.get("format"), image.get("bytes"),
        )
    )
    print("item_count={}".format(len(payload.get("draft", {}).get("items", []))))
    print("scan_notes={}".format(payload.get("draft", {}).get("scan_notes", "")[:300]))

    # Sanity check: the gateway must have returned preprocessed image metadata.
    if not image.get("width") or not image.get("height"):
        raise SystemExit("gateway did not return preprocessed image metadata")


if __name__ == "__main__":
    main()
