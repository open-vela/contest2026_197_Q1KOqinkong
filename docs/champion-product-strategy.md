# FoodLoop Champion Product Strategy

## Product Statement

FoodLoop, also named "Shi Guang" (Food Time), is a **trusted household food
ledger**. It connects what a household owns, what should be used first, what a
person eats, and what to buy next. The ESP32-S3-EYE is the visible fridge-side
terminal, not a hidden surveillance camera.

The product promise is simple:

> A household should never need to guess what is already at home, what is
> about to expire, or why a nutrition total is unexpectedly high.

## The Competition-Worthy Closed Loop

The contest demo must show one reliable loop from hardware input to a useful
decision and a verifiable state change:

```text
Explicit package scan or weighed ingredient
  -> MiMo structured draft with uncertainty markers
  -> user confirmation
  -> trusted household ingredient ledger
  -> expiry-aware, preference-aware meal recommendation
  -> consumed quantity and nutrition record
  -> updated inventory, daily intake, and shopping list
```

This is stronger than a generic AI refrigerator or a calorie chatbot because
every recommendation is grounded in visible household records. The product
must say "why" it recommends a dish: which ingredients are present, which are
near expiry, and which health target it supports.

## Scope Decisions

### The first demonstrable product

1. **Fridge-side FoodLoop terminal**: a BOOT-triggered scan, local result
   state, and an explicit privacy indicator.
2. **Trusted ingredient ledger**: every item is a batch with source, amount,
   storage location, date confidence, and confirmation state.
3. **Meal decision**: MiMo produces one or more meal suggestions constrained
   by the confirmed ledger, allergies/dislikes, time, and a calorie target.
4. **Companion experience**: a phone-oriented QuickApp or browser view for
   review, shopping, daily intake, and meal confirmation.

### Not first-release scope

- Direct Xiaomi Health, Mi Home, or Xiaomi scale data synchronization cannot
  be claimed until a public API or an on-device protocol has been validated.
- Plate-photo calorie estimation cannot be presented as exact. FoodLoop should
  use a confirmed ingredient/portion record instead of inventing precision.
- Continuous fridge monitoring, background microphone capture, and automatic
  household-member identification are deliberately out of scope.

## Hardware Priorities

The refrigerator terminal remains fully functional using the contest board
alone. The JLC voucher should enhance evidence rather than create a dependency.

1. **First accessory: kitchen ingredient scale**. A load cell plus HX711 gives
   actual gram changes for inventory and portions, addressing the hardest
   credibility problem in food and calorie tracking.
2. **Second accessory: 3D-printed magnetic fridge stand**. It makes the board
   read as a product and places the BOOT control and camera at a predictable
   scan angle.
3. **Body weight is a supporting signal, not the core sensor**. It can later
   adjust a person's goal, but it cannot tell which food was eaten or how much
   inventory remains.

## Xiaomi, openvela, and MiMo Evidence

| Capability | Proof in FoodLoop |
| --- | --- |
| openvela | ESP32-S3-EYE board configuration, V4L2 camera path, button driver, local storage, Wi-Fi, native application, AI Agent, and QuickApp-compatible companion direction |
| Xiaomi MiMo | Vision/OCR-to-structured draft, ambiguity handling, grounded meal planning, and human-readable explanations tied to ledger records |
| Xiaomi ecosystem | A companion experience designed for the watch/phone workflow. Any direct Xiaomi device or platform connection is shown only after it works in the submitted build. |

MiMo must return constrained JSON and explain its evidence. A deterministic
validator rejects malformed dates, unsupported nutrition claims, and items
that were not confirmed. MiMo plans; FoodLoop's ledger decides what is true.

## Critical Gaps To Close

1. **Vision transport**: M1 emits RGB565. M2 must encode it for the selected
   vision endpoint and prove the selected MiMo model accepts that request.
2. **Device network path**: the current Windows local bridge is a development
   workaround for ESP32 TLS. It must either be replaced by direct TLS or
   formalized as a documented home gateway, never hidden in the final demo.
3. **User interface**: M1 has serial state only. The LCD needs a clear scan,
   review, success, and privacy-state flow before the final video.
4. **Data integrity**: expiry, quantity, and nutrition values need source,
   confidence, and correction history. This is the project's main
   differentiator.

## Final Demo Story

At the fridge, a person scans milk and vegetables with one BOOT press. MiMo
extracts a draft; the user confirms it on the companion. Before leaving work,
FoodLoop shows what is already at home and recommends a dinner that uses the
nearest-expiry ingredients, respects a declared dislike, and fits the
remaining daily calorie target. After confirming the meal, the ingredient
ledger, calorie record, and shopping list change together.

The demo should make it obvious that no camera was running until the user
pressed BOOT, and every AI conclusion is traceable to a household record.
