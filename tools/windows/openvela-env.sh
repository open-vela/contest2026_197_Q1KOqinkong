#!/usr/bin/env bash

# Source this from MSYS2 before building or flashing openvela on Windows.
# This file lives in <contest-repo>/tools/windows; the workspace is one level
# above the contest repository.
_foodloop_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_foodloop_contest_root="$(cd "$_foodloop_script_dir/../.." && pwd)"
export OPENVELA_ROOT="$(cd "$_foodloop_contest_root/.." && pwd)"
export OPENVELA_XTENSA_BIN="$OPENVELA_ROOT/.toolchain/xtensa-esp32s3-elf/xtensa-esp32s3-elf/bin"
export OPENVELA_PYTHON_BIN="$OPENVELA_ROOT/.venv/Scripts"
export OPENVELA_TOOL_BIN="$OPENVELA_ROOT/.toolchain/bin"
export OPENVELA_KCONFIG_BIN="$OPENVELA_ROOT/prebuilts/kconfig-frontends/bin"
export OPENVELA_USER_BIN="$HOME/.local/bin"
export OPENVELA_GIT_LFS_BIN="/c/Program Files/Git/cmd"

export PATH="$OPENVELA_XTENSA_BIN:$OPENVELA_PYTHON_BIN:$OPENVELA_TOOL_BIN:$OPENVELA_KCONFIG_BIN:$OPENVELA_USER_BIN:$PATH:$OPENVELA_GIT_LFS_BIN"
export MSYS="winsymlinks:nativestrict"
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0="http.version"
export GIT_CONFIG_VALUE_0="HTTP/1.1"
export CCACHE_DISABLE=1

unset _foodloop_script_dir
unset _foodloop_contest_root
