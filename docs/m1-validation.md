# FoodLoop M1 Validation

Date: 2026-07-20

## Scope

Validate the privacy boundary and one-shot camera path on the ESP32-S3-EYE.

## Firmware

- openvela ESP32-S3-EYE configuration: `board/foodloop/configs/foodloop`
- Native command: `foodloop`
- Camera device: `/dev/video0`
- Capture format: QVGA RGB565, 320 x 240 x 2 bytes

## Result

1. The board booted into NuttShell and initialized the OV2640 camera.
2. `foodloop capture` completed a development-only single-frame capture.
3. `foodloop` entered an idle state with the message that the camera remained
   closed until BOOT was pressed.
4. A physical BOOT press triggered one capture and produced:

   ```text
   FoodLoop: scanning one photo...
   OV2640 sensor configured for QVGA RGB565
   FoodLoop: saved 153600 bytes. Ready for MiMo analysis.
   ```

5. The application returned to its waiting state after the capture, rather
   than retaining a live camera stream.

## Next Validation

M2 will validate format conversion, constrained MiMo metadata extraction, user
review, and local record creation. It must preserve the same explicit-capture
boundary established here.
