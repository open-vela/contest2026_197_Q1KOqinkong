#!/usr/bin/env bash
# Recreate the FoodLoop board configuration in the Windows/MSYS environment.

set -euo pipefail

# Start-Process does not always inherit MSYS's Unix tool path.
export PATH="/usr/local/bin:/usr/bin:/bin:$PATH"

script_dir="$(cd "$(dirname "$0")" && pwd)"
contest_root="$(cd "$script_dir/../.." && pwd)"
workspace_root="$(cd "$contest_root/.." && pwd)"
contest_name="$(basename "$contest_root")"

cd "$workspace_root"
source "$contest_root/tools/windows/openvela-env.sh"
export PATH="$contest_root/tools/windows/kconfig-compat:$PATH"
cd nuttx
tools/configure.sh "../$contest_name/board/foodloop/configs/foodloop"
echo CONFIGURE_FOODLOOP_OK
