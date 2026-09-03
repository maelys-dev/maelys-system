#!/usr/bin/env python3
"""Run a command with a deterministic wall-clock timeout."""

import os
import signal
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        return 2
    timeout = float(sys.argv[1])
    process = subprocess.Popen(sys.argv[2:], start_new_session=True)
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        return 124


if __name__ == "__main__":
    raise SystemExit(main())
