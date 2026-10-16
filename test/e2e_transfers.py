#!/usr/bin/env python3
"""End-to-end checks of TesserKIT's file nodes in upload/download envelopes
(TesserAPI DESIGN §10.1.1): over the serial line, and relayed by a gateway
to a node nearby over ESP-NOW (TesserAPI §14).

Usage:
  uv run --with pyserial python test/e2e_transfers.py --http http://<ip> \
      [--serial PORT] [--peer NAME] [--image <build>/tesserkit_full.bin] \
      [--peer-image <build>/tesserkit_full.bin] [--no-serial-firmware]

The device (and the peer) run examples/idf/full: Files, Backup, Firmware,
NowTp with the peer mounted under /peers. Opening the serial port resets the
board, so the script waits for it to be online again first.

Serial: a file uploaded to /sys/files and read back, the settings backup
downloaded and restored (restartAfter off meanwhile, then put back), the core
dump's download, and with --image a firmware update over the line (about
3.5 minutes for 1.6 MB at 115200 baud), the device coming back on the other
slot, not from a panic.

Relay (--peer): through the gateway's WebSocket, a file on the peer uploaded
and read back, and with --image the peer's firmware updated, each chunk's
reply time measured (the relay waits 3 s per chunk), the peer coming back on
the other slot. Prints "E2E pass=<n> fail=<n>".
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.error
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
    sys.stdout.flush()
    return ok


def b64(data):
    return base64.b64encode(data).decode()


class Http:
    def __init__(self, base):
        self.base = base.rstrip('/')

    def get(self, path, timeout=5):
        try:
            with urllib.request.urlopen(self.base + '/api' + path, timeout=timeout) as r:
                return json.loads(r.read() or b'null')
        except (urllib.error.URLError, OSError, ValueError):
            return None

    def wait(self, pred, timeout, what):
        """Polls until pred() is true, printing progress; bounded."""
        end = time.time() + timeout
        last = 0
        while time.time() < end:
            if pred():
                return True
            if time.time() - last > 10:
                print(f'  waiting for {what}...')
                last = time.time()
            time.sleep(1)
        return False


class Serial:
    def __init__(self, port):
        import serial
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = 115200
        self.s.timeout = 0.1
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.buf = b''
        self.next_id = 1

    def drain(self):
        time.sleep(0.2)
        self.s.reset_input_buffer()
        self.buf = b''
        self.s.write(b'\n')

    def envelope(self, fields, timeout=15):
        rid = self.next_id
        self.next_id += 1
        self.s.write(json.dumps({'id': rid, **fields}).encode() + b'\n')
        end = time.time() + timeout
        while time.time() < end:
            self.buf += self.s.read(max(1, self.s.in_waiting))
            while b'\n' in self.buf:
                line, self.buf = self.buf.split(b'\n', 1)
                text = line.decode('utf-8', 'replace')
                start = text.find('{"')
                if start < 0:
                    continue
                try:
                    msg = json.loads(text[start:])
                except ValueError:
                    continue
                if msg.get('id') == rid:
                    return msg.get('status'), msg.get('body')
        return 'no_reply', None


class WebSocket:
    """Just enough RFC 6455 for text frames, as TesserAPI's e2e_ws.py."""

    def __init__(self, host, port=80):
        self.sock = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f'GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                           f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        head = b''
        while b'\r\n\r\n' not in head:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError('handshake failed')
            head += chunk
        if b' 101 ' not in head.split(b'\r\n')[0]:
            raise ConnectionError(head.decode(errors='replace'))
        self.buf = head.split(b'\r\n\r\n', 1)[1]
        self.next_id = 1

    def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            header = bytes([0x81, 0x80 | n])
        elif n < 65536:
            header = bytes([0x81, 0x80 | 126]) + struct.pack('>H', n)
        else:
            header = bytes([0x81, 0x80 | 127]) + struct.pack('>Q', n)
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError('closed')
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def message(self):
        data = b''
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7f
            if n == 126:
                n = struct.unpack('>H', self._read(2))[0]
            elif n == 127:
                n = struct.unpack('>Q', self._read(8))[0]
            payload = self._read(n)
            if b0 & 0x0f in (0x9, 0xA, 0x8):  # ping, pong, close: not a message
                if b0 & 0x0f == 0x8:
                    raise ConnectionError('closed')
                continue
            data += payload
            if b0 & 0x80:
                return data.decode()

    def envelope(self, fields, timeout=15):
        rid = self.next_id
        self.next_id += 1
        self.send(json.dumps({'id': rid, **fields}))
        end = time.time() + timeout
        while time.time() < end:
            self.sock.settimeout(max(0.1, end - time.time()))
            try:
                msg = json.loads(self.message())
            except socket.timeout:
                break
            if msg.get('id') == rid:
                return msg.get('status'), msg.get('body')
        return 'no_reply', None


def upload(send, path, data, filename=None, label='', progress=False):
    """Uploads in the device's chunks, resending a busy chunk (a relay's
    last chunk still on its way). Returns (status, body, chunk times)."""
    fields = {'op': 'upload', 'path': path, 'size': len(data)}
    if filename:
        fields['filename'] = filename
    status, body = send(fields)
    if status != 'ok':
        return status, body, []
    chunk = body['chunk']
    received = body.get('received', 0)
    times = []
    shown = 0
    while True:
        part = data[received:received + chunk]
        t0 = time.time()
        for _ in range(10):
            status, body = send({'op': 'upload', 'path': path, 'offset': received, 'data': b64(part)})
            if status != 'busy':
                break
            time.sleep(1)
        times.append((received, time.time() - t0))
        if status != 'ok':
            return status, body, times
        if received + len(part) >= len(data):
            return status, body, times  # the upload's own reply
        received = body['received']
        if progress and received * 10 // len(data) > shown:
            shown = received * 10 // len(data)
            print(f'  {label} {shown * 10}% ({received} bytes, {time.time() - progress:.0f} s)')
            sys.stdout.flush()


def download(send, path):
    got = b''
    first = None
    while True:
        status, body = send({'op': 'download', 'path': path, 'offset': len(got)})
        if status != 'ok':
            return None, (status, body)
        if first is None:
            first = body
        got += base64.b64decode(body['data'])
        if body.get('end'):
            return got, first


def file_round_trip(send, base, label, size=20000):
    name = f'e2e-{label}.bin'
    blob = os.urandom(size)
    t0 = time.time()
    status, body, _ = upload(send, base + '/sys/files/upload', blob, filename=name)
    up = size / 1024 / max(time.time() - t0, 1e-3)
    check(f'{label}: upload {size} bytes to Files ({up:.1f} KB/s)', status == 'ok', (status, body))
    t0 = time.time()
    got, first = download(send, base + f'/sys/files/entries/{name}/content')
    down = size / 1024 / max(time.time() - t0, 1e-3)
    check(f'{label}: download it back ({down:.1f} KB/s)', got == blob,
          first if got is None else f'{len(got)} bytes, differ')
    status, body = send({'op': 'set', 'path': base + f'/sys/files/entries/{name}/delete'})
    check(f'{label}: delete it', status == 'ok', (status, body))


def test_serial(line, dev, image):
    file_round_trip(line.envelope, '', 'serial')

    # The settings backup, restored without a restart.
    restart_after = dev.get('/sys/backup/restartAfter')
    line.envelope({'op': 'set', 'path': '/sys/backup/restartAfter', 'body': False})
    got, first = download(line.envelope, '/sys/backup/settings')
    ok = got is not None
    try:
        settings = json.loads(got) if ok else None
    except ValueError:
        settings = None
    check('serial: download the settings', isinstance(settings, dict) and first.get('contentType') == 'application/json',
          first if got is None else got[:120])
    if settings is not None:
        status, body, _ = upload(line.envelope, '/sys/backup/settings', got, filename='settings.json')
        check('serial: restore them, no restart', status == 'ok' and isinstance(body, dict) and body.get('restart') is False,
              (status, body))
    line.envelope({'op': 'set', 'path': '/sys/backup/restartAfter', 'body': bool(restart_after)})
    check('serial: restartAfter put back', dev.get('/sys/backup/restartAfter') == restart_after)

    # The core dump: its content when there is one, else a refusal that says why.
    present = dev.get('/sys/coredumpPresent')
    got, first = download(line.envelope, '/sys/coredump')
    if present:
        check('serial: download the core dump', got is not None and len(got) > 0, first)
    else:
        check('serial: no core dump, refused', got is None, f'{len(got)} bytes' if got else '')
        print(f'  ({first})')

    if not image:
        return
    slot = dev.get('/sys/firmware/slot')
    data = open(image, 'rb').read()
    t0 = time.time()
    status, body, times = upload(line.envelope, '/sys/firmware/image', data, label='serial firmware',
                                 progress=t0)
    took = time.time() - t0
    worst = max(times, key=lambda t: t[1]) if times else (0, 0)
    check(f'serial: firmware uploaded, {len(data)} bytes in {took:.0f} s ({len(data) / 1024 / took:.1f} KB/s), '
          f'slowest chunk {worst[1] * 1000:.0f} ms at {worst[0]}, last {times[-1][1] * 1000:.0f} ms',
          status == 'ok', (status, body))
    came_back = dev.wait(lambda: dev.get('/sys/firmware/slot') not in (None, slot), 90, 'the device on its new slot')
    check(f'serial: the device boots the other slot ({dev.get("/sys/firmware/slot")})', came_back)
    reason = dev.get('/sys/status/resetReason')
    check(f'serial: restarted by software, not a panic ({reason})', reason == 'software', reason)


def test_relay(dev, host, peer, image):
    base = '/peers/' + peer
    ws = WebSocket(host)
    file_round_trip(ws.envelope, base, 'relay')
    if not image:
        return
    slot = dev.get(base + '/sys/firmware/slot')
    data = open(image, 'rb').read()
    t0 = time.time()
    status, body, times = upload(ws.envelope, base + '/sys/firmware/image', data, label='relayed firmware',
                                 progress=t0)
    took = time.time() - t0
    worst = max(times, key=lambda t: t[1]) if times else (0, 0)
    over = sum(1 for _, t in times if t > 3)
    check(f'relay: firmware uploaded, {len(data)} bytes in {took:.0f} s ({len(data) / 1024 / took:.1f} KB/s), '
          f'slowest chunk {worst[1] * 1000:.0f} ms at {worst[0]}, last {times[-1][1] * 1000:.0f} ms, '
          f'{over} over 3 s', status == 'ok', (status, body))
    came_back = dev.wait(lambda: dev.get(base + '/sys/firmware/slot') not in (None, slot), 120,
                         'the peer on its new slot')
    check(f'relay: the peer boots the other slot ({dev.get(base + "/sys/firmware/slot")})', came_back)
    reason = dev.get(base + '/sys/status/resetReason')
    check(f'relay: the peer restarted by software ({reason})', reason == 'software', reason)
    heap = dev.get('/sys/status/minHeap')
    print(f'  gateway minHeap after: {heap}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--http', required=True)
    ap.add_argument('--serial')
    ap.add_argument('--peer')
    ap.add_argument('--image')
    # The peer's own build when it differs from the gateway's (an Ethernet
    # gateway's image on a devkit peer).
    ap.add_argument('--peer-image')
    ap.add_argument('--no-serial-firmware', action='store_true')
    args = ap.parse_args()
    dev = Http(args.http)
    host = args.http.split('//', 1)[-1].split('/')[0]

    if args.serial:
        line = Serial(args.serial)
        # Opening the port may have reset the board.
        time.sleep(2)
        if not check('device online', dev.wait(lambda: dev.get('/sys/status/state') == 'online', 60, 'the device')):
            return 1
        line.drain()
        test_serial(line, dev, None if args.no_serial_firmware else args.image)
    if args.peer:
        if not check('peer online', dev.wait(lambda: dev.get(f'/peers/{args.peer}/sys/status/state') is not None, 90,
                                             'the peer under /peers')):
            return 1
        test_relay(dev, host, args.peer, args.peer_image or args.image)
    print(f'E2E pass={PASS} fail={FAIL}')
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
