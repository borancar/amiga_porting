#!/usr/bin/env python3
"""Run a command and wait for it by its pidfd (pidfs), not by a pid or a pattern.

    python3 ami9000/pidrun.py [--timeout SECONDS] [--] COMMAND [ARGS...]

The child is started in its own process group; a pidfd for it is opened right after the start, so waiting, the timeout and the signals all name that very process
(a pid can be reused and `pgrep -f PATTERN` can match the shell that is looking for it).  On a timeout the group gets SIGTERM, and SIGKILL five seconds later.
The exit status of the command is the exit status of this program (124 on a timeout, as timeout(1) has it).
"""
import os
import select
import signal
import subprocess
import sys


def main() -> int:
    args = sys.argv[1:]
    timeout = None
    if args[:1] == ["--timeout"]:
        timeout, args = float(args[1]), args[2:]
    if args[:1] == ["--"]:
        args = args[1:]
    if not args:
        print(__doc__)
        return 2
    child = subprocess.Popen(args, start_new_session=True)
    pidfd = os.pidfd_open(child.pid)
    poller = select.poll()
    poller.register(pidfd, select.POLLIN)

    def exited(wait: float | None) -> bool:
        return bool(poller.poll(None if wait is None else int(wait * 1000)))

    try:
        if exited(timeout):
            return child.wait()
        signal.pidfd_send_signal(pidfd, signal.SIGTERM)   # (the group, through the leader: its children are signalled by the process group below)
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        if not exited(5):
            signal.pidfd_send_signal(pidfd, signal.SIGKILL)
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        child.wait()
        return 124
    finally:
        os.close(pidfd)


if __name__ == "__main__":
    raise SystemExit(main())
