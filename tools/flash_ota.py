#!/usr/bin/env python3
"""Safe launcher for the native strict RP2040 OTA utility.

The protocol implementation intentionally has a single authority
(`flash_ota.exe`).  This launcher preserves the previous Python entry point
without maintaining a second, potentially divergent USB/ESB state machine.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys


def main() -> int:
    tool_directory = pathlib.Path(__file__).resolve().parent
    executable = tool_directory / "flash_ota.exe"
    package = (
        pathlib.Path(sys.argv[1])
        if len(sys.argv) > 1
        else tool_directory.parent / "firmware" / "WirelessKeyboard_OTA.wkota"
    )

    if not executable.is_file():
        print(
            "flash_ota.exe is missing. Build it first with:\n"
            "  powershell -ExecutionPolicy Bypass "
            "-File tools\\build_flash_ota.ps1",
            file=sys.stderr,
        )
        return 2

    completed = subprocess.run([str(executable), str(package)], check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
