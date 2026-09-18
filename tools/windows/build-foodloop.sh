#!/usr/bin/env bash
# Build the FoodLoop ESP32-S3-EYE image from a Windows/MSYS shell.

set -euo pipefail

export PATH="/usr/local/bin:/usr/bin:/bin:$PATH"

script_dir="$(cd "$(dirname "$0")" && pwd)"
contest_root="$(cd "$script_dir/../.." && pwd)"
workspace_root="$(cd "$contest_root/.." && pwd)"

cd "$workspace_root"
source "$contest_root/tools/windows/openvela-env.sh"
export PATH="$contest_root/tools/windows/kconfig-compat:$PATH"

bash packages/ai_agent/fix_esp32s3.sh &
fix_pid=$!

set +e
make -C nuttx EXTRAFLAGS="-Wno-cpp -Wno-deprecated-declarations" -j2
build_status=$?
set -e

wait "$fix_pid" || true
exit "$build_status"
