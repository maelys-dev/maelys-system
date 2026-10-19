#!/usr/bin/env python3
"""Run a command with a deterministic wall-clock timeout.

The command runs in a session of its own, and when it ends, by itself or
at the timeout, every process still in that session is killed: a test that
fails half-way leaves no peer behind, whatever process group a peer chose,
a keeper of owned process groups included. A process that left the session
by setsid is the test's own to end.
"""

import os
import signal
import subprocess
import sys


def session_members(sid):
    if os.path.isdir("/proc"):
        pids = [int(name) for name in os.listdir("/proc") if name.isdigit()]
    else:
        listing = subprocess.run(["ps", "-axo", "pid="], capture_output=True, text=True)
        pids = [int(field) for field in listing.stdout.split()]
    me = os.getpid()
    for pid in pids:
        if pid == me:
            continue
        try:
            if os.getsid(pid) == sid:
                yield pid
        except OSError:
            pass


def sweep(sid):
    for pid in list(session_members(sid)):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def main():
    if len(sys.argv) < 3:
        return 2
    timeout = float(sys.argv[1])
    process = subprocess.Popen(sys.argv[2:], start_new_session=True)
    try:
        status = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        status = 124
    sweep(process.pid)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
