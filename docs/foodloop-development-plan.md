# FoodLoop Development Plan

## Submission status note

This file records the implementation plan and intended milestones. It is not
proof that every listed milestone was demonstrated. The retained hardware
validation evidence in this repository confirms the M1 one-shot capture loop.
The M2 gateway, record-confirmation, and M3 reminder components are present in
source, but an end-to-end MiMo vision result and a complete M3 device
regression run are not claimed by the final submission snapshot.

## Product definition

FoodLoop is evolving from a food-storage helper into a trusted household food
ledger. The delivered contest product is intentionally narrower than the long
term vision: it proves the explicit-scan to confirmed-record to grounded meal
decision loop first. The product strategy and scope decisions live in
`docs/champion-product-strategy.md`.

FoodLoop is a privacy-first food storage assistant for students and small households. A person presses the ESP32-S3-EYE BOOT button to take one photo of a food package or ingredient. The device asks Xiaomi MiMo to extract a structured food record, stores that record locally, and later reminds the person before the food should be used.

The camera never records continuously. There is no background microphone collection. A picture leaves the device only after an explicit physical button press, and the local record can be deleted from the device at any time.

## Scope for the delivered board

The first version depends only on ESP32-S3-EYE capabilities:

| Capability | FoodLoop use |
| --- | --- |
| OV2640 camera | One-shot package or ingredient capture |
| BOOT button | Explicit capture confirmation and local actions |
| LCD | Capture state, recognition result, and reminder display |
| Wi-Fi | User-authorized MiMo image analysis and browser control page |
| MicroSD | Persistent food records, thumbnails, and custom Skill files |
| LED | Short local state feedback while capturing or sending |

No weighing sensor, temperature sensor, external microphone, or added PCB is required for the first usable version.

## Architecture

```text
BOOT button
    -> capture controller
    -> OV2640 one-shot frame
    -> local preview on LCD
    -> MiMo V2.5 vision request over Wi-Fi
    -> validated food record on microSD
    -> Food Guardian custom Skill
    -> LCD and local web result

Local schedule / ai_agent cron
    -> expiry or use-soon evaluation
    -> LCD reminder and browser notification
```

### Data ownership

- The original image is captured only after a button press.
- The device sends an image to MiMo only for the active recognition request.
- Food records are stored under `/mnt/foodloop/` on the microSD card, not in volatile agent storage.
- `/tmp/foodloop-agent/skills/food_guardian.md` holds the device Skill (this
  firmware sets `AGENT_DATA_DIR=/tmp/foodloop-agent`, so
  `AGENT_SKILLS_DIR = /tmp/foodloop-agent/skills/`). A setup step
  (`tools/windows/deploy-food-guardian.py`) copies it there after every
  firmware flash, since `/tmp` is tmpfs and is cleared on reboot.
- The local browser page can list, correct, export, and delete food records without a third-party account.

## Food record contract

MiMo must return JSON conforming to this shape. Unknown values remain `null`; the device must not invent expiry dates.

```json
{
  "name": "string",
  "category": "produce|dairy|meat|drink|snack|leftover|other",
  "quantity_hint": "string|null",
  "date_text": "string|null",
  "date_type": "expiry|production|best_before|unknown",
  "expiry_date": "YYYY-MM-DD|null",
  "storage": "room|refrigerated|frozen|unknown",
  "confidence": 0.0,
  "needs_confirmation": true
}
```

The user confirms ambiguous dates in the local page before FoodLoop creates a reminder. This is especially important for Chinese package labels where production and expiry dates can appear together.

## OpenVela and MiMo evidence

| Requirement | Planned implementation evidence |
| --- | --- |
| Actual openvela hardware | ESP32-S3-EYE running the contest `dev-ai-contest-2026` firmware |
| Input channel | BOOT-triggered capture plus local Wi-Fi browser page |
| Custom Skill | `food_guardian` Markdown Skill with structured recognition, confirmation, storage, and reminder rules |
| Proactive action | `ai_agent` cron checks confirmed local records and displays actionable use-soon reminders |
| AI capability | MiMo V2.5 vision-to-structured-data, with JSON validation and user confirmation |
| Multimedia / graphics | OV2640 camera pipeline and an LCD result state flow |

## Delivery milestones

### M0: board baseline

- Build and flash the official ESP32-S3-EYE openvela configuration.
- Verify Wi-Fi, LCD, BOOT button, LED, camera, and microSD individually.
- Record serial logs and a 30-second proof video.

### M1: private capture loop

- Implement BOOT press to capture exactly one frame.
- Display capture, upload, success, and error states on LCD.
- Save only user-approved images and records on microSD.

### M2: MiMo Food Guardian (implemented; real MiMo end-to-end validation pending)

- Validate the selected MiMo vision endpoint with a single explicit capture.
- Convert RGB565 to a supported image format and validate the JSON response.
- Provide confirmation for uncertain date, storage, quantity, and nutrition
  fields before creating a record.
- Implemented: two-stage gateway prompt (transcribe then structure), 2x
  bilinear upscale + contrast enhancement to compensate for the QVGA-only
  board driver, and a BOOT-confirmation flow that appends confirmed drafts to
  `records.jsonl` (SD first, `/data` tmpfs fallback). See
  `docs/m2-gateway-protocol.md`.

### M3: proactive reminders (partially implemented; full device validation pending)

- Persist confirmed records in `/mnt/foodloop/records.jsonl`. (Done:
  `foodloop_confirm_and_save_draft` + `foodloop_save_record`)
- Surface `use today`, `use soon`, and `expired` states on LCD and the local page.
  (Done on-device: `foodloop status [YYYY-MM-DD]` labels each confirmed item
  and shows an action list on the LCD; items with `needs_confirmation` are
  skipped so ambiguous dates never create reminders.)
- Register a periodic `ai_agent` cron job. (Done on-device: a daily
  `foodloop_daily` job with `kind=every, interval_s=86400` was written to
  `/tmp/foodloop-agent/cron.json`; ai_agent startup logs confirm
  `[cron] Loaded 1 cron jobs` + `Cron started (1 jobs, interval 10s)`.)
- Custom Skill: `app/foodloop/skills/food_guardian.md` implements the Food
  Guardian skill (structured recognition, confirmation boundary, cron-based
  reminders). Deploy it to `/tmp/foodloop-agent/skills/food_guardian.md` —
  that is `AGENT_SKILLS_DIR` for this firmware
  (`AGENT_DATA_DIR "/skills/"` with `CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR`
  = `/tmp/foodloop-agent`, not `/data/agent`). The skill instructs the agent
  to call `foodloop list` / `foodloop status`, to never remind on
  `needs_confirmation` items, and to use `cron_add` for daily or one-shot
  expiry checks. Note: `/tmp` is tmpfs, so the skill and cron.json are
  cleared on every reboot and must be re-deployed after each firmware flash
  (`tools/windows/deploy-food-guardian.py` handles the skill; cron.json is
  written the same way).

### M4: product completion

- Add a compact 3D-printed scanner stand using the JLC voucher.
- Produce repeatable flash/setup scripts, test data, screenshots, demo video, and AI Coding logs.
- Document privacy behavior and failure handling in the contest README.

### M5: grounded family decisions

- Add a companion view for household inventory, meal confirmation, shopping,
  and daily nutrition records.
- Add a grounded meal suggestion that cites confirmed ingredients, expiry
  state, dietary preferences, and the remaining calorie target.
- Prototype an optional kitchen scale accessory only after the board-only
  loop is reliable.

## Acceptance checklist for the first demo

1. Pressing BOOT captures one image and does not begin any continuous capture.
2. The LCD shows the request lifecycle without requiring a phone app.
3. A MiMo response creates a valid local draft record or a visible validation error.
4. An ambiguous date cannot create a reminder until the user confirms it.
5. A scheduled reminder remains available after reboot because records live on microSD.
6. Deleting a record from the local page removes its associated image and schedule.

## Later hardware objective

After the first version works with board-only capabilities, the JLC voucher can fund a non-critical accessory: a 3D-printed countertop stand and a small PCB with a large capture button, RGB status LED, and buzzer. The product must remain fully functional without this accessory, so hardware fabrication does not block the competition deliverable.
