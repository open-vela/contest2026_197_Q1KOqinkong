"""Deploy FoodLoop M3 runtime files to the board over serial.

Deploys two files onto the ESP32-S3-EYE's ai_agent runtime area:

  1. food_guardian.md  -> /tmp/foodloop-agent/skills/food_guardian.md
  2. cron.json         -> /tmp/foodloop-agent/cron.json

Both live under AGENT_DATA_DIR, which this firmware's defconfig sets to
`/tmp/foodloop-agent` (`CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR`).  `/tmp` is
tmpfs, so both files are cleared on every reboot and must be re-deployed
after each firmware flash.  This script is the "setup step" that does it.

Why segmented dd + cat instead of one big dd:
  - The console input path drops bursts larger than ~64 bytes, so payloads
    must be sent in small chunks with short pauses.
  - A single dd of the whole file (3.5 KB) is timing-sensitive and
    occasionally fails; writing segments of <= SEGMENT bytes (each verified
    by size) and then `cat`-ing them together is reliable.
  - NSH treats ' " and ` as quote separators, so `echo '<line>'` cannot
    carry the double quotes in these files; dd reads raw bytes from stdin.

Usage:
    python deploy-food-guardian.py [COM_PORT]

Prerequisites:
    - Board connected on the given COM port (default COM7).
    - NSH prompt available (the script waits for "nsh> " before writing).
"""

import json
import os
import sys
import tempfile
import time
from pathlib import Path

import serial

BAUD = 115200
CHUNK = 16        # bytes per serial write (console drops bursts > ~64)
CHUNK_GAP = 0.05  # seconds between chunks (0.03 drops bytes on this board)
SEGMENT = 600     # per-dd segment size in bytes (<=700 reliable)
MAX_ATTEMPTS = 3

# ai_agent data dir for this firmware (defconfig AI_AGENT_VELA_DATA_DIR)
AGENT_DIR = "/tmp/foodloop-agent"
SKILL_TARGET = AGENT_DIR + "/skills/food_guardian.md"
CRON_TARGET = AGENT_DIR + "/cron.json"

SKILL_PATH = Path(__file__).resolve().parents[2] / "app" / "foodloop" / \
    "skills" / "food_guardian.md"


def wait_prompt(ser, timeout=8.0):
    """Drain until the NSH prompt appears."""
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        buf += ser.read(4096)
        if b"nsh> " in buf:
            return True
        time.sleep(0.1)
    return False


def send_small(ser, data, chunk=CHUNK, gap=CHUNK_GAP):
    for i in range(0, len(data), chunk):
        ser.write(data[i:i + chunk])
        time.sleep(gap)


def cmd_small(ser, command, wait=2.0):
    """Send a command in small chunks; long commands (>~64B) get truncated
    if written in one burst, so always chunk them."""
    send_small(ser, command.encode("utf-8"))
    ser.write(b"\r")
    time.sleep(wait)
    return ser.read(8192).decode("utf-8", "replace")


def file_size(ser, path):
    """Return the board-side file size in bytes, or None if absent.

    NuttX `ls -l` output is `-rwxrwxrwx <size> <path>` (3 columns), unlike
    Linux which has owner/group columns between them.
    """
    out = cmd_small(ser, "ls -l " + path, 2.0)
    for line in out.splitlines():
        if line.strip().startswith("-"):
            parts = line.split()
            if len(parts) >= 3:
                try:
                    return int(parts[1])
                except ValueError:
                    return None
    return None


def deploy_file(ser, local_path, target_path):
    """Deploy one local file to the board via segmented dd + cat.

    Returns the deployed size on success, None on failure.
    """
    with open(local_path, "rb") as f:
        payload = f.read()

    target_dir = os.path.dirname(target_path)
    cmd_small(ser, "mkdir -p " + target_dir, 0.8)
    cmd_small(ser, "rm -f " + target_path, 0.8)

    # 1) Write segments to short paths (/tmp/segN) so the cat command stays
    #    well under the NSH command-line limit.
    segs = [payload[i:i + SEGMENT] for i in range(0, len(payload), SEGMENT)]
    for idx, seg in enumerate(segs):
        seg_path = "/tmp/seg%d" % idx
        cmd_small(ser, "rm -f " + seg_path, 0.5)
        ser.write(b"dd of=%s bs=%d count=1\r" % (seg_path.encode(), len(seg)))
        time.sleep(1.0)
        ser.read(8192)  # discard dd command echo
        send_small(ser, seg)
        if not wait_prompt(ser, 8):
            print("  segment %d: dd did not complete" % idx)
            return None
        if file_size(ser, seg_path) != len(seg):
            print("  segment %d: size mismatch" % idx)
            return None

    # 2) cat all segments into the target file.
    seg_list = " ".join("/tmp/seg%d" % i for i in range(len(segs)))
    cmd_small(ser, "cat %s > %s" % (seg_list, target_path), 3.0)

    # 3) Clean up segments.
    for i in range(len(segs)):
        cmd_small(ser, "rm -f /tmp/seg%d" % i, 0.4)

    return file_size(ser, target_path)


def cron_json_bytes():
    """The daily food-expiry check job, serialized as the cron.json payload."""
    cron = {
        "jobs": [{
            "id": "foodloop_daily",
            "name": "foodloop-daily-check",
            "enabled": True,
            "kind": "every",
            "interval_s": 86400,
            "message": "检查食物保质期：调用 foodloop status 并汇报 USE TODAY / USE SOON / EXPIRED 条目",
            "channel": "system",
            "chat_id": "cron",
            "delete_after_run": False,
            "last_run": 0,
            "next_run": 0,
        }]
    }
    return json.dumps(cron, ensure_ascii=False).encode("utf-8")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
    skill_path = str(SKILL_PATH)

    if not os.path.isfile(skill_path):
        raise SystemExit("skill not found: %s" % skill_path)

    ser = serial.Serial(port, BAUD, timeout=1)
    try:
        time.sleep(0.5)
        ser.reset_input_buffer()
        if not wait_prompt(ser):
            raise SystemExit("no NSH prompt on %s; is the board booted?" % port)

        # --- food_guardian.md skill ---
        with open(skill_path, "rb") as f:
            skill_bytes = f.read()
        print("Deploying skill (%d bytes) -> %s on %s"
              % (len(skill_bytes), SKILL_TARGET, port))
        ok = False
        for attempt in range(1, MAX_ATTEMPTS + 1):
            size = deploy_file(ser, skill_path, SKILL_TARGET)
            print("  attempt %d: skill size = %s (expected %d)"
                  % (attempt, size, len(skill_bytes)))
            if size == len(skill_bytes):
                ok = True
                break
            time.sleep(1.0)
        if not ok:
            raise SystemExit("FAILED to deploy skill after %d attempts"
                             % MAX_ATTEMPTS)
        print("OK: skill deployed (%d bytes)" % len(skill_bytes))

        # --- cron.json daily check ---
        cron_bytes = cron_json_bytes()
        print("Deploying cron.json (%d bytes) -> %s"
              % (len(cron_bytes), CRON_TARGET))
        cron_path = os.path.join(tempfile.gettempdir(), "foodloop-cron.json")
        with open(cron_path, "wb") as f:
            f.write(cron_bytes)
        try:
            ok = False
            for attempt in range(1, MAX_ATTEMPTS + 1):
                size = deploy_file(ser, cron_path, CRON_TARGET)
                print("  attempt %d: cron size = %s (expected %d)"
                      % (attempt, size, len(cron_bytes)))
                if size == len(cron_bytes):
                    ok = True
                    break
                time.sleep(1.0)
            if not ok:
                raise SystemExit("FAILED to deploy cron.json after %d attempts"
                                 % MAX_ATTEMPTS)
            print("OK: cron.json deployed (%d bytes)" % len(cron_bytes))
        finally:
            if os.path.isfile(cron_path):
                os.remove(cron_path)

        print("\nDone. Restart ai_agent to pick up the skill and cron job.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
