# FoodLoop Windows tools

These scripts are part of the FoodLoop contest submission. Run them from a
Windows checkout created with the repository manifest. They derive both the
contest repository and the surrounding openvela workspace from their own
location, so the checkout does not need to live at a fixed drive path.

## Build

Open an MSYS2 shell with the contest toolchain available, then run:

~~~bash
bash contest2026_197_Q1KOqinkong/tools/windows/configure-foodloop.sh
bash contest2026_197_Q1KOqinkong/tools/windows/build-foodloop.sh
~~~

The firmware output is nuttx/nuttx.bin. The Windows Kconfig compatibility
wrapper is included under kconfig-compat/.

## Local gateway

The real gateway keeps the MiMo Token Plan key in the local bridge process,
not in firmware or repository files:

~~~powershell
.\contest2026_197_Q1KOqinkong\tools\windows\start-mimo-tokenplan-bridge.ps1
.\contest2026_197_Q1KOqinkong\tools\windows\start-foodloop-home-gateway.ps1 -BoardIp <board-ip>
~~~

For a board-side confirmation-flow test without MiMo, use:

~~~powershell
python .\contest2026_197_Q1KOqinkong\tools\windows\mock-foodloop-gateway.py --port 8790
~~~

The gateway accepts only the configured board IP unless its explicit
local-test option is supplied.

## Board setup helpers

- connect-foodloop-wifi.ps1 configures a 2.4 GHz Wi-Fi connection through
  the serial console.
- deploy-food-guardian.py deploys the Food Guardian Skill and its daily
  cron configuration to the board runtime.
- run-foodloop-button-test.ps1 records a BOOT capture test to the system
  temporary directory rather than the repository.

These helpers prompt for credentials or obtain them from the local bridge
state; they do not store a real MiMo API key in this repository.
