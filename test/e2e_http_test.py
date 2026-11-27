#!/usr/bin/env python3
"""Host tests of test/e2e_http.py, and of the e2e scripts against a device
that isn't there or answers garbage: no board needed.

  python3 test/e2e_http_test.py [-v]

The scripts must report what broke as FAIL lines and exit non-zero, never
stop on a traceback (test/hil.py would then see "8 passed, exit 1" and
nothing naming the failure).
"""
import contextlib
import http.server
import io
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e_http  # noqa: E402


class Handler(http.server.BaseHTTPRequestHandler):
    """Answers from the server's `routes`: path -> (status, content type, body),
    or 'hang' (never answers), or 'close' (closes without a reply).
    Anything else gets its `default`."""

    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def answer(self):
        n = int(self.headers.get('Content-Length') or 0)
        if n:
            self.rfile.read(n)
        path = self.path[len('/api'):] if self.path.startswith('/api') else self.path
        route = self.server.routes.get(path, self.server.default)
        if route == 'hang':
            self.server.release.wait(10)
            self.close_connection = True
            return
        if route == 'close':
            self.close_connection = True
            return
        status, ctype, body = route
        self.send_response(status)
        if ctype:
            self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = answer


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, routes=None, default=(200, 'application/json', b'null')):
        super().__init__(('127.0.0.1', 0), Handler)
        self.routes = dict(routes or {})
        self.default = default
        self.release = threading.Event()
        threading.Thread(target=self.serve_forever, daemon=True).start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.server_address[1]}'

    def stop(self):
        self.release.set()
        self.shutdown()
        self.server_close()


class ArtNetServer(http.server.ThreadingHTTPServer):
    """A Tesser device for host runs of test/e2e_services.py's Art-Net checks:
    the HTTP surface the script reads, and an Art-Net node on port 6454 that
    maps universe 0 as examples/idf/full does (channel 1 dims /led/brightness,
    channel 2 at 128 or above switches /led/on) and answers Art-Poll.

    The script binds its own socket to every address on the Art-Net port;
    while this server holds the exact 127.0.0.1 binding, a reply addressed
    there is delivered to it rather than to that wildcard socket, so a poll
    reply goes out from an unbound socket just after letting go of the
    binding (which comes back for the next packet).
    """

    daemon_threads = True
    NAME = 'fake-tesser'

    def __init__(self):
        super().__init__(('127.0.0.1', 0), self.Handler)
        self.lock = threading.Lock()
        self.brightness = 128
        self.on = False
        self.packets = 0
        self.universe = 0
        self.last_frame = None
        self.stopped = False
        self.udp_failed = False
        self.http_thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.http_thread.start()
        self.udp_thread = threading.Thread(target=self._udp, daemon=True)
        self.udp_thread.start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.server_address[1]}'

    def stop(self):
        self.stopped = True
        self.shutdown()
        self.server_close()
        self.http_thread.join(1)
        self.udp_thread.join(1)

    # ---- Art-Net on 6454 --------------------------------------------------

    def _udp(self):
        while not self.stopped:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind(('127.0.0.1', 6454))
            except OSError:
                self.udp_failed = True
                return
            s.settimeout(0.2)
            while not self.stopped:
                try:
                    data, addr = s.recvfrom(640)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not self._handle(data, addr, s):
                    break  # a poll reply closed s; rebind below
            s.close()

    def _handle(self, data, addr, s):
        """One datagram. False when it closed `s` (a poll reply)."""
        if len(data) < 12 or data[:8] != b'Art-Net\0':
            return True
        op = struct.unpack('<H', data[8:10])[0]
        if op == 0x5000:  # Art-DMX
            if len(data) < 18:
                return True
            universe = data[14] | (data[15] & 0x7F) << 8
            n = min(data[16] << 8 | data[17], len(data) - 18, 512)
            with self.lock:
                self.packets += 1
                self.universe = universe
                self.last_frame = time.time()
                if universe == 0 and n >= 2:
                    self.brightness = data[18]
                    self.on = data[19] >= 128
            return True
        if op != 0x2000:  # Art-Poll
            return True
        s.close()
        reply = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            reply.sendto(self._poll_reply(), addr)
        finally:
            reply.close()
        return False

    @staticmethod
    def _poll_reply():
        r = bytearray(239)
        r[0:8] = b'Art-Net\0'
        r[8:10] = struct.pack('<H', 0x2100)
        r[10:14] = socket.inet_aton('127.0.0.1')
        for at in (26, 44):  # short and long name, NUL padded
            r[at:at + len(ArtNetServer.NAME)] = ArtNetServer.NAME.encode()
        return bytes(r)

    # ---- HTTP ---------------------------------------------------------------

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args):
            pass

        def answer(self):
            n = int(self.headers.get('Content-Length') or 0)
            if n:
                self.rfile.read(n)
            path = self.path[len('/api'):] if self.path.startswith('/api') else self.path
            dev = self.server
            status = 200
            with dev.lock:
                if path == '/sys/auth/open':
                    body = b'true'
                elif path == '/led':
                    body = json.dumps({'brightness': dev.brightness, 'on': dev.on}).encode()
                elif path == '/sys/artnet/status/packets':
                    body = str(dev.packets).encode()
                elif path == '/sys/artnet/status':
                    age = -1 if dev.last_frame is None else int(time.time() - dev.last_frame)
                    body = json.dumps({'packets': dev.packets, 'universe': dev.universe,
                                       'age': age, 'received': dev.last_frame is not None}).encode()
                elif path == '/sys/name':
                    body = b'""'
                elif path == '/sys/network/host':
                    body = json.dumps(ArtNetServer.NAME).encode()
                else:
                    body = b'{"error": "not_found"}'
                    status = 404
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = answer


class DyingServer(http.server.ThreadingHTTPServer):
    """Answers its very first request, then stops answering at all: a device
    that is already unreachable when test_sleep's checks run, with no
    /sys/sleep to have accepted them."""

    daemon_threads = True

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args):
            pass

        def answer(self):
            n = int(self.headers.get('Content-Length') or 0)
            if n:
                self.rfile.read(n)
            with self.server.lock:
                first = not self.server.answered
                self.server.answered = True
            if first:
                body = b'{"state": "online"}'
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.close_connection = True

        do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = answer

    def __init__(self):
        super().__init__(('127.0.0.1', 0), self.Handler)
        self.lock = threading.Lock()
        self.answered = False
        threading.Thread(target=self.serve_forever, daemon=True).start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.server_address[1]}'

    def stop(self):
        self.shutdown()
        self.server_close()


def dead_url():
    """An address where nothing listens: connections are refused at once."""
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return f'http://127.0.0.1:{port}'


GARBAGE = (200, 'application/json', b'<html>Internal error</html>')
EMPTY_ERROR = (500, '', b'')
JSON = (200, 'application/json', b'{"state": "online"}')


class Helpers(unittest.TestCase):
    def setUp(self):
        e2e_http.PASS = e2e_http.FAIL = 0
        self.out = io.StringIO()
        self.servers = []

    def tearDown(self):
        for s in self.servers:
            s.stop()

    def server(self, routes=None, default=GARBAGE):
        s = Server(routes, default)
        self.servers.append(s)
        return s

    def call(self, fn, *args, **kw):
        with contextlib.redirect_stdout(self.out):
            return fn(*args, **kw)

    def lines(self):
        return self.out.getvalue().splitlines()

    def assert_one_fail(self, method, path, error):
        lines = self.lines()
        self.assertEqual(len(lines), 1, lines)
        self.assertTrue(lines[0].startswith(f'FAIL http {method} {path}: '), lines[0])
        self.assertIn(error, lines[0])
        self.assertEqual(e2e_http.FAIL, 1)

    def test_dead_address(self):
        d = e2e_http.Device(dead_url())
        self.assertEqual(self.call(d.request, 'POST', '/sys/udp/send', {'path': '/led/on'}), (None, None))
        self.assert_one_fail('POST', '/sys/udp/send', 'ConnectionRefusedError')

    def test_dead_address_set(self):
        d = e2e_http.Device(dead_url())
        self.assertIsNone(self.call(d.set, '/led/on', True))
        self.assert_one_fail('POST', '/led/on', 'ConnectionRefusedError')

    def test_timeout(self):
        s = self.server({'/sys/status': 'hang'})
        d = e2e_http.Device(s.url)
        t = time.time()
        self.assertEqual(self.call(d.request, 'GET', '/sys/status', timeout=0.3), (None, None))
        self.assertLess(time.time() - t, 3)
        self.assert_one_fail('GET', '/sys/status', 'timed out')

    def test_closed_without_reply(self):
        d = e2e_http.Device(self.server({'/led': 'close'}).url)
        self.assertEqual(self.call(d.request, 'PATCH', '/led', {'on': True}), (None, None))
        self.assert_one_fail('PATCH', '/led', 'RemoteDisconnected')

    def test_garbage(self):
        d = e2e_http.Device(self.server().url)
        self.assertEqual(self.call(d.request, 'GET', '/sys/log'), (None, None))
        self.assert_one_fail('GET', '/sys/log', 'not JSON')
        self.assertIn('<html>Internal', self.lines()[0])

    def test_garbage_error(self):
        # e2e_power.py's crash: an HTTP error whose body isn't JSON.
        d = e2e_http.Device(self.server(default=(502, None, b'Bad gateway\n')).url)
        self.assertEqual(self.call(d.request, 'PATCH', '/sys/mqtt', {'enabled': True}), (None, None))
        self.assert_one_fail('PATCH', '/sys/mqtt', 'not JSON')

    def test_empty_error_is_a_status(self):
        d = e2e_http.Device(self.server(default=EMPTY_ERROR).url)
        self.assertEqual(self.call(d.request, 'POST', '/sys/restart'), (500, None))
        self.assertEqual(self.lines(), [])

    def test_json(self):
        s = self.server({'/sys/status': JSON, '/missing': (404, 'application/json', b'{"error":"not_found"}')})
        d = e2e_http.Device(s.url)
        self.assertEqual(self.call(d.request, 'GET', '/sys/status'), (200, {'state': 'online'}))
        self.assertEqual(self.call(d.request, 'GET', '/missing'), (404, {'error': 'not_found'}))
        self.assertEqual(self.call(d.value, '/sys/status'), {'state': 'online'})
        self.assertIsNone(self.call(d.value, '/missing'))
        self.assertEqual(self.lines(), [])
        self.assertEqual(e2e_http.FAIL, 0)

    def test_text_download(self):
        d = e2e_http.Device(self.server({'/sys/log/recent': (200, 'text/plain', b'W (1) x\n')}).url)
        self.assertEqual(self.call(d.request, 'GET', '/sys/log/recent'), (200, 'W (1) x\n'))
        self.assertEqual(self.lines(), [])

    def test_value_is_quiet(self):
        # Polling a device that is asleep or restarting is not a failure.
        for d in (e2e_http.Device(dead_url()), e2e_http.Device(self.server().url)):
            self.assertIsNone(self.call(d.value, '/sys/status/uptime'))
        self.assertEqual(self.lines(), [])
        self.assertEqual(e2e_http.FAIL, 0)

    def test_check(self):
        self.assertTrue(self.call(e2e_http.check, 'ok', True))
        self.assertFalse(self.call(e2e_http.check, 'broken', False, 42))
        self.assertEqual(self.lines(), ['PASS ok', 'FAIL broken: 42'])
        self.assertEqual((e2e_http.PASS, e2e_http.FAIL), (1, 1))


class Scripts(unittest.TestCase):
    """The scripts named in the bench run's failures, against fake devices."""

    def run_script(self, *args, timeout=120):
        p = subprocess.run([sys.executable, os.path.join(HERE, args[0])] + list(args[1:]),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
        return p.returncode, p.stdout

    def assert_failed_cleanly(self, rc, out, path):
        self.assertNotEqual(rc, 0, out)
        self.assertNotIn('Traceback', out)
        self.assertTrue(any(line.startswith('FAIL http ') and path in line for line in out.splitlines()), out)

    def test_udp_gateway_restarting(self):
        # The first device answers its status reads, then stops answering
        # writes (it is restarting): its sends time out.
        routes = {
            '/sys/network/host': (200, 'application/json', b'"a"'),
            '/sys/udp/status': (200, 'application/json', b'{"running": true, "failed": 0}'),
            '/peers?depth=1': (200, 'application/json', b'{"a": {}, "b": {}}'),
            '/peers/b/sys/status/state': (200, 'application/json', b'"online"'),
        }
        a = Server(routes, default='close')
        b = Server(dict(routes, **{'/sys/network/host': (200, 'application/json', b'"b"')}), default='close')
        try:
            rc, out = self.run_script('e2e_udp.py', a.url, b.url, timeout=300)
        finally:
            a.stop()
            b.stop()
        self.assert_failed_cleanly(rc, out, '/sys/udp/send')
        self.assertIn('DONE pass=', out)  # it went on to the end

    def test_services_artnet_url_with_port(self):
        # A URL with an explicit port: Device.host kept it, and the Art-Net
        # sends died on a gaierror before any check ran.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(('127.0.0.1', 6454))
        except OSError:
            self.skipTest('port 6454 is in use')
        finally:
            probe.close()
        s = ArtNetServer()
        try:
            rc, out = self.run_script('e2e_services.py', s.url, '--only', 'artnet')
        finally:
            s.stop()
        self.assertEqual(rc, 0, out)
        self.assertNotIn('Traceback', out)
        self.assertIn('DONE pass=8 fail=0', out)

    def test_device_ported_url(self):
        # A URL with an explicit port: Device.host kept it, so the WebSocket
        # dialed (host, 80) and wifi_status compared 'ip' against 'host:port'.
        routes = {
            '/sys/status/state': (200, 'application/json', b'"online"'),
            '/sys/network/wifi/status': (200, 'application/json', b'{"connected": true, "ip": "127.0.0.1"}'),
            '/sys/network/wifi/networks': (200, 'application/json', b'[]'),
        }
        s = Server(routes)
        try:
            rc, out = self.run_script('e2e_device.py', s.url, '--only', 'test_websocket,test_scan')
        finally:
            s.stop()
        self.assertNotIn('Traceback', out)
        self.assertIn('PASS wifi_status', out)  # st['ip'] == d.host, without the port
        self.assertIn('DONE pass=', out)  # it went on to the end

    def test_github_ported_url(self):
        # A URL with an explicit port: Device.host kept it, and local_ip()
        # died on a gaierror before any check ran.
        probe = socket.socket()
        try:
            probe.bind(('127.0.0.1', 8765))
        except OSError:
            self.skipTest('port 8765 is in use')
        finally:
            probe.close()
        routes = {
            '/sys/firmware': (200, 'application/json', b'{"version": "9.9.8", "github": {"repo": "leokeba/TesserKIT"}}'),
            '/sys/firmware/github': (200, 'application/json', b'{"status": "available", "latest": "v9.9.9"}'),
            '/sys/firmware/state': (200, 'application/json', b'"failed"'),
            '/sys/firmware/version': (200, 'application/json', b'"9.9.9"'),
        }
        s = Server(routes)
        with tempfile.TemporaryDirectory(prefix='e2e-github-') as tmp:
            image = os.path.join(tmp, 'fw.bin')
            with open(image, 'wb') as f:
                f.write(b'\x00' * 16)
            try:
                rc, out = self.run_script('e2e_github.py', s.url, '--image', image)
            finally:
                s.stop()
        self.assertNotIn('Traceback', out)
        self.assertIn('PASS release offered', out)  # it got past local_ip(dev.host)
        self.assertIn('passed,', out)  # it went on to the end

    def test_power_error_body(self):
        # e2e_power.py died on json.loads of an HTTP error's body.
        s = Server({'/sys/status': JSON, '/sys/log/recent': (502, 'text/html', b'')}, default=(502, None, b'oops'))
        try:
            rc, out = self.run_script('e2e_power.py', s.url, '--only', 'log')
        finally:
            s.stop()
        self.assert_failed_cleanly(rc, out, '/sys/log/level')
        self.assertIn('DONE pass=', out)

    def test_power_log_dropped_missing(self):
        # A device without /sys/log must not pass log_nothing_dropped:
        # (value or 0) == 0 was true when the read failed.
        s = Server({'/sys/status': JSON}, default=(404, 'application/json', b'{"error":"not_found"}'))
        try:
            rc, out = self.run_script('e2e_power.py', s.url, '--only', 'log')
        finally:
            s.stop()
        self.assertNotEqual(rc, 0, out)
        self.assertIn('DONE pass=', out)
        self.assertIn('FAIL log_nothing_dropped', out)
        self.assertNotIn('PASS log_nothing_dropped', out)

    def test_power_sleep_vacuous(self):
        # A device that stops answering after its first probe (and has no
        # /sys/sleep) must not pass sleep_unreachable -- uptime is None for
        # want of a reply, not because the device slept -- nor
        # sleep_long_enough, whose took >= 8 only because the 90 s wait_for
        # timed out after sleep_back failed.
        s = DyingServer()
        try:
            rc, out = self.run_script('e2e_power.py', s.url, '--only', 'sleep')
        finally:
            s.stop()
        self.assertNotEqual(rc, 0, out)
        self.assertIn('DONE pass=', out)  # it went on to the end
        self.assertIn('FAIL sleep_accepted', out)
        self.assertIn('FAIL sleep_unreachable', out)
        self.assertNotIn('PASS sleep_unreachable', out)
        self.assertIn('FAIL sleep_long_enough', out)
        self.assertNotIn('PASS sleep_long_enough', out)
        self.assertNotIn('PASS sleep_back', out)

    def test_power_sleep_never_left(self):
        # A device that keeps answering but has no /sys/sleep never went
        # away: sleep_back must not pass because it "came back" at once.
        s = Server({'/sys/status': JSON}, default=(404, 'application/json', b'{"error":"not_found"}'))
        try:
            rc, out = self.run_script('e2e_power.py', s.url, '--only', 'sleep')
        finally:
            s.stop()
        self.assertNotEqual(rc, 0, out)
        self.assertNotIn('Traceback', out)
        self.assertIn('DONE pass=', out)
        for name in ('sleep_unreachable', 'sleep_back', 'sleep_long_enough'):
            self.assertIn('FAIL ' + name, out)


class MqttFake(http.server.ThreadingHTTPServer):
    """A device's /sys/mqtt with mutable settings, for mqtt_begin/mqtt_end:
    GET answers the current settings (or fails when `read_ok` is false),
    PATCH merges its body into them."""

    daemon_threads = True

    def __init__(self, settings=None, read_ok=True):
        super().__init__(('127.0.0.1', 0), self.Handler)
        self.settings = dict(settings or {})
        self.read_ok = read_ok
        self.lock = threading.Lock()
        threading.Thread(target=self.serve_forever, daemon=True).start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.server_address[1]}'

    def stop(self):
        self.shutdown()
        self.server_close()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args):
            pass

        def answer(self):
            n = int(self.headers.get('Content-Length') or 0)
            body = self.rfile.read(n) if n else b''
            path = self.path[len('/api'):] if self.path.startswith('/api') else self.path
            dev = self.server
            with dev.lock:
                if path == '/sys/mqtt' and self.command == 'PATCH':
                    dev.settings.update(json.loads(body))
                    payload, status = b'{}', 200
                elif path == '/sys/mqtt' and not dev.read_ok:
                    payload, status = b'', 500
                else:
                    payload, status = json.dumps(dev.settings).encode(), 200
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = answer


class MqttCleanup(unittest.TestCase):
    """mqtt_begin/mqtt_end against a fake device: the test broker only runs
    during the check, so a run must leave MQTT off unless it found settings
    that point somewhere else."""

    URI = 'mqtt://127.0.0.1:1883'
    OURS = (URI,)

    def setUp(self):
        e2e_http.PASS = e2e_http.FAIL = 0
        self.out = io.StringIO()
        self.fakes = []

    def tearDown(self):
        for s in self.fakes:
            s.stop()

    def fake(self, **kw):
        s = MqttFake(**kw)
        self.fakes.append(s)
        return s

    def begin_end(self, s, extra=None, name='mqtt'):
        dev = e2e_http.Device(s.url)
        with contextlib.redirect_stdout(self.out):
            token = e2e_http.mqtt_begin(dev, self.URI, ours=self.OURS, extra=extra, name=name)
            e2e_http.mqtt_end(dev, token, name=name)

    def lines(self):
        return self.out.getvalue().splitlines()

    def test_already_pointed_at_broker_ends_off(self):
        # An interrupted run left it pointed at the test broker.
        s = self.fake(settings={'uri': self.URI, 'enabled': True, 'prefix': ''})
        self.begin_end(s)
        self.assertFalse(s.settings['enabled'], s.settings)
        self.assertEqual(e2e_http.FAIL, 0, self.lines())

    def test_failed_first_read_ends_off(self):
        # The first read fails: what it was before is unknown, so off.
        s = self.fake(settings={'uri': 'mqtt://elsewhere:1883', 'enabled': True}, read_ok=False)
        self.begin_end(s)
        self.assertFalse(s.settings['enabled'], s.settings)
        self.assertEqual(e2e_http.FAIL, 0, self.lines())

    def test_users_settings_are_put_back(self):
        found = {'uri': 'mqtt://mine.example:1883', 'enabled': True, 'prefix': 'home'}
        s = self.fake(settings=dict(found))
        self.begin_end(s)
        for k, v in found.items():
            self.assertEqual(s.settings.get(k), v, (k, s.settings))
        self.assertEqual(e2e_http.FAIL, 0, self.lines())

    def test_off_stays_off(self):
        # It was off: the run turned it on, so it goes back off.
        found = {'uri': 'mqtt://mine.example:1883', 'enabled': False}
        s = self.fake(settings=dict(found))
        self.begin_end(s)
        for k, v in found.items():
            self.assertEqual(s.settings.get(k), v, (k, s.settings))
        self.assertEqual(e2e_http.FAIL, 0, self.lines())

    def test_discovery_home_assistant_round_trip(self):
        # e2e_power's discovery: homeAssistant on during the check, back after.
        found = {'uri': 'mqtt://mine.example:1883', 'enabled': True, 'prefix': '', 'homeAssistant': False}
        s = self.fake(settings=dict(found))
        dev = e2e_http.Device(s.url)
        with contextlib.redirect_stdout(self.out):
            token = e2e_http.mqtt_begin(dev, self.URI, ours=self.OURS,
                                        extra={'homeAssistant': True}, name='discovery')
            self.assertTrue(s.settings['homeAssistant'], s.settings)
            e2e_http.mqtt_end(dev, token, name='discovery')
        for k, v in found.items():
            self.assertEqual(s.settings.get(k), v, (k, s.settings))
        self.assertEqual(e2e_http.FAIL, 0, self.lines())


if __name__ == '__main__':
    unittest.main()
