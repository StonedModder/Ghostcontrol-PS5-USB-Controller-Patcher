#!/usr/bin/env python3
"""Send the companion core ELF (Ghost-toothAPI) to the console's ELF loader.

The standing operating order for this project: deploy the core FIRST, then
launch the app. The app is only a view over the running service; if the
service is not running it shows "Service not reachable" and waits.

Two things this script gets right that a plain `nc` does not:

  * It binds its socket to the PC's LAN address (192.168.69.23, the Ethernet
    interface). With ProtonVPN / Tailscale / other tunnel adapters installed,
    an unbound socket's packets to the console subnet get misrouted and the
    connection is refused or dropped in confusing ways. If your LAN address
    differs, pass --bind.
  * It reports a clean error when the loader is not listening, because that
    is the common case after a console reboot: start the console's ELF loader
    (elfldr / your loader, TCP 9021) first, then rerun.

Usage:
    deploy-core.py --file core/out/<ver>/ghostcontrol-core-<ver>.elf
                   [--host 192.168.69.8] [--port 9021] [--bind 192.168.69.23]
"""
from __future__ import annotations

import argparse
import socket
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.69.8")
    parser.add_argument("--port", type=int, default=9021)
    parser.add_argument("--bind", default="192.168.69.23",
                        help="local LAN address to bind (VPN misroute fix)")
    parser.add_argument("--file", required=True, type=Path)
    args = parser.parse_args(argv)

    if not args.file.is_file():
        raise SystemExit(f"not a file: {args.file}")
    payload = args.file.read_bytes()

    sock = socket.socket()
    sock.settimeout(20)
    try:
        if args.bind:
            sock.bind((args.bind, 0))
        sock.connect((args.host, args.port))
    except ConnectionRefusedError:
        raise SystemExit(
            f"refused by {args.host}:{args.port} — the console's ELF loader is "
            f"not running. Start it on the console, then rerun.")
    except Exception as error:  # noqa: BLE001 - report whatever happened
        raise SystemExit(f"cannot reach {args.host}:{args.port}: {error}")

    sock.sendall(payload)
    sock.shutdown(socket.SHUT_WR)
    sock.settimeout(5)
    try:
        ack = sock.recv(2048)
    except Exception:  # noqa: BLE001 - the loader usually does not ack
        ack = b""
    finally:
        sock.close()

    print(f"sent {len(payload)} bytes to {args.host}:{args.port}")
    print("loader ack:", ack[:200].decode("utf-8", "replace") if ack else "(none — the loader does not ack)")
    print("transfer complete; startup and controller output are NOT yet verified — read the console logs before launching the app")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
