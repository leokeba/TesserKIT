#!/usr/bin/env python3
"""Host tests of test/hil.py's broker check: hil must not hand a suite a
--broker that doesn't answer: services/mqtt and power/discovery would fail
on reconnect loops, so hil prints a SKIP for them instead.
No board, no paho: the probe speaks MQTT CONNECT/CONNACK on a raw socket.

  python3 test/hil_test.py [-v]
"""
import contextlib
import io
import json
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hil  # noqa: E402


class TcpServer:
    """A threaded TCP server on a free port of 127.0.0.1; `handle(conn)` per
    connection. Subclasses set their own attributes before calling
    super().__init__ (a connection can't arrive before addr is read)."""

    def __init__(self, handle):
        self.handle = handle
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.stopped = False
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while not self.stopped:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.handle, args=(conn,), daemon=True).start()

    @property
    def addr(self):
        return f'127.0.0.1:{self.port}'

    def stop(self):
        self.stopped = True
        try:
            self.sock.close()
        except OSError:
            pass


class MqttBroker(TcpServer):
    """Answers an MQTT CONNECT with a CONNACK, like a broker does."""

    def __init__(self, return_code=0):
        self.return_code = return_code
        self.seen = []
        self.lock = threading.Lock()
        super().__init__(self._conn)

    def _conn(self, conn):
        try:
            data = conn.recv(256)
            with self.lock:
                self.seen.append(data)
            if data[:1] == b'\x10':  # a CONNECT
                conn.sendall(b'\x20\x02\x00' + bytes([self.return_code]))
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


class ResetServer(TcpServer):
    """Accepts and resets at once. A TCP connection alone would say it is
    reachable; only a CONNECT that gets no CONNACK sees through it."""

    def __init__(self):
        super().__init__(self._conn)

    def _conn(self, conn):
        try:
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        except OSError:
            pass
        try:
            conn.close()
        except OSError:
            pass


class HangServer(TcpServer):
    """Accepts and reads the CONNECT, then says nothing (no CONNACK)."""

    def __init__(self):
        super().__init__(self._conn)

    def _conn(self, conn):
        try:
            conn.recv(256)
            time.sleep(30)  # longer than any timeout the tests use
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def dead_port():
    """A port where nothing listens: connections are refused at once."""
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class BrokerProbe(unittest.TestCase):
    """hil.broker_reachable: a CONNECT that gets its CONNACK, nothing less."""

    def setUp(self):
        self.servers = []

    def tearDown(self):
        for s in self.servers:
            s.stop()

    def server(self, cls, *args):
        s = cls(*args)
        self.servers.append(s)
        return s

    def test_reachable_broker(self):
        b = self.server(MqttBroker)
        ok, why = hil.broker_reachable(b.addr)
        self.assertTrue(ok, why)
        self.assertEqual(why, '')
        end = time.time() + 2
        while not b.seen and time.time() < end:
            time.sleep(0.01)
        self.assertEqual(len(b.seen), 1, b.seen)
        c = b.seen[0]
        self.assertEqual(c[:1], b'\x10')           # a CONNECT
        self.assertEqual(c[2:8], b'\x00\x04MQTT')  # protocol name
        self.assertEqual(c[8], 4)                  # MQTT 3.1.1
        self.assertEqual(c[9] & 0x02, 0x02)        # clean session

    def test_refused_broker(self):
        # A broker that answers CONNACK but refuses (not anonymous here).
        b = self.server(MqttBroker, 4)
        ok, why = hil.broker_reachable(b.addr)
        self.assertFalse(ok)
        self.assertIn('refused', why)

    def test_nothing_listening(self):
        ok, why = hil.broker_reachable(f'127.0.0.1:{dead_port()}')
        self.assertFalse(ok)
        self.assertIn('ConnectionRefusedError', why)

    def test_accept_then_reset(self):
        # The TCP connection goes through, then the server resets it. Unreachable, whatever the reset looks like.
        r = self.server(ResetServer)
        ok, why = hil.broker_reachable(r.addr)
        self.assertFalse(ok, why)
        self.assertTrue(why)

    def test_no_conack_is_bounded(self):
        h = self.server(HangServer)
        t0 = time.time()
        ok, why = hil.broker_reachable(h.addr, timeout=1)
        self.assertFalse(ok)
        self.assertLess(time.time() - t0, 4)  # the timeout ends it, not a retry loop


class HilMain(unittest.TestCase):
    """hil.main()'s --broker decision, with every board step stubbed out."""

    def setUp(self):
        hil.FAILURES.clear()
        self.tmp = tempfile.TemporaryDirectory()
        self.suites = []
        self._saved = {}
        for name in ('suite', 'wait_up', 'check_heap', 'fetch_siblings'):
            self._saved[name] = getattr(hil, name)
        hil.suite = lambda name, cmd, boards, floors, work: self.suites.append((name, list(cmd)))
        hil.wait_up = lambda boards, settle: None
        hil.check_heap = lambda boards, floors, after: None
        hil.fetch_siblings = lambda work: {}

    def tearDown(self):
        for name, fn in self._saved.items():
            setattr(hil, name, fn)
        hil.FAILURES.clear()
        self.tmp.cleanup()

    def run_hil(self, broker):
        cfg = {
            'work': self.tmp.name,
            'boards': [{'name': 'B', 'role': 'gateway', 'mac': 'aa:bb:cc:dd:ee:ff',
                        'port': '/dev/null', 'url': 'http://127.0.0.1:9',
                        'hostname': 'tesserkit-eeff', 'build': 'full'}],
            'broker': broker,
        }
        path = os.path.join(self.tmp.name, 'boards.json')
        with open(path, 'w') as f:
            json.dump(cfg, f)
        out = io.StringIO()
        argv = sys.argv
        try:
            sys.argv = ['hil.py', path, '--skip-build', '--skip-flash']
            with contextlib.redirect_stdout(out):
                rc = hil.main()
        finally:
            sys.argv = argv
        return rc, out.getvalue()

    def test_broker_down_skips_mqtt_parts(self):
        broker = f'127.0.0.1:{dead_port()}'
        rc, out = self.run_hil(broker)
        by_name = {n: c for n, c in self.suites}
        self.assertEqual(rc, 0, out)  # a dead broker is not a firmware failure
        self.assertNotIn('--broker', by_name['services'])
        self.assertNotIn('--broker', by_name['power'])
        self.assertIn('SKIP services/mqtt:', out)
        self.assertIn('SKIP power/discovery:', out)
        self.assertIn(broker, out)  # the SKIP names the broker it probed

    def test_broker_up_passes_it_on(self):
        b = MqttBroker()
        self.servers = [b]
        try:
            rc, out = self.run_hil(b.addr)
        finally:
            b.stop()
        by_name = {n: c for n, c in self.suites}
        self.assertEqual(rc, 0, out)
        self.assertIn('--broker', by_name['services'])
        self.assertIn(b.addr, by_name['services'])
        self.assertIn('--broker', by_name['power'])
        self.assertNotIn('SKIP services/mqtt:', out)
        self.assertNotIn('SKIP power/discovery:', out)

    def test_no_broker_still_skips(self):
        rc, out = self.run_hil(None)
        by_name = {n: c for n, c in self.suites}
        self.assertEqual(rc, 0, out)
        self.assertNotIn('--broker', by_name['services'])
        self.assertIn('no "broker" in the boards file', out)


if __name__ == '__main__':
    unittest.main()
