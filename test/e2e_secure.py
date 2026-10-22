#!/usr/bin/env python3
"""End-to-end checks of TesserKIT's secure mode (DESIGN.md section 7) on a device.

Usage:
  python3 test/e2e_secure.py http://<ip>

The device runs examples/idf/full (any profile) and is open: no admin
account. The script adds one, which turns secure mode on, checks what that
changes, then removes it, so the device is open again when it ends (also when
a check fails).

- The HTTP server's strict mode: a set without a JSON body refused (415), a
  foreign Origin refused (403) and the device's own and the launcher's
  accepted, a Host that isn't the device's refused (421) and <name>.local or
  an address accepted.
- The access point's password: securePassword, readable by admins only.
- UDP's nodes at Public (peerAccess auto).
- A token revoked by logoutAll stops working at once.
- Back to open mode: none of the checks apply any more.
Every wait is bounded.
"""
import http.client
import json
import sys
import time
from urllib.parse import urlparse

PASS = 0
FAIL = 0
ACCOUNT = {'name': 'e2e-secure', 'password': 'e2e-secure-pass', 'role': 'admin'}


def check(name, ok, detail=''):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f'PASS {name}')
    else:
        FAIL += 1
        print(f'FAIL {name}{": " + str(detail) if detail else ""}')


class Device:
    def __init__(self, url):
        u = urlparse(url)
        self.ip, self.port = u.hostname, u.port or 80
        self.token = ''

    def request(self, method, path, body=None, headers=None, host=None):
        """(status, text), with the Host header set as given (the address by default)."""
        c = http.client.HTTPConnection(self.ip, self.port, timeout=8)
        c.putrequest(method, '/api' + path, skip_host=True)
        c.putheader('Host', host or self.ip)
        for k, v in (headers or {}).items():
            c.putheader(k, v)
        data = body.encode() if isinstance(body, str) else body
        if data is not None:
            c.putheader('Content-Length', str(len(data)))
        c.endheaders(data)
        r = c.getresponse()
        text = r.read().decode(errors='replace')
        c.close()
        return r.status, text

    def json(self, extra=None):
        h = {'Content-Type': 'application/json'}
        if self.token:
            h['Authorization'] = 'Bearer ' + self.token
        h.update(extra or {})
        return h

    def value(self, path, **kw):
        s, t = self.request('GET', path, headers=self.json(), **kw)
        return json.loads(t) if s == 200 and t else None

    def set(self, path, value, **kw):
        return self.request('POST', path, json.dumps(value), self.json(), **kw)[0]

    def wait(self, path, want, seconds=10):
        got = None
        for _ in range(seconds * 2):
            got = self.value(path)
            if got == want:
                return True
            time.sleep(0.5)
        print(f'  {path} is {got!r} after {seconds} s, not {want!r}')
        return False


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = Device(sys.argv[1])
    if d.value('/sys/auth/open') is not True:
        sys.exit('the device must be open (no admin account) to start')
    host = d.value('/sys/network/host') or ''
    try:
        check('an admin account added', d.set('/sys/auth/users', [ACCOUNT]) == 200)
        time.sleep(2)  # its password hashed on the background task
        s, t = d.request('POST', '/sys/auth/login', json.dumps({k: ACCOUNT[k] for k in ('name', 'password')}),
                         d.json())
        d.token = json.loads(t).get('token', '') if s == 200 else ''
        check('signed in', bool(d.token), t)
        check('secure mode: no longer open', d.wait('/sys/auth/open', False))
        time.sleep(1.5)  # the device's loop switches the strict mode on

        status, _ = d.request('POST', '/led/on', 'true',
                              {'Content-Type': 'text/plain', 'Authorization': 'Bearer ' + d.token})
        check('strict: a set without JSON refused (415)', status == 415, status)
        check('strict: a JSON set accepted', d.set('/led/on', True) == 200)
        status, _ = d.request('GET', '/sys/status/state', headers={'Origin': 'http://evil.example'})
        check('strict: a foreign Origin refused (403)', status == 403, status)
        for origin in ('https://leokeba.github.io', f'http://{d.ip}'):
            status, _ = d.request('GET', '/sys/status/state', headers={'Origin': origin})
            check(f'strict: Origin {origin} accepted', status == 200, status)
        status, _ = d.request('GET', '/sys/status/state', host='evil.example')
        check('strict: a rebinding Host refused (421)', status == 421, status)
        for h in (f'{host}.local', f'{d.ip}:{d.port}'):
            status, _ = d.request('GET', '/sys/status/state', host=h)
            check(f'strict: Host {h} accepted', status == 200, status)

        secret = d.value('/sys/network/ap/securePassword')
        check('access point: securePassword readable by the admin', isinstance(secret, str), secret)
        status, _ = d.request('GET', '/sys/network/ap/securePassword')
        check('access point: securePassword hidden from Public', status in (401, 403), status)
        check('UDP: peerAccess auto', d.value('/sys/udp/peerAccess') in ('auto', None))

        old = d.token
        check('logoutAll', d.set('/sys/auth/logoutAll', None) == 200)
        status, _ = d.request('GET', '/sys/auth/users', headers={'Authorization': 'Bearer ' + old})
        check('a revoked token stops working', status in (401, 403), status)
        s, t = d.request('POST', '/sys/auth/login', json.dumps({k: ACCOUNT[k] for k in ('name', 'password')}),
                         {'Content-Type': 'application/json'})
        d.token = json.loads(t).get('token', '') if s == 200 else ''
        check('signed in again', bool(d.token), t)
    finally:
        # Open again, whatever failed.
        removed = d.set('/sys/auth/users', []) == 200
        d.token = ''
        check('the account removed: open again', removed and d.wait('/sys/auth/open', True))
    time.sleep(1.5)
    status, _ = d.request('POST', '/led/on', 'false', {'Content-Type': 'text/plain'})
    check('open: a set without JSON accepted', status == 200, status)
    status, _ = d.request('GET', '/sys/status/state', host='evil.example')
    check('open: any Host accepted', status == 200, status)
    print(f'\n{PASS} passed, {FAIL} failed')
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
