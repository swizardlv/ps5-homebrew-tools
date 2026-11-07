#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Talk to the dev-control payload (TCP 9098) on a PS5 console.

Standard library only, like the other host tools in this repository.

    python3 host/dev_ctl.py --host $PS5_HOST ping
    python3 host/dev_ctl.py --host $PS5_HOST list
    python3 host/dev_ctl.py --host $PS5_HOST kill            # ACTIVE app
    python3 host/dev_ctl.py --host $PS5_HOST kill PPSA12345
    python3 host/dev_ctl.py --host $PS5_HOST kill eboot.bin
    python3 host/dev_ctl.py --host $PS5_HOST shot -o capture.png

Protocol (one command per connection, payload closes when done):
  PING / LIST / KILL:<target> reply with text lines ending in OK:.../ERR:...
  SHOT replies with one header line, "OK:SHOT <png|jpg> <bytes>", followed
  by exactly that many raw bytes; on failure it replies "ERR:..." instead.

SHOT streams the newest system screenshot file the console already has
(see payloads/dev-control/README.md). It is not a live framebuffer
capture: if the console has no screenshot file, the payload answers
ERR:NO_SCREENSHOT and this tool exits 1 without writing a file.
"""

import argparse
from pathlib import Path
import socket
import sys

DEFAULT_PORT = 9098


def port_number(value):
    result = int(value)
    if not 1 <= result <= 65535:
        raise argparse.ArgumentTypeError('port must be between 1 and 65535')
    return result


def positive_timeout(value):
    result = float(value)
    if not 0 < result < float('inf'):
        raise argparse.ArgumentTypeError('timeout must be positive and finite')
    return result


class DevControlError(Exception):
    pass


def _read_line(reader):
    line = reader.readline(4096)
    if not line:
        raise DevControlError('connection closed before a reply line')
    return line.decode('utf-8', errors='replace').strip()


def _read_exact(reader, size):
    data = reader.read(size)
    if len(data) != size:
        raise DevControlError(
            f'truncated transfer: received {len(data)}/{size} bytes')
    return data


def run_text_command(host, port, timeout, command):
    """Send one line command, return the full text reply."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.sendall(command.encode('ascii') + b'\n')
            chunks = []
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
    except OSError as error:
        raise DevControlError(f'cannot reach dev-control on {host}:{port}: '
                              f'{error}') from error
    return b''.join(chunks).decode('utf-8', errors='replace')


def run_shot(host, port, timeout):
    """Send SHOT, return (format, raw bytes). Raises DevControlError."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            reader = sock.makefile('rb')
            sock.sendall(b'SHOT\n')
            header = _read_line(reader)
            if header.startswith('ERR:'):
                raise DevControlError(f'PS5: {header}')
            parts = header.split()
            if len(parts) != 3 or parts[0] != 'OK:SHOT' or parts[1] not in ('png', 'jpg'):
                raise DevControlError(f'unexpected SHOT header: {header!r}')
            try:
                size = int(parts[2])
            except ValueError:
                raise DevControlError(f'unexpected SHOT header: {header!r}') from None
            if size <= 0:
                raise DevControlError(f'unexpected SHOT size: {header!r}')
            data = _read_exact(reader, size)
            return parts[1], data
    except OSError as error:
        raise DevControlError(f'cannot reach dev-control on {host}:{port}: '
                              f'{error}') from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, help='console address or hostname')
    parser.add_argument('--port', type=port_number, default=DEFAULT_PORT,
                        help='dev-control TCP port (default: 9098)')
    parser.add_argument('--timeout', type=positive_timeout, default=30,
                        help='seconds to wait for connect/reply (default: 30)')

    sub = parser.add_subparsers(dest='action', required=True)

    sub.add_parser('ping', help='check the daemon answers')
    sub.add_parser('list', help='list processes (pid, comm, title)')

    kill_p = sub.add_parser('kill', help='kill the active app or a target')
    kill_p.add_argument('target', nargs='?', default='ACTIVE',
                        help="'ACTIVE' (default), title id (PPSAxxxxx), "
                             'numeric pid, or process name (eboot.bin)')

    shot_p = sub.add_parser('shot', help='fetch the newest system screenshot')
    shot_p.add_argument('-o', '--out', type=Path, default=None,
                        help='output file; default capture.<png|jpg> in the '
                             'current directory')

    args = parser.parse_args(argv)

    try:
        if args.action == 'ping':
            reply = run_text_command(args.host, args.port, args.timeout, 'PING')
        elif args.action == 'list':
            reply = run_text_command(args.host, args.port, args.timeout, 'LIST')
        elif args.action == 'kill':
            reply = run_text_command(args.host, args.port, args.timeout,
                                     f'KILL:{args.target}')
        elif args.action == 'shot':
            if args.out and (args.out.exists() or args.out.is_symlink()):
                parser.error(f'output already exists: {args.out}')
            fmt, data = run_shot(args.host, args.port, args.timeout)
            out = args.out or Path(f'capture.{fmt}')
            if out.exists() or out.is_symlink():
                parser.error(f'output already exists: {out}')
            out.write_bytes(data)
            print(f'[+] Saved {len(data)} bytes ({fmt}) to {out}')
            return 0
    except DevControlError as error:
        print(f'dev_ctl: {error}', file=sys.stderr)
        return 1

    reply = reply.strip()
    print(f'[PS5] {reply}' if reply else '[PS5] (empty reply)')
    ok = reply.startswith('OK:') or 'OK:LIST' in reply
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
