# FoodLoop submission status

This file distinguishes included implementation from demonstrated evidence so
that the contest repository can be reviewed without treating planned work or
mock data as a completed product claim.

## Included source

- Native ESP32-S3-EYE FoodLoop application with camera capture, LCD states,
  gateway upload, draft parsing, confirmation, JSONL storage, listing, and
  expiry-status commands.
- FoodLoop board configuration, Food Guardian Skill, and the deployment
  helper that writes the Skill and daily cron configuration to the configured
  Agent tmpfs directory.
- Trusted-local-network gateway, MiMo Token Plan bridge, deterministic mock
  gateway, and Windows build and serial helpers.
- QuickApp companion user-interface prototype that uses mock inventory data.

## Demonstrated evidence retained in the workspace

- The M1 note records a physical BOOT-button capture of one 320 by 240 RGB565
  frame, with a saved size of 153600 bytes and no persistent camera stream.
- The saved firmware build completed and emitted nuttx.bin, with compiler
  warnings.
- The mock gateway received two 153600-byte frames and returned HTTP 200
  draft responses.

## Not demonstrated as complete

- A successful production MiMo vision request. The retained real-gateway log
  contains a successful health probe followed by HTTP 502.
- A full hardware regression for M3 record status, daily Agent reminder, and
  post-reboot deployment.
- QuickApp synchronization, editing, deletion, and notification against
  board records.
- The demonstration video is an external competition deliverable and is not
  embedded in this source repository; it is supplied separately.

## AI Coding records

A genuine Codex Desktop rollout was recovered and converted into a selected
partial project-development excerpt at
`logs/Q1KO-Official/2026-08-28/codex__01a04916-0339-73e1-967f-2110701bd3db.jsonl`.
Its `manifest.json` records 576 events, the confirmed GitHub identity, the
source session id, redaction count, and a completeness warning. The official
log validator passes.

This is not a complete transcript: it omits non-development and later
recovery turns and does not reconstruct hidden encrypted reasoning. The old
contest template account and example JSONL were removed.

## External dependency disclosure

The surrounding openvela workspace contains local modifications outside this
contest repository. The FoodLoop contest source does not include them. Of
those changes, the Windows-specific NuttX configuration-path change used in
the original workspace is supplied as
tools/patches/nuttx-configure-windows.patch for transparent optional local
reproduction. The unrelated Wi-Fi diagnostic logging change is not included.
