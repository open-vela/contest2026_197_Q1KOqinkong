# FoodLoop local MiMo gateway notes

## Purpose

The ESP32-S3-EYE firmware in this project sends a captured RGB565 frame to a
trusted computer on the same local network. The computer-side gateway converts
the frame to PNG, performs optional contrast enhancement and two-stage
prompting, then uses a separate local bridge to contact the MiMo Token Plan
endpoint. This keeps the real MiMo key out of firmware, source files, logs,
and the board serial command history.

This is a development architecture, not evidence that the current board
firmware has a working direct TLS path to MiMo.

## Local services

Start the Token Plan bridge. It prompts for the key and writes a short-lived
local bridge state file under LOCALAPPDATA:

~~~powershell
.\contest2026_197_Q1KOqinkong\tools\windows\start-mimo-tokenplan-bridge.ps1
~~~

Start the FoodLoop gateway and permit only the board IP:

~~~powershell
.\contest2026_197_Q1KOqinkong\tools\windows\start-foodloop-home-gateway.ps1 -BoardIp <board-ip>
~~~

The device uses the gateway endpoint on port 8789:

~~~text
foodloop scan <computer-ip> 8789
~~~

For a deterministic board confirmation test that does not call MiMo:

~~~powershell
python .\contest2026_197_Q1KOqinkong\tools\windows\mock-foodloop-gateway.py --port 8790
~~~

Then use foodloop scan with port 8790.

## Protocol boundary

- The device uploads one explicitly captured frame only.
- The local gateway validates dimensions and content length before processing.
- The gateway allows only the configured board IP, unless the explicit
  local-test option is selected.
- The gateway keeps neither source image nor response after the HTTP reply.
- The bridge reads the real MiMo key only from its process environment.
- The board stores a draft only after an HTTP 200 response and saves a record
  only after the confirmation flow.

The detailed RGB565 request and JSON draft contract is in
docs/m2-gateway-protocol.md.

## Validation status

The mock gateway path returned HTTP 200 for captured-size requests. A retained
real-gateway attempt passed the health check but returned HTTP 502 during the
MiMo-side request. Therefore this repository does not present real MiMo vision
analysis as validated.
