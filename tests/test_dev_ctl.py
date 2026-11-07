# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests for dev_ctl.py; no SDK or console required.

A fake dev-control server answers one connection, the same way the
payload does: text commands end with OK:/ERR: lines, SHOT answers with
an "OK:SHOT <fmt> <bytes>" header followed by exactly that many bytes.
"""
import io
from pathlib import Path
import socket
import tempfile
import threading
import unittest

from host import dev_ctl


class FakeServer:
    """Serve one canned reply (bytes or a callable) on an ephemeral port."""

    def __init__(self, reply):
        self.reply = reply
        self.received = b''
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.thread.join(timeout=5)
        self.sock.close()
        return False

    def _serve(self):
        conn, _ = self.sock.accept()
        with conn:
            conn.settimeout(5)
            while not self.received.endswith(b'\n'):
                chunk = conn.recv(4096)
                if not chunk:
                    break
                self.received += chunk
            reply = self.reply(self.received) if callable(self.reply) else self.reply
            conn.sendall(reply)


def shot_reply(_received):
    payload = b'\x89PNG\r\n\x1a\n' + bytes(100)
    return b'OK:SHOT png %d\n' % len(payload) + payload


class DevCtlTests(unittest.TestCase):
    def test_ping_ok(self):
        with FakeServer(b'OK:PONG\n') as server:
            self.assertEqual(
                dev_ctl.run_text_command('127.0.0.1', server.port, 5, 'PING'),
                'OK:PONG\n')
            self.assertEqual(server.received, b'PING\n')

    def test_kill_text_reply(self):
        with FakeServer(b'OK:KILLED pid=123 count=1\n') as server:
            reply = dev_ctl.run_text_command('127.0.0.1', server.port, 5,
                                             'KILL:eboot.bin')
            self.assertIn('OK:KILLED', reply)
            self.assertEqual(server.received, b'KILL:eboot.bin\n')

    def test_list_reply_contains_ok_list(self):
        body = (b'proc pid=1 ppid=0 stat=2 nthr=3 comm=SceShellCore title=\n'
                b'OK:LIST n=1\n')
        with FakeServer(body) as server:
            reply = dev_ctl.run_text_command('127.0.0.1', server.port, 5, 'LIST')
            self.assertIn('OK:LIST n=1', reply)

    def test_shot_saves_png_bytes(self):
        with FakeServer(shot_reply) as server, tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'capture.png'
            self.assertEqual(dev_ctl.main(['--host', '127.0.0.1', '--port',
                                           str(server.port), 'shot',
                                           '-o', str(out)]), 0)
            data = out.read_bytes()
            self.assertEqual(len(data), 108)
            self.assertTrue(data.startswith(b'\x89PNG'))

    def test_shot_error_writes_nothing(self):
        with FakeServer(b'ERR:NO_SCREENSHOT\n') as server, tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'capture.png'
            self.assertEqual(dev_ctl.main(['--host', '127.0.0.1', '--port',
                                           str(server.port), 'shot',
                                           '-o', str(out)]), 1)
            self.assertFalse(out.exists())

    def test_shot_truncated_is_an_error(self):
        with FakeServer(b'OK:SHOT png 100\nshort') as server:
            with self.assertRaises(dev_ctl.DevControlError):
                dev_ctl.run_shot('127.0.0.1', server.port, 5)

    def test_shot_refuses_to_overwrite(self):
        with FakeServer(shot_reply) as server, tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'capture.png'
            out.write_bytes(b'old')
            with self.assertRaises(SystemExit):
                dev_ctl.main(['--host', '127.0.0.1', '--port',
                              str(server.port), 'shot', '-o', str(out)])
            self.assertEqual(out.read_bytes(), b'old')

    def test_kill_error_exit_code(self):
        with FakeServer(b'ERR:NOT_FOUND\n') as server:
            output = io.StringIO()
            import contextlib
            with contextlib.redirect_stdout(output):
                code = dev_ctl.main(['--host', '127.0.0.1', '--port',
                                     str(server.port), 'kill', 'nope.bin'])
            self.assertEqual(code, 1)
            self.assertIn('ERR:NOT_FOUND', output.getvalue())

    def test_unreachable_host_is_an_error(self):
        with self.assertRaises(dev_ctl.DevControlError):
            dev_ctl.run_text_command('127.0.0.1', 1, 1, 'PING')


if __name__ == '__main__':
    unittest.main()
