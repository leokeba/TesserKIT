#!/usr/bin/env python3
"""End-to-end checks of a TesserKIT device over HTTP and WebSocket.

Usage:
  python3 test/e2e_device.py http://<ip> [--image app.bin] [--crash-image crash.bin] [--only test_a,test_b] [--app /led]

The device runs examples/idf/minimal (or anything with the same services and
an application value at /led/brightness). It restarts several times. With
--image, the image is uploaded as a firmware update, confirmed, then rolled
back. With --crash-image, an image that crashes at boot is uploaded, and the
bootloader must roll back to the running one.
"""
import base64
import json
import os
import random
import socket
import struct
import sys
import time
import urllib.error
import urllib.request

PASS = 0
FAIL = 0
APP = '/led'  # the application's object, with `on` and `brightness`


def check(name, ok, detail=''):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f'PASS {name}')
    else:
        FAIL += 1
        print(f'FAIL {name}: {detail}')
    return ok


class Device:
    def __init__(self, base):
        self.base = base.rstrip('/')
        self.host = self.base.split('://', 1)[1].split('/')[0]

    token = None  # sent as Authorization: Bearer when set

    def request(self, method, path, body=None, raw=False, headers=None, timeout=20):
        headers = dict(headers or {})
        if self.token:
            headers['Authorization'] = 'Bearer ' + self.token
        data = None
        if body is not None:
            data = body if isinstance(body, (bytes, bytearray)) else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                payload = r.read()
                return r.status, r.headers, payload if raw else (json.loads(payload) if payload else None)
        except urllib.error.HTTPError as e:
            payload = e.read()
            try:
                return e.code, e.headers, payload if raw else json.loads(payload)
            except ValueError:
                return e.code, e.headers, payload

    def get(self, path, **kw):
        return self.request('GET', '/api' + path, **kw)

    def set(self, path, body=None, **kw):
        return self.request('POST', '/api' + path, body, **kw)

    def value(self, path):
        try:
            status, _, body = self.get(path, timeout=5)
        except (OSError, urllib.error.URLError):
            return None  # restarting
        return body if status == 200 else None

    def wait_restart(self, timeout=40):
        """Waits for the device to go away, then to answer again."""
        start = time.time()
        up_since = self.uptime()
        while time.time() - start < timeout:
            u = self.uptime()
            if u is not None and up_since is not None and u < up_since:
                return True
            if u is None:
                break
            time.sleep(0.5)
        while time.time() - start < timeout:
            if self.uptime() is not None:
                return True
            time.sleep(1)
        return False

    def uptime(self):
        try:
            status, _, body = self.get('/sys/status/uptime', timeout=3)
            return body if status == 200 else None
        except (OSError, urllib.error.URLError):
            return None

    def wait_online(self, timeout=60):
        start = time.time()
        while time.time() - start < timeout:
            try:
                if self.value('/sys/status/state') == 'online':
                    return True
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(1)
        return False


class WebSocket:
    """Just enough of RFC 6455 for text messages."""

    def __init__(self, host, path='/ws', port=80, timeout=5):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f'GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                           f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        head = b''
        while b'\r\n\r\n' not in head:
            chunk = self.sock.recv(1)
            if not chunk:
                raise OSError('handshake closed')
            head += chunk
        if b' 101 ' not in head.split(b'\r\n', 1)[0]:
            raise OSError(head.split(b'\r\n', 1)[0].decode())

    def send(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        header = bytes([0x81])
        n = len(data)
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack('>H', n)
        else:
            header += bytes([0x80 | 127]) + struct.pack('>Q', n)
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n):
        out = b''
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise OSError('closed')
            out += chunk
        return out

    def recv(self, timeout=5):
        self.sock.settimeout(timeout)
        message = b''
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack('>H', self._read(2))[0]
            elif n == 127:
                n = struct.unpack('>Q', self._read(8))[0]
            payload = self._read(n)
            opcode = b0 & 0x0F
            if opcode == 0x9:  # ping
                continue
            message += payload
            if b0 & 0x80:
                return json.loads(message)

    def close(self):
        self.sock.close()


def test_status(d):
    status, _, body = d.get('/sys/status')
    check('status', status == 200 and body['state'] == 'online' and body['chip'].startswith('ESP32'), body)
    check('status_mac', len(body.get('mac', '')) == 17, body)
    h = d.value('/?view=hash')
    check('schema_hash', isinstance(h, str) and len(h) == 8, h)
    status, _, schema = d.get('/sys?view=schema')
    check('sys_schema', status == 200 and schema.get('ui', {}).get('page') is True, str(schema)[:200])


def test_ui(d):
    status, _, page = d.request('GET', '/', raw=True)
    check('ui_page', status == 200 and b'TESSER' in page, page[:120])
    status, headers, js = d.request('GET', '/ui.js', raw=True, headers={'Accept-Encoding': 'gzip'})
    etag = headers.get('ETag')
    check('ui_bundle', status == 200 and headers.get('Content-Encoding') == 'gzip' and len(js) > 20000 and etag,
          (status, dict(headers)))
    status, _, _ = d.request('GET', '/ui.js', raw=True, headers={'If-None-Match': etag or '', 'Accept-Encoding': 'gzip'})
    check('ui_bundle_cached', status == 304, status)
    status, headers, _ = d.request('GET', '/generate_204', raw=True)
    check('no_portal_when_joined', status == 404, status)


def test_websocket(d):
    try:
        ws = WebSocket(d.host)
        ws.send({'id': 1, 'op': 'sub', 'path': '/sys/status', 'keys': 'uptime,heap', 'interval': 500})
        snap = ws.recv()
        check('ws_snapshot', snap.get('id') == 1 and snap.get('status') == 'ok' and 'uptime' in snap['body'], snap)
        change = None
        deadline = time.time() + 4
        while time.time() < deadline:
            m = ws.recv(timeout=4)
            if m.get('op') == 'change':
                change = m
                break
        check('ws_change', change is not None and change['path'] == '/sys/status', change)
        ws.close()
    except OSError as e:
        check('ws', False, e)


def test_scan(d):
    status, _, body = d.set('/sys/network/wifi/scan', timeout=20)
    check('scan', status == 200 and isinstance(body, list) and body and {'ssid', 'rssi', 'channel', 'secure'} <= set(body[0]),
          (status, body))
    st = d.value('/sys/network/wifi/status')
    check('wifi_status', st and st['connected'] and st['ip'] == d.host, st)
    check('wifi_password_hidden', all(n.get('password') is None for n in d.value('/sys/network/wifi/networks')))


def test_time(d):
    deadline = time.time() + 40
    while time.time() < deadline and d.value('/sys/time/synced') is not True:
        time.sleep(1)
    t = d.value('/sys/time')
    check('time_synced', t['synced'] and t['source'] == 'sntp' and t['epoch'] > 1.7e9, t)
    check('time_now_local', abs(t['epoch'] - time.time()) < 30, (t['epoch'], time.time()))


def test_persistence(d):
    level = random.randint(1, 250)
    old_tz = d.value('/sys/time/tz')
    check('set_value', d.set(APP + '/brightness', level)[0] == 200)
    check('set_setting', d.set('/sys/time/tz', 'EST5EDT')[0] == 200)
    time.sleep(3)  # the debounce
    check('restart', d.set('/sys/restart')[0] == 200)
    check('restart_back', d.wait_restart() and d.wait_online())
    check('persisted_value', d.value(APP + '/brightness') == level, d.value(APP + '/brightness'))
    check('persisted_setting', d.value('/sys/time/tz') == 'EST5EDT')
    check('reset_reason', d.value('/sys/status/resetReason') == 'software', d.value('/sys/status/resetReason'))
    d.set('/sys/time/tz', old_tz)


def test_backup(d):
    status, headers, raw = d.get('/sys/backup/settings', raw=True)
    check('backup_download', status == 200 and ('"' + APP[1:] + '"').encode() in raw, raw[:200])
    disposition = headers.get('Content-Disposition', '')
    check('backup_filename', '-settings.json' in disposition, disposition)
    backup = json.loads(raw)
    check('backup_has_secrets', any(n.get('password') for n in backup['sys']['network']['wifi']['networks']))
    d.set('/sys/backup/restartAfter', False)
    backup[APP[1:]]['brightness'] = 42
    backup[APP[1:]]['gone'] = 1
    status, _, body = d.request('PUT', '/api/sys/backup/settings', json.dumps(backup).encode())
    check('restore', status == 200 and body == {'skipped': [APP + '/gone'], 'restart': False}, (status, body))
    check('restored_value', d.value(APP + '/brightness') == 42)
    status, _, body = d.request('PUT', '/api/sys/backup/settings', b'not json')
    check('restore_garbage', status == 400, (status, body))
    d.set('/sys/backup/restartAfter', True)


def test_bad_image(d):
    status, _, body = d.request('PUT', '/api/sys/firmware/image', os.urandom(4096))
    check('bad_image', status == 422 and body.get('message') == 'not an app image', (status, body))
    fw = d.value('/sys/firmware')
    check('bad_image_state', fw['state'] == 'failed' and fw['error'] == 'not an app image', fw)
    check('bad_image_running', d.value('/sys/status/state') == 'online')


def test_coredump(d):
    present = d.value('/sys/coredumpPresent')
    check('coredump_present_known', present in (True, False), present)
    if present is False:
        status, _, _ = d.get('/sys/coredump', raw=True)
        check('coredump_none', status == 404, status)


def test_auth(d):
    check('auth_open', d.value('/sys/auth/open') is True)
    status, _, body = d.request('PUT', '/api/sys/auth/users', [{'name': 'e2e', 'password': 'e2e-pass', 'role': 'admin'}])
    check('auth_add_admin', status == 200, (status, body))
    check('auth_closed', d.value('/sys/auth/open') is False)
    status, _, _ = d.set('/sys/restart')
    check('auth_needs_token', status == 403, status)
    status, _, body = d.set('/sys/auth/login', {'name': 'e2e', 'password': 'nope'})
    check('auth_wrong_password', status == 403, (status, body))
    start = time.time()
    status, _, body = d.set('/sys/auth/login', {'name': 'e2e', 'password': 'e2e-pass'})
    print(f'     login took {(time.time() - start) * 1000:.0f} ms')
    check('auth_login', status == 200 and body['access'] == 'admin' and body['token'].startswith('e2e.'), (status, body))
    d.token = body.get('token') if status == 200 else None
    check('auth_whoami', d.set('/sys/auth/whoami')[2] == {'access': 'admin'})
    check('auth_users_hidden_without_token', 'users' not in (Device(d.base).value('/sys/auth') or {}))
    try:
        ws = WebSocket(d.host, path='/ws?token=' + (d.token or ''))
        ws.send({'id': 1, 'op': 'get', 'path': '/sys/auth/users'})
        m = ws.recv()
        check('auth_websocket_token', m.get('status') == 'ok', m)
        ws.close()
    except OSError as e:
        check('auth_websocket_token', False, e)
    # Open again for the other tests.
    status, _, body = d.request('PUT', '/api/sys/auth/users', [])
    check('auth_reopen', status == 200 and d.value('/sys/auth/open') is True, (status, body))
    d.token = None


def test_timers(d):
    # Due at the next minute, local time.
    now = d.value('/sys/time/now')  # "2026-10-10T20:11:57+02:00"
    h, m, sec = int(now[11:13]), int(now[14:16]), int(now[17:19])
    if sec > 50:  # too close: aim for the one after
        time.sleep(61 - sec)
        now = d.value('/sys/time/now')
        h, m = int(now[11:13]), int(now[14:16])
    m += 1
    h, m = (h + m // 60) % 24, m % 60
    target = not d.value(APP + '/on')
    status, _, body = d.request('PUT', '/api/sys/timers', [
        {'name': 'e2e', 'time': f'{h:02d}:{m:02d}', 'days': 127, 'path': APP + '/on', 'value': json.dumps(target)}])
    check('timer_set', status == 200, (status, body))
    status, _, _ = d.request('PUT', '/api/sys/timers', [{'name': 'bad', 'path': APP + '/nope'}])
    check('timer_bad_path', status == 404, status)
    deadline = time.time() + 75
    while time.time() < deadline and d.value('/sys/timers/e2e/last') != 'ok':
        time.sleep(1)
    check('timer_ran', d.value('/sys/timers/e2e/last') == 'ok' and d.value(APP + '/on') == target)
    d.request('PUT', '/api/sys/timers', [])


def upload(d, path):
    image = open(path, 'rb').read()
    start = time.time()
    status, _, body = d.request('PUT', '/api/sys/firmware/image', image, timeout=120)
    took = time.time() - start
    print(f'     uploaded {len(image)} bytes in {took:.1f} s ({len(image) / took / 1024:.0f} KB/s)')
    return status, body


def test_update(d, image):
    before = d.value('/sys/firmware/slot')
    status, body = upload(d, image)
    check('upload', status == 200, (status, body))
    check('upload_restart', d.wait_restart(60) and d.wait_online())
    fw = d.value('/sys/firmware')
    check('upload_slot', fw['slot'] != before, fw)
    check('upload_pending', fw['pending'] is True, fw)
    check('confirm', d.set('/sys/firmware/confirm')[0] == 200 and d.value('/sys/firmware/pending') is False)
    status, _, body = d.set('/sys/firmware/rollback')
    check('rollback', status == 200, (status, body))
    check('rollback_restart', d.wait_restart(60) and d.wait_online())
    check('rollback_slot', d.value('/sys/firmware/slot') == before, d.value('/sys/firmware/slot'))


def test_crash_rollback(d, image):
    before = d.value('/sys/firmware/slot')
    status, body = upload(d, image)
    check('crash_upload', status == 200, (status, body))
    # It crashes, the bootloader goes back to the previous image.
    deadline = time.time() + 90
    back = False
    while time.time() < deadline:
        time.sleep(2)
        if d.value('/sys/firmware/slot') == before and d.value('/sys/status/state') == 'online':
            back = True
            break
    check('crash_rolled_back', back)
    check('crash_coredump', d.value('/sys/coredumpPresent') is True, d.value('/sys/coredumpPresent'))
    if d.value('/sys/coredumpPresent'):
        status, _, raw = d.get('/sys/coredump', raw=True, timeout=60)
        check('coredump_download', status == 200 and len(raw) > 1000, (status, len(raw)))
        check('coredump_clear', d.set('/sys/clearCoredump')[0] == 200 and d.value('/sys/coredumpPresent') is False)


def main():
    args = sys.argv[1:]
    image = crash = None
    if '--image' in args:
        image = args[args.index('--image') + 1]
    if '--crash-image' in args:
        crash = args[args.index('--crash-image') + 1]
    only = args[args.index('--only') + 1].split(',') if '--only' in args else None
    global APP
    if '--app' in args:
        APP = args[args.index('--app') + 1]
    d = Device(args[0])
    if not d.wait_online():
        print('device not online')
        return 1
    tests = [test_status, test_ui, test_websocket, test_scan, test_time, test_coredump, test_backup, test_bad_image,
             test_auth, test_timers, test_persistence]
    for t in tests:
        if not only or t.__name__ in only:
            t(d)
    if image and (not only or 'test_update' in only):
        test_update(d, image)
    if crash and (not only or 'test_crash_rollback' in only):
        test_crash_rollback(d, crash)
    print(f'DONE pass={PASS} fail={FAIL}')
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
