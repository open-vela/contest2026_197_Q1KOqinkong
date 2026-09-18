# FoodLoop Native App

The `foodloop` command is the privacy boundary for FoodLoop M1.

- It waits for the ESP32-S3-EYE BOOT button before opening the camera.
- One press captures exactly one QVGA RGB565 frame from `/dev/video0`.
- The frame is written to `/tmp/foodloop-last.rgb565` and is retained only
  until the device restarts or a subsequent scan replaces it.
- No microphone, continuous preview, or background camera capture is used.

Run `foodloop` on the board, then press BOOT to scan. `foodloop capture` is a
development-only shortcut for camera validation without a physical press.

M2 sends only an already captured frame to the documented local home gateway:

```text
foodloop scan <gateway-ip>
foodloop watch <gateway-ip>
```

The gateway returns a MiMo draft to `/tmp/foodloop-draft.json`; a later
confirmation flow is required before that draft becomes inventory.
