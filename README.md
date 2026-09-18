# FoodLoop

FoodLoop is an ESP32-S3-EYE food-storage assistant built on openvela. It uses
an explicit BOOT-button press to capture one food-package image, presents a
reviewable draft, and stores only user-confirmed food records locally.

This submission contains the native FoodLoop application, ESP32-S3-EYE
configuration, local Windows gateway tools, a Food Guardian Skill, a companion
QuickApp prototype, and project documentation. The validated hardware result
in this repository is the single-frame capture flow. The local gateway and
record-confirmation path are implemented and can be exercised with the
included mock gateway. A successful end-to-end production MiMo vision request
is not claimed by this submission.

## Direction

AI hardware product innovation. FoodLoop combines openvela camera, button,
LCD, file-system, Wi-Fi, and AI Agent capabilities with a local MiMo gateway
design. Its privacy boundary is deliberate:

- The camera stays closed while FoodLoop is idle.
- The BOOT button is the production capture trigger.
- One scan captures one 320 by 240 RGB565 frame.
- The temporary image is replaced by the next scan and is cleared on reboot.
- Cloud analysis is an explicit scan action, never a background upload.

## Current implementation status

| Area | Status |
| --- | --- |
| BOOT-triggered one-shot camera capture | Verified on the ESP32-S3-EYE |
| LCD capture and draft states | Implemented in the native application |
| RGB565 upload and local gateway protocol | Implemented |
| Gateway preprocessing and constrained draft parser | Implemented |
| Mock gateway confirmation flow | Exercised with HTTP 200 responses |
| MiMo Token Plan bridge | Implemented for a trusted local network |
| Real MiMo vision flow | Not confirmed end to end; the saved gateway log includes one HTTP 502 result |
| Local JSONL record storage and status command | Implemented |
| Food Guardian Skill and daily cron deployment helper | Implemented; deployment must be repeated after reboot because the configured Agent data directory is tmpfs |
| Companion QuickApp | UI prototype using mock inventory data |

## Architecture

~~~text
BOOT press
  -> one QVGA RGB565 image on ESP32-S3-EYE
  -> LCD preview
  -> optional local FoodLoop gateway
  -> RGB565 to PNG, contrast enhancement, two-stage draft request
  -> draft on LCD
  -> BOOT confirmation
  -> records.jsonl on microSD, with tmpfs fallback
  -> foodloop list and foodloop status
~~~

The draft contract preserves uncertainty. Items with needs_confirmation remain
excluded from expiry reminders until a later review resolves the ambiguity.

## Repository layout

- app/foodloop contains the native device application and Food Guardian Skill.
- board/foodloop contains the ESP32-S3-EYE FoodLoop configuration.
- tools/windows contains build, serial, Wi-Fi, local gateway, mock gateway,
  and Skill deployment helpers.
- tools/patches contains the optional Windows MSYS Kconfig path patch used in
  the original local workspace.
- quickapp/hello_quickapp contains a companion UI prototype with mock data.
- docs contains the protocol, M1 validation note, project plan, and submission
  status.

## Build and run

Use the repository manifest to obtain the surrounding openvela workspace and
open an MSYS2 shell with the contest toolchain. From the workspace root:

~~~bash
bash contest2026_197_Q1KOqinkong/tools/windows/configure-foodloop.sh
bash contest2026_197_Q1KOqinkong/tools/windows/build-foodloop.sh
~~~

The build produces nuttx/nuttx.bin. The source workspace used for this
submission had a Windows-only Kconfig path compatibility change in the common
NuttX configure script. The exact patch is included in
tools/patches/nuttx-configure-windows.patch for disclosure and optional local
reproduction; it is not silently applied by the contest scripts.

After flashing and opening the serial console, the main commands are:

~~~text
foodloop
foodloop capture
foodloop scan <gateway-ip>
foodloop list
foodloop status YYYY-MM-DD
foodloop demo
~~~

foodloop demo is a presentation-only loop. It cycles through the home,
capturing, analyzing, draft-ready and confirmed screens with a visible DEMO
badge. It does not open the camera, contact the gateway, or write records.

To exercise the local confirmation path without a MiMo request:

~~~powershell
python .\contest2026_197_Q1KOqinkong\tools\windows\mock-foodloop-gateway.py --port 8790
~~~

Then use foodloop scan with the host computer IP and port 8790. The real
gateway and bridge setup is documented in tools/windows/README.md.

## Validation evidence

- The M1 board validation recorded a physical BOOT press, a 153600-byte QVGA
  RGB565 frame, and return to an idle state with the camera closed. See
  docs/m1-validation.md.
- A saved firmware build produced nuttx.bin. The build emitted warnings, so
  this is evidence of a completed build rather than a clean warning-free
  build.
- The local mock gateway recorded two 153600-byte board requests and returned
  valid HTTP 200 draft responses.
- The real gateway was started and passed its health probe, but the retained
  record includes an HTTP 502 response from the MiMo-side path. This is an
  unresolved limitation, not a completed-cloud claim.

## Known limitations

- This repository does not include a video or screenshots; those are external
  deliverables still required by the competition submission process.
- Direct MiMo vision analysis has not been demonstrated successfully from the
  retained evidence.
- The QuickApp does not yet synchronize real records or mutate board storage.
- The Agent Skill and cron definition are deployed to tmpfs and must be
  re-deployed after a board reboot.

## AI Coding records

A genuine Codex Desktop rollout was recovered from the local session store and
converted into an explicitly marked partial project-development excerpt under
`logs/Q1KO-Official/`. The JSONL contains 576 validated events from eight
selected development turns, with the source session id, timestamps, visible
messages, tool calls, tool outputs, and a completeness warning in
`manifest.json`. The official log validator reports `ALL OK`.

This is not a complete session export: non-development and later recovery turns
are omitted, hidden encrypted reasoning is not reconstructed, and the old
contest template log was removed rather than submitted as evidence.

## License

This project is provided under the Apache License 2.0. See LICENSE.
