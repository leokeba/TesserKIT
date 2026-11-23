#!/usr/bin/env python3
"""Benchmarks of TesserKIT's transports, run by hand against the boards.

  uv run --with pyserial --with bleak --with zeroconf \\
      python test/bench.py test/bench_boards.json

Drives every transport from the test machine against the boards in the
boards file (test/bench_boards.example.json): the HTTP API, WebSocket, the
serial line, Bluetooth, UDP (unicast and broadcast), reads through /peers,
the network clock's carriers and mDNS. The boards run examples/idf/full
(the Gledoptos with the classic profile and the Gledopto Ethernet board),
on one LAN, joined on the same Wi-Fi channel, ESP-NOW on that channel.

Settings changed during the run are restored (peers' route order, the
clock's carriers, the time zone). The boards file is gitignored (like
hil_boards.json): it names this machine's boards.

Per board unless noted:
  HTTP      - GET /api/sys/status latency (30) and throughput (50)
  WebSocket - handshake, snapshot, write -> change event (/sys/time/tz)
  Serial    - envelope round trip over the console UART (30)
  Bluetooth - NUS envelope round trip (boards with a BLE address)
  UDP       - unicast /sys/udp/send round trip, every board to every other
  UDP       - broadcast: the source's HTTP answer and every peer's LED on
  peers     - a read through /peers on the route in use; forced "udp" and
             "nowtp" orders on the first board
  Clock     - /sys/clock/status, the carrier in use; forced "udp" and
             "nowtp" carriers on the non-reference boards
  mDNS      - discovery of _http._tcp and _tesser._tcp

Not measured: a write -> change event over UDP (the reply is the source's
HTTP answer, not the peer's), and the absolute cross-board clock phase
(the pulse test measures waveform timing, not synchronized phase).
"""
import asyncio
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.request

RESULTS = []


def note(section, name, ok, detail=''):
    RESULTS.append((section, name, ok, detail))
    print(f'{section:<11} {name:<38} {"OK" if ok else "FAIL":<4} {detail}')


def stats(xs):
    if not xs:
        return None
    xs = sorted(xs)
    n = len(xs)
    return {
        'n': n,
        'min': xs[0],
        'median': xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2,
        'p95': xs[min(n - 1, int(0.95 * n))],
        'max': xs[-1],
        'mean': sum(xs) / n,
    }


def fmt(s, unit='ms'):
    if s is None:
        return 'n/a'
    return f'min {s["min"]:.2f}  med {s["median"]:.2f}  p95 {s["p95"]:.2f}  max {s["max"]:.2f} {unit}'


def get(ip, path, timeout=10):
    with urllib.request.urlopen(f'http://{ip}/api{path}', timeout=timeout) as r:
        payload = r.read()
        return json.loads(payload) if payload else None


def set_value(ip, path, body, timeout=10):
    data = json.dumps(body).encode()
    req = urllib.request.Request(f'http://{ip}/api{path}', data=data, method='POST',
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        payload = r.read()
        return json.loads(payload) if payload else None


def node_name(ip):
    """The node's name, as /peers mounts it: the hostname, else the label."""
    for path in ('/sys/network/host', '/sys/name'):
        try:
            v = get(ip, path)
            if isinstance(v, str) and v:
                return v
        except Exception:
            pass
    return None


def http_bench(ip, n=30, throughput=50):
    lat = []
    for _ in range(n):
        t = time.perf_counter()
        get(ip, '/sys/status')
        lat.append((time.perf_counter() - t) * 1000)
    t = time.perf_counter()
    for _ in range(throughput):
        get(ip, '/sys/status')
    dt = time.perf_counter() - t
    return stats(lat), throughput / dt


class WS:
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
            if opcode == 0x9:
                continue
            message += payload
            if b0 & 0x80:
                return json.loads(message)

    def close(self):
        self.sock.close()


def ws_bench(ip, n=5):
    handshakes, snapshots, changes = [], [], []
    tz_before = get(ip, '/sys/time/tz')
    for i in range(n):
        try:
            t = time.perf_counter()
            ws = WS(ip)
            handshakes.append((time.perf_counter() - t) * 1000)
            ws.send({'id': 1, 'op': 'sub', 'path': '/sys/time/tz'})
            t = time.perf_counter()
            ws.recv(timeout=5)
            snapshots.append((time.perf_counter() - t) * 1000)
            # A write that only this node produces: the time zone.
            t = time.perf_counter()
            set_value(ip, '/sys/time/tz', 'UTC0', timeout=5)
            deadline = time.time() + 4
            while time.time() < deadline:
                try:
                    m = ws.recv(timeout=4)
                except Exception:
                    break
                if m.get('op') == 'change' and m.get('path') == '/sys/time/tz':
                    changes.append((time.perf_counter() - t) * 1000)
                    break
            ws.close()
        except Exception as e:
            print(f'    ws try {i} failed: {e!r}')
        time.sleep(0.2)
    try:
        set_value(ip, '/sys/time/tz', tz_before, timeout=5)
    except Exception as e:
        print(f'    restore tz failed: {e!r}')
    return stats(handshakes), stats(snapshots), stats(changes)


def serial_bench(port, n=30):
    import serial
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(1)
    lat = []
    for i in range(n):
        env = {'id': i, 'op': 'get', 'path': '/sys/status'}
        s.write(json.dumps(env, separators=(',', ':')).encode() + b'\n')
        t = time.perf_counter()
        buf = b''
        end = time.time() + 5
        got = None
        while time.time() < end:
            buf += s.read(max(1, s.in_waiting))
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                p = line.find(b'{"id"')
                if p < 0:
                    continue
                try:
                    m = json.loads(line[p:])
                except Exception:
                    continue
                if m.get('id') == i:
                    got = m
                    break
            if got is not None:
                break
        if got is not None:
            lat.append((time.perf_counter() - t) * 1000)
        else:
            print(f'    serial try {i}: no reply')
    s.close()
    return stats(lat)


async def ble_bench(address, n=30):
    from bleak import BleakClient
    lat = []
    async with BleakClient(address) as client:
        rx = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'
        tx = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'
        pending = []

        def on_notify(_, data):
            pending.append(bytes(data))

        await client.start_notify(tx, on_notify)
        for i in range(n):
            env = {'id': i, 'op': 'get', 'path': '/sys/status'}
            data = json.dumps(env, separators=(',', ':')).encode() + b'\n'
            pending.clear()
            t = time.perf_counter()
            await client.write_gatt_char(rx, data, response=True)
            end = time.time() + 5
            buf = b''
            got = None
            while time.time() < end:
                await asyncio.sleep(0.05)
                for chunk in pending:
                    buf += chunk
                pending.clear()
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    p = line.find(b'{"id"')
                    if p < 0:
                        continue
                    try:
                        m = json.loads(line[p:])
                    except Exception:
                        continue
                    if m.get('id') == i:
                        got = m
                        break
                if got is not None:
                    break
            if got is not None:
                lat.append((time.perf_counter() - t) * 1000)
            else:
                print(f'    ble try {i}: no reply')
        await client.stop_notify(tx)
    return stats(lat)


def udp_unicast_bench(src_ip, dst_ip, n=20):
    lat = []

    def send_once(timeout=30):
        data = json.dumps({'path': '/led/brightness', 'value': '77', 'to': dst_ip}).encode()
        req = urllib.request.Request(f'http://{src_ip}/api/sys/udp/send', data=data, method='POST',
                                     headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            r.read()

    try:
        send_once()  # warm up: learn the peer
    except Exception as e:
        print(f'    warm-up failed: {e!r}')
    for i in range(n):
        t = time.perf_counter()
        try:
            send_once()
            lat.append((time.perf_counter() - t) * 1000)
        except Exception as e:
            print(f'    udp try {i}: {e!r}')
        time.sleep(0.1)
    return stats(lat)


def udp_broadcast_bench(src_ip, others_ips, n=5):
    lat = []
    for i in range(n):
        data = json.dumps({'path': '/led/on', 'value': 'true', 'to': ''}).encode()
        req = urllib.request.Request(f'http://{src_ip}/api/sys/udp/send', data=data, method='POST',
                                     headers={'Content-Type': 'application/json'})
        t = time.perf_counter()
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                r.read()
        except Exception as e:
            print(f'    broadcast try {i}: {e!r}')
            continue
        # time until every peer has applied the write
        deadline = time.time() + 10
        applied = False
        while time.time() < deadline:
            try:
                vals = [get(ip, '/led') for ip in others_ips]
                if all(isinstance(v, dict) and v.get('on') is True for v in vals):
                    applied = True
                    break
            except Exception:
                pass
            time.sleep(0.1)
        if applied:
            lat.append((time.perf_counter() - t) * 1000)
        else:
            print(f'    broadcast try {i}: peers did not apply')
        time.sleep(0.3)
    # leave the LEDs off (the example's sequencer re-toggles them anyway)
    for ip in others_ips:
        try:
            set_value(ip, '/led/on', False, timeout=5)
        except Exception:
            pass
    return stats(lat)


def peers_read_bench(src_ip, peer_name, n=10):
    lat = []
    for i in range(n):
        t = time.perf_counter()
        try:
            get(src_ip, f'/peers/{peer_name}/sys/status/state', timeout=10)
            lat.append((time.perf_counter() - t) * 1000)
        except Exception as e:
            print(f'    peers try {i}: {e!r}')
        time.sleep(0.2)
    return stats(lat)


def clock_bench(ip, carriers=None, waitMs=30000):
    """Read /sys/clock/status; optionally force the carrier first."""
    before = None
    try:
        if carriers is not None:
            before = get(ip, '/sys/clock/carriers')
            set_value(ip, '/sys/clock/carriers', carriers, timeout=10)
            time.sleep(2)
        # wait for the sync
        end = time.time() + waitMs / 1000
        status = None
        while time.time() < end:
            status = get(ip, '/sys/clock/status')
            if status and status.get('synced') and status.get('rounds', 0) > 0:
                break
            time.sleep(1)
        return status
    finally:
        if carriers is not None and before is not None:
            try:
                set_value(ip, '/sys/clock/carriers', before, timeout=10)
            except Exception as e:
                print(f'    restore carriers failed: {e!r}')


def mdns_bench(ip, timeout=15):
    from zeroconf import ServiceBrowser, Zeroconf
    found = {}
    done = False

    class Handler:
        def __init__(self):
            self.t0 = time.perf_counter()

        def add_service(self, zc, type_, name):
            try:
                info = zc.get_service_info(type_, name, timeout=3)
            except Exception:
                info = None
            if info and info.addresses:
                for a in info.addresses:
                    try:
                        addr = socket.inet_ntoa(a)
                    except Exception:
                        continue
                    if addr == ip:
                        found[type_] = (time.perf_counter() - self.t0) * 1000
            if len(found) >= 2:
                nonlocal done
                done = True

        def remove_service(self, zc, type_, name):
            pass

        def update_service(self, zc, type_, name):
            pass

    zc = Zeroconf()
    handler = Handler()
    browsers = [ServiceBrowser(zc, '_http._tcp.local.', handler),
                ServiceBrowser(zc, '_tesser._tcp.local.', handler)]
    end = time.time() + timeout
    while time.time() < end and not done:
        time.sleep(0.2)
    for b in browsers:
        b.cancel()
    zc.close()
    return found


def load_boards(path):
    with open(path) as f:
        cfg = json.load(f)
    boards = cfg.get('boards')
    if not boards:
        raise SystemExit(f'{path}: no "boards"')
    for b in boards:
        if not b.get('url'):
            raise SystemExit(f'{path}: board {b.get("name")!r} has no "url"')
        b['ip'] = b['url'].split('://', 1)[1].split('/')[0].split(':')[0]
    return boards


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    boards = load_boards(sys.argv[1])
    print(f'boards: {", ".join(b["name"] for b in boards)}')
    for b in boards:
        try:
            b['node'] = node_name(b['ip'])
        except Exception as e:
            print(f'    {b["name"]}: no node name ({e!r})')
            b['node'] = None

    print('=== HTTP (host -> board) ===')
    for b in boards:
        try:
            st, rps = http_bench(b['ip'])
            note('HTTP', b['name'], True, f'lat: {fmt(st)}  throughput: {rps:.1f} req/s')
        except Exception as e:
            note('HTTP', b['name'], False, repr(e))

    print('=== WebSocket (host -> board) ===')
    for b in boards:
        try:
            hs, snap, chg = ws_bench(b['ip'])
            note('WebSocket', b['name'], True,
                 f'handshake: {fmt(hs)}  snapshot: {fmt(snap)}  write->change: {fmt(chg)}')
        except Exception as e:
            note('WebSocket', b['name'], False, repr(e))

    print('=== Serial (host -> board UART) ===')
    for b in boards:
        if not b.get('port'):
            continue
        try:
            st = serial_bench(b['port'])
            note('Serial', b['name'], True, fmt(st))
        except Exception as e:
            note('Serial', b['name'], False, repr(e))

    print('=== Bluetooth (host -> board) ===')
    for b in boards:
        if not b.get('ble'):
            continue
        try:
            st = asyncio.run(ble_bench(b['ble']))
            note('Bluetooth', b['name'], True, fmt(st))
        except Exception as e:
            note('Bluetooth', b['name'], False, repr(e))

    print('=== UDP unicast (source -> target) ===')
    for i, s in enumerate(boards):
        for j, d in enumerate(boards):
            if i == j:
                continue
            # The target's Ethernet address when both ends have one (the
            # direct cable); its address on the LAN otherwise.
            target = d['ethernet'] if (s.get('ethernet') and d.get('ethernet')) else d['ip']
            try:
                st = udp_unicast_bench(s['ip'], target)
                note('UDP unicast', f'{s["name"]} -> {d["name"]}', True, fmt(st))
            except Exception as e:
                note('UDP unicast', f'{s["name"]} -> {d["name"]}', False, repr(e))

    print('=== UDP broadcast (one write, all peers) ===')
    for b in boards:
        others = [o['ip'] for o in boards if o is not b]
        try:
            st = udp_broadcast_bench(b['ip'], others)
            note('UDP broadcast', b['name'], True, fmt(st))
        except Exception as e:
            note('UDP broadcast', b['name'], False, repr(e))

    print('=== peers read (default order) ===')
    for i, b in enumerate(boards):
        peer = boards[(i + 1) % len(boards)]
        if not peer.get('node'):
            note('peers read', f'{b["name"]} -> {peer["name"]}', False, 'no node name')
            continue
        try:
            st = peers_read_bench(b['ip'], peer['node'])
            note('peers read', f'{b["name"]} -> {peer["name"]}', True, fmt(st))
        except Exception as e:
            note('peers read', f'{b["name"]} -> {peer["name"]}', False, repr(e))

    print('=== peers read (forced route order, first board) ===')
    first = boards[0]
    peer = boards[1] if len(boards) > 1 else None
    if peer and peer.get('node'):
        for order in ('udp', 'nowtp'):
            before = None
            try:
                before = get(first['ip'], '/sys/peers/order')
                set_value(first['ip'], '/sys/peers/order', order, timeout=10)
                time.sleep(2)
                st = peers_read_bench(first['ip'], peer['node'], n=10)
                note('peers read', f'{first["name"]} order={order}', True, fmt(st))
            except Exception as e:
                note('peers read', f'{first["name"]} order={order}', False, repr(e))
            finally:
                if before is not None:
                    try:
                        set_value(first['ip'], '/sys/peers/order', before, timeout=10)
                    except Exception as e:
                        print(f'    restore order failed: {e!r}')

    print('=== Clock (default carriers) ===')
    reference = None
    for b in boards:
        try:
            st = clock_bench(b['ip'])
            if st and st.get('isReference'):
                reference = b
            note('Clock', b['name'], True,
                 f'carrier={st.get("carrier")} roundTrip={st.get("roundTrip")} us '
                 f'offset={st.get("offset")} us drift={st.get("drift")} ppm '
                 f'rounds={st.get("rounds")}')
        except Exception as e:
            note('Clock', b['name'], False, repr(e))

    print('=== Clock (forced carriers, non-reference boards) ===')
    for b in boards:
        if b is reference:
            continue
        for carrier in ('udp', 'nowtp'):
            try:
                st = clock_bench(b['ip'], carriers=carrier)
                note('Clock', f'{b["name"]} ({carrier})', True,
                     f'carrier={st.get("carrier")} roundTrip={st.get("roundTrip")} us '
                     f'offset={st.get("offset")} us drift={st.get("drift")} ppm '
                     f'rounds={st.get("rounds")}')
            except Exception as e:
                note('Clock', f'{b["name"]} ({carrier})', False, repr(e))

    print('=== mDNS ===')
    for b in boards:
        try:
            found = mdns_bench(b['ip'])
            note('mDNS', b['name'], bool(found),
                 f'_http={found.get("_http._tcp.local.", "not found")} '
                 f'_tesser={found.get("_tesser._tcp.local.", "not found")}')
        except Exception as e:
            note('mDNS', b['name'], False, repr(e))

    print()
    print('=== Summary ===')
    for section, name, ok, detail in RESULTS:
        print(f'{section:<11} {name:<38} {"OK" if ok else "FAIL":<4} {detail}')


if __name__ == '__main__':
    main()
