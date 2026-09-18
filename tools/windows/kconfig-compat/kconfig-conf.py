#!/usr/bin/env python3
"""Minimal Kconfig frontend wrapper for the Windows contest toolchain.

The bundled kconfig-conf.exe predates numeric Kconfig expressions used by the
current openvela tree. NuttX calls only the modes implemented below during
non-interactive builds.
"""

import os
import sys

from kconfiglib.kconfiglib import Kconfig


def fail(message):
    print(f"kconfig-conf wrapper: {message}", file=sys.stderr)
    return 2


def main(argv):
    if "--olddefconfig" in argv:
        mode = "olddefconfig"
    elif "--savedefconfig" in argv:
        mode = "savedefconfig"
    else:
        return fail(f"unsupported arguments: {' '.join(argv)}")

    kconfig_file = next((arg for arg in reversed(argv) if arg.endswith("Kconfig")), None)
    if not kconfig_file:
        return fail("missing Kconfig file")

    config_path = os.environ.get("KCONFIG_CONFIG", ".config")
    kconfig = Kconfig(kconfig_file)
    if os.path.exists(config_path):
        kconfig.load_config(config_path)

    if mode == "olddefconfig":
        kconfig.write_config(config_path)
        return 0

    output_index = argv.index("--savedefconfig") + 1
    if output_index >= len(argv):
        return fail("missing output path for --savedefconfig")
    kconfig.write_min_config(argv[output_index])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
