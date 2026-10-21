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

Status 125 means this wrapper failed, never the command: a cleanup that
fails (a member still alive after the rounds, a signal refused for
another reason than a process already gone, the command's leader not
ended within the rounds after its SIGKILL), a command that cannot be
started, a wrong invocation, or any error of the wrapper itself. Each is
said on standard error, on lines that begin with "run-with-timeout:",
with the command's own status when it has one. A command that exits 125
by itself is taken for a failure of the wrapper too, which errs on the
side of a gate that stops. A command ended by a signal gives 128 plus its
number. SIGKILL of this wrapper cannot be caught and leaves the session
as it is. A process that left the session by setsid is the test's own to
end.
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


def sweep(sid, until):
    """Kills the session's live members, round after round until none is
    left or until passes; returns how many were killed and what went
    wrong, if anything."""
    killed = set()
    refused = {}
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
        problems.append("still alive after the rounds: %s" % " ".join(map(str, left)))
    for pid, code in sorted(refused.items()):
        problems.append("SIGKILL refused for %d: %s" % (pid, os.strerror(code)))
    return len(killed), problems


# --- the command ---------------------------------------------------------

# The leader's end is seen without reaping it. waitid with WNOWAIT does it
# where Python has it, which on macOS it does only from 3.13; elsewhere,
# the Python of the system included (3.9, the one a Homebrew build finds
# first), a kqueue watches the leader's exit.
_exit_watch = {}


def watch_leader(pid):
    if hasattr(os, "waitid"):
        return
    import select
    queue = select.kqueue()
    event = select.kevent(pid, filter=select.KQ_FILTER_PROC,
                          flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT, fflags=select.KQ_NOTE_EXIT)
    try:
        queue.control([event], 0, 0)
    except OSError:
        # ESRCH: ended already, unreaped
        queue.close()
        _exit_watch[pid] = [None, True]
        return
    _exit_watch[pid] = [queue, False]


def leader_ended(pid):
    if hasattr(os, "waitid"):
        info = os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT | os.WNOHANG)
        return info is not None and info.si_pid == pid
    watch = _exit_watch[pid]
    if not watch[1] and watch[0].control(None, 1, 0):
        watch[1] = True
    return watch[1]


def say(text):
    print("run-with-timeout: " + text, file=sys.stderr, flush=True)


def run(state):
    if len(sys.argv) < 3:
        say("usage: run-with-timeout.py SECONDS COMMAND [ARGUMENT...]")
        return CLEANUP_FAILED
    timeout = float(sys.argv[1])
    try:
        process = subprocess.Popen(sys.argv[2:], start_new_session=True)
    except OSError as error:
        say("the command cannot be started: %s" % error)
        return CLEANUP_FAILED
    leader = state["leader"] = process.pid
    watch_leader(leader)
    until = time.monotonic() + timeout
    timed_out = False
    while not interrupted and not leader_ended(leader):
        if time.monotonic() >= until:
            timed_out = True
            break
        time.sleep(0.01)
    if timed_out or interrupted:
        # the leader unreaped, its group's number is still its own. EPERM
        # is what macOS answers for a group of zombies alone; a live member
        # that refuses is the sweep's to find and report
        try:
            os.killpg(leader, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    # The other members are swept whether or not the leader has ended yet,
    # within one bound for both; the leader, unreaped, keeps the session's
    # number its own meanwhile.
    rounds_until = time.monotonic() + ROUNDS_FOR
    killed, problems = sweep(leader, rounds_until)
    while not leader_ended(leader) and time.monotonic() < rounds_until:
        time.sleep(0.01)
    if not leader_ended(leader):
        problems.append("the command's leader %d did not end within the rounds after its SIGKILL" % leader)
        status = None
    else:
        status = process.wait()
        if status < 0:
            status = 128 - status
    state["leader"] = None
    if killed:
        say("%d process(es) of the session left alive, killed" % killed)
    if problems:
        for problem in problems:
            say("cleanup failed: " + problem)
        say("the command's own status was %s" % ("unknown" if status is None else status))
        return CLEANUP_FAILED
    if interrupted:
        number = interrupted[0]
        signal.signal(number, signal.SIG_DFL)
        os.kill(os.getpid(), number)
        return 128 + number
    if timed_out:
        return 124
    return status


def main():
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, note_interrupt)
    state = {"leader": None}
    try:
        return run(state)
    except BaseException as error:
        # the wrapper's own failure: never the command's result
        import traceback
        for line in traceback.format_exception(type(error), error, error.__traceback__):
            for part in line.rstrip("\n").split("\n"):
                say("internal error: " + part)
        if state["leader"] is not None:
            # at best: the group, then one bounded sweep, each of which may
            # fail the same way
            try:
                os.killpg(state["leader"], signal.SIGKILL)
            except OSError:
                pass
            try:
                sweep(state["leader"], time.monotonic() + ROUNDS_FOR)
            except BaseException:
                say("the session could not be swept; members of it may remain")
        return CLEANUP_FAILED


if __name__ == "__main__":
    raise SystemExit(main())
