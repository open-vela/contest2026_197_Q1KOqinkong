# FoodLoop M2 Gateway Protocol

## Purpose

The ESP32-S3-EYE captures RGB565, while the MiMo vision API expects a standard
image type. During development, the FoodLoop home gateway converts a single,
explicitly captured RGB565 frame to PNG entirely in memory and returns a MiMo
food-record draft.

This is a documented LAN gateway, not an undisclosed TLS workaround. It is an
M2 development architecture while direct board TLS support is investigated.

## Device Capture Constraint (important)

The current board camera driver (`esp32s3_board_camera.c`) hard-codes the
OV2640 to **QVGA 320x240 RGB565**: it registers exactly one format and one
frame size, and the sensor register tables are written at driver init. Asking
the V4L2 layer for a larger frame would not change the sensor output and would
produce an incomplete DMA frame. Raising the capture resolution therefore
requires reworking the vendor OV2640 register tables, which cannot be validated
without hardware and is deliberately **out of scope for M2**.

To compensate for QVGA without any firmware change, the gateway:

1. **Upscales** the frame (default 2x integer bilinear) so the vision encoder
   sees more pixels of small Chinese label text.
2. **Enhances contrast** (per-channel min/max stretch) for washed-out photos.
3. **Uses a two-stage MiMo prompt**: transcribe every visible character
   verbatim first, then structure the transcription into the constrained
   draft JSON. The structured pass is text-only, which is cheaper and more
   reliable than image-to-JSON in one step.

If a future firmware adds a higher-resolution capture mode, the gateway
already accepts any frame size whose RGB565 length matches its dimensions, so
no gateway change is required.

## Request

`POST /v1/foodloop/analyze-rgb565`

| Header | Value |
| --- | --- |
| `Content-Type` | `application/octet-stream` |
| `X-FoodLoop-Width` | frame width in pixels (default 320) |
| `X-FoodLoop-Height` | frame height in pixels (default 240) |
| `Content-Length` | `width * height * 2` |

The request body is exactly one little-endian RGB565 frame. The gateway only
accepts the configured board IP and does not write the frame to disk. Frames
outside 16..2048 pixels per side or with a mismatched length are rejected.

## Response

```json
{
  "status": "draft",
  "draft": { "items": [], "scan_notes": "string" },
  "image": {
    "format": "png",
    "bytes": 12345,
    "width": 640,
    "height": 480
  }
}
```

`image.width`/`image.height` report the **preprocessed** dimensions sent to
MiMo (e.g. QVGA at 2x becomes 640x480), so the demo can prove the upscale
happened. The gateway removes Markdown fences and validates the MiMo response
contract. The returned draft is not a confirmed record. FoodLoop must still
present ambiguity to the user and retain source/confidence information before
creating a household ledger entry.

## Two-Stage MiMo Prompting

Stage 1 (`TRANSCRIBE_PROMPT`): the model receives the image and must return
every visible character verbatim in reading order, preserving Chinese
characters, digits and punctuation, with `[unreadable]` for unclear parts and
no commentary.

Stage 2 (`STRUCTURE_PROMPT`): the model receives only that transcription
(no image) and must return the constrained draft JSON, using `null` for any
value the text does not support and quoting the exact OCR line as evidence.
A `date_type` of `expiry` vs `production` vs `best_before` must come from the
label text; a calendar expiry date is never inferred from a production date or
a vague shelf life.

If stage 1 returns empty text, the gateway falls back to the previous
single-shot image-to-draft prompt so the loop still completes.

## Draft-to-Record Confirmation Flow

The gateway response is a **draft**, never a confirmed record. FoodLoop on the
device enforces the confirmation boundary:

1. The gateway returns the draft and the device displays it on the LCD.
2. The device shows `DRAFT READY` and waits for the physical BOOT press
   (30 seconds).
3. A BOOT press confirms the draft: the device stores one line per record in
   `records.jsonl` and shows `RECORD SAVED`.
4. No BOOT press within 30 seconds discards the draft (`/tmp` is cleared) and
   the device returns to the idle state with the camera closed.

Record storage, in preference order:

| Path | Media | Persistence |
| --- | --- | --- |
| `/mnt/foodloop/records.jsonl` | microSD | survives reboot |
| `/data/foodloop-records.jsonl` | tmpfs | cleared on reboot (fallback) |

Each line is one JSON object:

```json
{
  "confirmed": true,
  "ts": 1755000000,
  "draft": {
    "items": [
      {
        "name": "牛奶",
        "category": "dairy",
        "expiry_date": "2026-08-15",
        "storage": "refrigerated",
        "confidence": 0.9,
        "needs_confirmation": true,
        "evidence": ["保质期至 2026-08-15"]
      }
    ],
    "scan_notes": "string"
  }
}
```

`needs_confirmation` stays `true` in the stored draft: the BOOT press confirms
the capture, while ambiguous dates still require a later explicit review (M3)
before a reminder is created. The device extracts the inner `draft` object
from the gateway envelope, so the ledger stores the food record itself, not
the HTTP wrapper.

## Security and Privacy

- Camera capture remains BOOT-triggered on the device.
- The gateway retains neither source image nor food response after returning
  the HTTP response.
- Token Plan credentials remain only in the existing local bridge process.
- The gateway forwards to the local bridge with its opaque board token and
  never logs that token.
