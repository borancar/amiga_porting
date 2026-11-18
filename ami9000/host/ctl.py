#!/usr/bin/env python3
"""Send Lua to a running `ami9000_host ... --control PATH` and print the result.

    ctl.py PATH 'ami.frame()'          one line, or several lines as separate arguments
    echo 'ami.reg("pc")' | ctl.py PATH  lines from stdin

Each line runs in the script's own Lua state between frames, also while the machine is paused (see README).
Exit status 1 if a line raised an error."""
import socket
import sys


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise SystemExit("control socket closed")
        data += chunk
    return data


def reply(sock):
    head = b""
    while not head.endswith(b"\n"):
        head += recv_exact(sock, 1)
    status, length = head.split()
    return status == b"ok", recv_exact(sock, int(length)).decode(errors="replace")


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        raise SystemExit(__doc__)
    lines = sys.argv[2:] or [l.rstrip("\n") for l in sys.stdin]
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(sys.argv[1])
    status = 0
    for line in lines:
        sock.sendall(line.replace("\n", " ").encode() + b"\n")
        ok, text = reply(sock)
        if text:
            print(text, file=sys.stdout if ok else sys.stderr)
        status |= not ok
    return status


sys.exit(main())
