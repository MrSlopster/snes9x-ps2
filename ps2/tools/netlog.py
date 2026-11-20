#!/usr/bin/env python3
"""Receive the Snes9x PS2 network log (UDP, default port 18195).

Enable it on the PS2 with Settings > Network log. Lines are broadcast to
the local subnet unless ps2.cfg sets netlog_host. Every line is printed
with a timestamp and the sender's address; -o also appends to a file.

    python3 ps2/tools/netlog.py [-p PORT] [-o FILE]
"""
import argparse
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port", type=int, default=18195)
    ap.add_argument("-o", "--output", help="also append lines to this file")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    out = open(args.output, "a", buffering=1) if args.output else None
    print(f"listening on UDP :{args.port}", file=sys.stderr, flush=True)

    while True:
        data, (host, _port) = sock.recvfrom(2048)
        line = f"{time.strftime('%H:%M:%S')} {host} {data.decode('latin-1').rstrip()}"
        print(line, flush=True)
        if out:
            out.write(line + "\n")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
