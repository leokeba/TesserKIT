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
import os
import socket
import subprocess
import sys
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

    def test_power_error_body(self):
        # e2e_power.py died on json.loads of an HTTP error's body.
        s = Server({'/sys/status': JSON, '/sys/log/recent': (502, 'text/html', b'')}, default=(502, None, b'oops'))
        try:
            rc, out = self.run_script('e2e_power.py', s.url, '--only', 'log')
        finally:
            s.stop()
        self.assert_failed_cleanly(rc, out, '/sys/log/level')
        self.assertIn('DONE pass=', out)


if __name__ == '__main__':
    unittest.main()
