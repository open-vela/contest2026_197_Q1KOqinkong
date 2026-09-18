# FoodLoop board validation checklist

This is a repeatable validation runbook. It distinguishes commands that can be
run from the results retained in the repository. Only the M1 result in
docs/m1-validation.md is confirmed by a saved hardware validation note.

## Preparation

- Reconnect the ESP32-S3-EYE and identify its serial port in Windows.
- Place the board and development computer on the same non-isolated 2.4 GHz
  network. Client or AP isolation prevents the optional gateway scan.
- Configure Wi-Fi through the included serial helper:

~~~powershell
.\contest2026_197_Q1KOqinkong\tools\windows\connect-foodloop-wifi.ps1 -Port COM7
~~~

## Build and flash

From an MSYS2 shell in the openvela workspace:

~~~bash
bash contest2026_197_Q1KOqinkong/tools/windows/configure-foodloop.sh
bash contest2026_197_Q1KOqinkong/tools/windows/build-foodloop.sh
esptool --chip esp32s3 --port COM7 --baud 460800 --before default-reset --after hard-reset write-flash 0x0 nuttx/nuttx.bin
~~~

The source workspace that produced the saved firmware needed the optional
Windows Kconfig-path patch under tools/patches. Apply it only if the current
MSYS configuration step fails for that path reason.

## M1 one-shot capture

Open the serial console and run:

~~~text
foodloop
~~~

Press BOOT once. The expected behavior is one 153600-byte QVGA RGB565 capture,
then return to the waiting state with the camera closed. The development-only
shortcut is:

~~~text
foodloop capture
~~~

## Local gateway confirmation flow

For a deterministic path without a MiMo call, start the mock gateway on the
development computer:

~~~powershell
python .\contest2026_197_Q1KOqinkong\tools\windows\mock-foodloop-gateway.py --port 8790
~~~

Then run foodloop scan with the computer IP and port 8790. Confirm the returned
draft with BOOT within 30 seconds. Check the microSD path
/mnt/foodloop/records.jsonl first, or the tmpfs fallback
/data/foodloop-records.jsonl when no microSD is mounted.

## Record status commands

~~~text
foodloop list
foodloop status 2026-08-15
~~~

The status command should classify confirmed records as EXPIRED, USE TODAY,
USE SOON, or OK. Items marked needs_confirmation are intentionally skipped.

## Food Guardian deployment

The configured Agent data directory is /tmp/foodloop-agent, so it is reset
after reboot. Re-deploy the Skill and daily cron definition with:

~~~powershell
.\.venv\Scripts\python.exe .\contest2026_197_Q1KOqinkong\tools\windows\deploy-food-guardian.py COM7
~~~

## Real MiMo path

The local bridge and gateway commands are documented in
tools/windows/README.md. A real MiMo vision run is not marked complete in the
current submission because the retained gateway log includes HTTP 502.
