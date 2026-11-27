"""What the e2e scripts share: PASS/FAIL lines and an HTTP client that fails a
check, instead of raising, when the device doesn't answer or garbles its reply.

test/hil.py counts the PASS and FAIL lines of each script and records its exit
status. A script that dies on a traceback leaves "8 passed, exit 1" with no
line saying what broke, and skips every check after it. So:

- request() never raises for the network or the reply: a timeout, a refused
  or reset connection, or a body that isn't JSON prints
  `FAIL http <METHOD> <path>: <error>` and returns (None, None), and the
  script goes on to its next check (which fails too if it needed the reply);
- value() is for polling a device that may be restarting or asleep: no reply,
  or a reply it can't read, is None, without a FAIL line (the check that
  waited for it fails if it matters);
- an exception the scripts don't handle still prints a FAIL line before the
  traceback, so hil.py's summary names it.

Each script ends with `sys.exit(1 if e2e.FAIL else 0)` (or returns it).
"""
import http.client
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

PASS = 0
FAIL = 0


def check(name, ok, detail=''):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f'PASS {name}')
    else:
        FAIL += 1
        print(f'FAIL {name}: {detail}')
    return ok


class Unreachable(Exception):
    """No reply: a timeout, a refused or reset connection, a cut response."""


class BadReply(Exception):
    """A reply that should be JSON and isn't."""


def describe(e):
    """`TimeoutError: timed out`, `ConnectionRefusedError: [Errno 111] ...`."""
    while isinstance(e, urllib.error.URLError) and not isinstance(e, urllib.error.HTTPError) \
            and isinstance(e.reason, BaseException):
        e = e.reason
    text = str(e)
    return f'{type(e).__name__}: {text}' if text else type(e).__name__


def fetch(url, method='GET', data=None, headers=None, timeout=10):
    """(status, headers, payload bytes) for any HTTP status; raises Unreachable."""
    req = urllib.request.Request(url, data=data, method=method, headers=dict(headers or {}))
    try:
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.headers, r.read()
        except urllib.error.HTTPError as e:
            with e:
                return e.code, e.headers, e.read()
    except (OSError, http.client.HTTPException) as e:  # URLError is an OSError
        raise Unreachable(describe(e)) from e


def parse(payload):
    """The JSON in `payload`, None when empty; raises BadReply."""
    if not payload:
        return None
    try:
        return json.loads(payload)
    except ValueError as e:  # JSONDecodeError, UnicodeDecodeError
        raise BadReply(f'not JSON ({e}): {bytes(payload[:80])!r}') from e


def decode(headers, payload):
    """parse(payload), or the text of a reply that says it isn't JSON (a
    text/plain download, say); raises BadReply."""
    ctype = headers.get('Content-Type', '') if headers else ''
    if payload and ctype and 'json' not in ctype:
        return payload.decode(errors='replace')
    return parse(payload)


def field(v, key):
    """v[key] when v is a dict with the key; None when the reply was missing
    or garbled (a string body, say) or the key isn't there: the check that
    used it fails, and the script goes on."""
    return v.get(key) if isinstance(v, dict) else None


def failed(method, path, error):
    check(f'http {method} {path}', False, error)


class Device:
    """A device's HTTP API at `base` (http://<ip>[:port]); paths are under /api.

    `host` is the address without the port, and `port` the one in `base`
    (80 or 443 by scheme when it has none): scripts that dial a socket to the
    device use (host, port), and a device reports its ip without the port.
    Bearer `token` when set. JSON bodies are sent as application/json; bytes
    are sent as they are.
    """

    token = None

    def __init__(self, base, timeout=10):
        self.base = base.rstrip('/')
        u = urllib.parse.urlparse(self.base)
        self.host = u.hostname
        self.port = u.port or (443 if u.scheme == 'https' else 80)
        self.timeout = timeout

    def headers(self):
        return {'Authorization': 'Bearer ' + self.token} if self.token else {}

    def exchange(self, method, path, body=None, timeout=None, headers=None):
        """(status, headers, payload bytes); raises Unreachable."""
        h = self.headers()
        data = None
        if body is not None:
            if isinstance(body, (bytes, bytearray)):
                data = bytes(body)
            else:
                data = json.dumps(body).encode()
                h['Content-Type'] = 'application/json'
        h.update(headers or {})
        return fetch(self.base + '/api' + path, method, data, h, timeout or self.timeout)

    def request(self, method, path, body=None, timeout=None, headers=None):
        """(status, JSON body); (None, None) after a FAIL line when there was
        no reply or it wasn't JSON (see decode())."""
        try:
            status, h, payload = self.exchange(method, path, body, timeout, headers)
            return status, decode(h, payload)
        except (Unreachable, BadReply) as e:
            failed(method, path, e)
            return None, None

    def value(self, path, timeout=5):
        """The body of a 200 reply to GET `path`, else None: for polling."""
        try:
            status, h, payload = self.exchange('GET', path, timeout=timeout)
            return decode(h, payload) if status == 200 else None
        except (Unreachable, BadReply):
            return None  # asleep, restarting, or not there yet

    def set(self, path, body=None, timeout=None):
        """The status of POST `path` (None when it failed)."""
        return self.request('POST', path, body, timeout)[0]


def mqtt_begin(dev, uri, ours=(), extra=None, name='mqtt'):
    """Point `dev`'s MQTT at `uri` (enabled, no prefix, plus `extra`) for an
    MQTT check, unless it already is. Returns a token for mqtt_end(): the
    settings found, to put them back; or None when they couldn't be read, or
    the device was left pointing at one of `ours` by an interrupted run: then
    mqtt_end() leaves MQTT off, since the broker only runs during the check
    and a board pointed at it reconnects every few seconds, logging resets.

    `ours` are the spellings of the test broker's uri (by IP and by host
    name), since the stored one may be either; every `extra` setting must
    already hold its value for the device to count as configured."""
    body = {'uri': uri, 'enabled': True, 'prefix': ''}
    body.update(extra or {})
    before = dev.value('/sys/mqtt')
    if not isinstance(before, dict):  # a failed or garbled read
        before = None
    if not (before and before.get('enabled') and before.get('uri') in ours and not before.get('prefix')
            and all(before.get(k) == v for k, v in (extra or {}).items())):
        check(f'{name}_configured', dev.request('PATCH', '/sys/mqtt', body)[0] == 200)
    if not before or (before.get('enabled') and before.get('uri') in ours):
        return None  # unreadable, or an interrupted run left it on here
    keys = ('uri', 'enabled', 'prefix') + tuple(extra or ())
    return {k: before[k] for k in keys if k in before}


def mqtt_end(dev, token, name='mqtt'):
    """Put back what mqtt_begin found; leave MQTT off when it returned None
    (the settings were unreadable, or the device was left pointing at the
    test broker by an interrupted run)."""
    if token is None:
        check(f'{name}_left_off', dev.request('PATCH', '/sys/mqtt', {'enabled': False})[0] == 200)
    else:
        check(f'{name}_settings_restored', dev.request('PATCH', '/sys/mqtt', token)[0] == 200)


def _report_crash(kind, value, tb):
    if not issubclass(kind, KeyboardInterrupt):
        global FAIL
        FAIL += 1
        print(f'FAIL {os.path.basename(sys.argv[0])}: stopped by {kind.__name__}: {value}', flush=True)
    _previous_hook(kind, value, tb)


_previous_hook = sys.excepthook
sys.excepthook = _report_crash
