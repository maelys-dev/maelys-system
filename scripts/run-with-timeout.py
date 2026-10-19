#!/usr/bin/env python3
"""Run a command with a deterministic wall-clock timeout.

The command runs in a session of its own. When it ends, by itself, at the
timeout (status 124) or because this wrapper received SIGINT, SIGTERM or
SIGHUP, every process still alive in that session is killed, round after
round, until none is left or ten seconds have passed: a test that fails
half-way leaves no peer behind, whatever process group a peer chose, a
keeper of owned process groups included.

What is signaled is that session and nothing else. The session's leader,
the command, is left unreaped until the end, so that the session's number
cannot be taken by a new session while the rounds run: a process whose
session is that number is one of the command's. A member is then pinned
before it is signaled. On Linux, by a pidfd opened before its session is
read and found alive after: the SIGKILL reaches that process and no
other. On macOS, which has no such handle, by its start time, read before
and after its session, just before the signal: a number reused in that
last interval would have to go round the whole pid space between two
system calls, which narrows the race without closing it.

A failed cleanup, a member still alive after the rounds or a signal
refused for another reason than a process already gone, is said on
standard error and gives status 125 whatever the command's, so that it is
never taken for the command's own result. SIGKILL of this wrapper cannot
be caught and leaves the session as it is. A process that left the
session by setsid is the test's own to end.
"""

import ctypes
import os
import signal
import subprocess
import sys
import time

CLEANUP_FAILED = 125
ROUNDS_FOR = 10.0

interrupted = []


def note_interrupt(signum, frame):
    interrupted.append(signum)


# --- what a process is -------------------------------------------------

if sys.platform == "darwin":
    _libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    _PROC_PIDTBSDINFO = 3
    _BSDINFO_SIZE = 136  # struct proc_bsdinfo
    _SZOMB = 5

    def all_pids():
        count = _libproc.proc_listallpids(None, 0)
        while True:
            room = max(count, 0) + 64
            buffer = (ctypes.c_int * room)()
            count = _libproc.proc_listallpids(buffer, ctypes.sizeof(buffer))
            if count < 0:
                raise OSError(ctypes.get_errno(), "proc_listallpids")
            if count < room:
                return [pid for pid in buffer[:count] if pid > 0]

    def identity(pid):
        """(start time, zombie) of a number in use, None when it is not."""
        buffer = ctypes.create_string_buffer(_BSDINFO_SIZE)
        got = _libproc.proc_pidinfo(pid, _PROC_PIDTBSDINFO, ctypes.c_uint64(0), buffer, _BSDINFO_SIZE)
        if got != _BSDINFO_SIZE:
            return None
        raw = buffer.raw
        status = int.from_bytes(raw[4:8], sys.byteorder)
        start = (int.from_bytes(raw[120:128], sys.byteorder), int.from_bytes(raw[128:136], sys.byteorder))
        return start, status == _SZOMB
else:
    def all_pids():
        return [int(name) for name in os.listdir("/proc") if name.isdigit()]

    def identity(pid):
        """(start time, zombie) of a number in use, None when it is not."""
        try:
            with open("/proc/%d/stat" % pid, "rb") as stat:
                fields = stat.read().rsplit(b")", 1)[1].split()
        except OSError:
            return None
        return int(fields[19]), fields[0] == b"Z"


def in_session(pid, sid):
    try:
        return os.getsid(pid) == sid
    except OSError:
        return False


def kill_pinned(pid, sid):
    """SIGKILL to pid if it is a live member of sid: 'killed', 'gone' when
    it is not, or the errno the signal was refused with."""
    if hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
        try:
            handle = os.pidfd_open(pid)
        except OSError:
            return "gone"
        try:
            seen = identity(pid)
            if seen is None or seen[1] or not in_session(pid, sid):
                return "gone"
            try:
                # still there after its session was read: what was read
                # was that process's, and the handle is that process
                signal.pidfd_send_signal(handle, 0)
                signal.pidfd_send_signal(handle, signal.SIGKILL)
            except ProcessLookupError:
                return "gone"
            except OSError as error:
                return error.errno
            return "killed"
        finally:
            os.close(handle)
    first = identity(pid)
    if first is None or first[1] or not in_session(pid, sid):
        return "gone"
    try:
        if identity(pid) != first:
            return "gone"
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        return "gone"
    except OSError as error:
        return error.errno
    return "killed"


def alive_members(sid):
    me = os.getpid()
    members = []
    for pid in all_pids():
        if pid in (me, sid) or not in_session(pid, sid):
            continue
        seen = identity(pid)
        if seen is not None and not seen[1]:
            members.append(pid)
    return members


def sweep(sid):
    """Kills the session's live members, round after round; returns how
    many were killed and what went wrong, if anything."""
    killed = set()
    refused = {}
    until = time.monotonic() + ROUNDS_FOR
    while True:
        members = alive_members(sid)
        if not members:
            break
        for pid in members:
            result = kill_pinned(pid, sid)
            if result == "killed":
                killed.add(pid)
            elif result != "gone":
                refused[pid] = result
        if time.monotonic() >= until:
            break
        time.sleep(0.01)
    problems = []
    left = alive_members(sid)
    if left:
        problems.append("still alive after %g s of rounds: %s" % (ROUNDS_FOR, " ".join(map(str, left))))
    for pid, code in sorted(refused.items()):
        problems.append("SIGKILL refused for %d: %s" % (pid, os.strerror(code)))
    return len(killed), problems


# --- the command ---------------------------------------------------------

def leader_ended(pid):
    info = os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT | os.WNOHANG)
    return info is not None and info.si_pid == pid


def main():
    if len(sys.argv) < 3:
        return 2
    timeout = float(sys.argv[1])
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, note_interrupt)
    process = subprocess.Popen(sys.argv[2:], start_new_session=True)
    leader = process.pid
    until = time.monotonic() + timeout
    timed_out = False
    while not interrupted and not leader_ended(leader):
        if time.monotonic() >= until:
            timed_out = True
            break
        time.sleep(0.01)
    if timed_out or interrupted:
        # the leader unreaped, its group's number is still its own
        try:
            os.killpg(leader, signal.SIGKILL)
        except ProcessLookupError:
            pass
        while not leader_ended(leader):
            time.sleep(0.01)
    killed, problems = sweep(leader)
    status = process.wait()
    if killed:
        print("run-with-timeout: %d process(es) of the session left alive, killed" % killed, file=sys.stderr)
    if problems:
        for problem in problems:
            print("run-with-timeout: cleanup failed: " + problem, file=sys.stderr)
        print("run-with-timeout: the command's own status was %d" % status, file=sys.stderr)
        return CLEANUP_FAILED
    if interrupted:
        number = interrupted[0]
        signal.signal(number, signal.SIG_DFL)
        os.kill(os.getpid(), number)
        return 128 + number
    if timed_out:
        return 124
    return status


if __name__ == "__main__":
    raise SystemExit(main())
