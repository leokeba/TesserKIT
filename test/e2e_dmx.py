#!/usr/bin/env python3
"""Load test of Art-Net and sACN reception into an application's sink.

Usage:
  python3 test/e2e_dmx.py http://<device> [--universes 24] [--fps 44]
      [--seconds 20] [--protocols artnet,sacn,sacn-multicast] [--no-ui]

The device runs examples/idf/full built with -DDMX_PROBE=<n> (n at least
--universes; least and most are checked when n is --universes): Art-Net universes 0 to n-1 and sACN universes 1 to n go to a
DmxSink that copies each frame into a frame buffer, as an LED controller
does, and counts them under /dmx. For each protocol the script sends every
universe at --fps frames a second, each frame's universes back to back as a
console does, then a sync packet (ArtSync; an E1.31 sync on universe 64000,
which the frames name), while a streamed subscription to the whole tree stays
open and /sys/status is read every second, as TesserUI's page does. It
checks that the sink got every frame and every sync, that no sequence error
was counted, and reports the heap's lowest point and the longest sink call.

sacn-multicast sends to the universes' groups (239.255.0.x), out of the
interface that reaches the device; the others are unicast to it.
"""
import argparse
import json
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request

PASS = 0
FAIL = 0
SYNC_UNIVERSE = 64000 - 1  # E1.31 universes go to 63999


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
        self.host = self.base.split('://', 1)[1].split('/')[0].split(':')[0]

    def request(self, method, path, body=None, timeout=10):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.base + '/api' + path, data=data, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                payload = r.read()
                return r.status, json.loads(payload) if payload else None
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b'null')

    def value(self, path):
        try:
            status, body = self.request('GET', path, timeout=5)
        except (OSError, urllib.error.URLError):
            return None
        return body if status == 200 else None


def art_dmx(universe, seq, data):
    return (b'Art-Net\0' + struct.pack('<H', 0x5000) + struct.pack('>H', 14) + bytes([seq, 0]) +
            struct.pack('<H', universe) + struct.pack('>H', len(data)) + data)


def art_sync():
    return b'Art-Net\0' + struct.pack('<H', 0x5200) + struct.pack('>H', 14) + b'\0\0'


CID = bytes(range(16))


def sacn_data(universe, seq, data, sync=0, priority=100):
    n = 126 + len(data)
    p = bytearray(n)
    p[0:2] = b'\x00\x10'
    p[4:16] = b'ASC-E1.17\0\0\0'
    p[16:18] = struct.pack('>H', 0x7000 | (n - 16))
    p[18:22] = struct.pack('>I', 4)
    p[22:38] = CID
    p[38:40] = struct.pack('>H', 0x7000 | (n - 38))
    p[40:44] = struct.pack('>I', 2)
    p[44:52] = b'e2e_dmx\0'
    p[108] = priority
    p[109:111] = struct.pack('>H', sync)
    p[111] = seq
    p[113:115] = struct.pack('>H', universe)
    p[115:117] = struct.pack('>H', 0x7000 | (n - 115))
    p[117] = 0x02
    p[118] = 0xA1
    p[121:123] = b'\x00\x01'
    p[123:125] = struct.pack('>H', len(data) + 1)
    p[126:] = data
    return bytes(p)


def sacn_sync(address, seq):
    p = bytearray(49)
    p[0:2] = b'\x00\x10'
    p[4:16] = b'ASC-E1.17\0\0\0'
    p[16:18] = struct.pack('>H', 0x7000 | 33)
    p[18:22] = struct.pack('>I', 8)
    p[22:38] = CID
    p[38:40] = struct.pack('>H', 0x7000 | 11)
    p[40:44] = struct.pack('>I', 1)
    p[44] = seq
    p[45:47] = struct.pack('>H', address)
    return bytes(p)


def group(universe):
    return f'239.255.{universe >> 8}.{universe & 0xFF}'


class Ui(threading.Thread):
    """What TesserUI's open page costs the device: a streamed subscription to
    the whole tree, and /sys/status read every second."""

    def __init__(self, dev):
        super().__init__(daemon=True)
        self.dev = dev
        self.stop = False
        self.lines = 0
        self.reads = 0

    def run(self):
        threading.Thread(target=self.poll, daemon=True).start()
        try:
            req = urllib.request.Request(self.dev.base + '/api/?sub&interval=1000')
            with urllib.request.urlopen(req, timeout=30) as r:
                while not self.stop:
                    if not r.readline():
                        break
                    self.lines += 1
        except (OSError, urllib.error.URLError) as e:
            print(f'  (subscription ended: {e})')

    def poll(self):
        while not self.stop:
            if self.dev.value('/sys/status') is not None:
                self.reads += 1
            time.sleep(1)


def counters(dev):
    d = dev.value('/dmx')
    a = dev.value('/sys/artnet/status')
    s = dev.value('/sys/sacn/status')
    st = dev.value('/sys/status')
    if not (d and a and s and st):
        return None
    return {'dmx': d, 'artnet': a, 'sacn': s, 'status': st}


SEQUENCES = {}


def send(dev, proto, universes, fps, seconds, sock, spread):
    period = 1.0 / fps
    frames = int(seconds * fps)
    data = bytes(range(256)) * 2
    # One source throughout (one CID): its numbering goes on from run to run.
    seqs = SEQUENCES.setdefault('artnet' if proto == 'artnet' else 'sacn', [1] * universes)
    sync_seq = 0
    multicast = proto == 'sacn-multicast'
    start = time.perf_counter()
    late = 0
    for f in range(frames):
        for u in range(universes):
            if spread:  # evenly through the period, not back to back
                wait = start + (f + u / (universes + 1)) * period - time.perf_counter()
                if wait > 0:
                    time.sleep(wait)
            if proto == 'artnet':
                sock.sendto(art_dmx(u, seqs[u], data), (dev.host, 6454))
                seqs[u] = seqs[u] % 255 + 1
            else:
                to = group(u + 1) if multicast else dev.host
                sock.sendto(sacn_data(u + 1, seqs[u], data, sync=SYNC_UNIVERSE), (to, 5568))
                seqs[u] = (seqs[u] + 1) % 256
        if spread:
            wait = start + (f + universes / (universes + 1)) * period - time.perf_counter()
            if wait > 0:
                time.sleep(wait)
        if proto == 'artnet':
            sock.sendto(art_sync(), (dev.host, 6454))
        else:
            to = group(SYNC_UNIVERSE) if multicast else dev.host
            sock.sendto(sacn_sync(SYNC_UNIVERSE, sync_seq), (to, 5568))
            sync_seq = (sync_seq + 1) % 256
        wait = start + (f + 1) * period - time.perf_counter()
        if wait > 0:
            time.sleep(wait)
        else:
            late += 1
    elapsed = time.perf_counter() - start
    return frames, elapsed, late


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('device')
    ap.add_argument('--universes', type=int, default=24)
    ap.add_argument('--fps', type=float, default=44)
    ap.add_argument('--seconds', type=float, default=20)
    ap.add_argument('--protocols', default='artnet,sacn,sacn-multicast')
    ap.add_argument('--no-ui', action='store_true')
    ap.add_argument('--spread', action='store_true', help="each frame's universes evenly through the period")
    args = ap.parse_args()
    dev = Device(args.device)
    if not check('device answers', dev.value('/sys/status') is not None, 'no /sys/status'):
        sys.exit(1)
    if not check('device has the DMX probe', dev.value('/dmx') is not None,
                 'build examples/idf/full with -DDMX_PROBE=<n>'):
        sys.exit(1)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    # Multicast out of the interface that reaches the device.
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((dev.host, 9))
    local = probe.getsockname()[0]
    probe.close()
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(local))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
    print(f'from {local} to {dev.host}: {args.universes} universes at {args.fps:g} fps for {args.seconds:g} s')

    ui = None
    if not args.no_ui:
        ui = Ui(dev)
        ui.start()
        time.sleep(1)

    for proto in args.protocols.split(','):
        dev.request('POST', '/dmx/reset')
        time.sleep(0.5)
        before = counters(dev)
        if not check(f'{proto}: counters read', before is not None, 'no answer'):
            continue
        frames, elapsed, late = send(dev, proto, args.universes, args.fps, args.seconds, sock, args.spread)
        time.sleep(1.0)  # the last ones, and the next status reads
        after = counters(dev)
        if not check(f'{proto}: counters read after', after is not None, 'no answer'):
            continue
        sent = frames * args.universes
        key = 'artnet' if proto == 'artnet' else 'sacn'
        got = after['dmx'][key] - before['dmx'][key]
        svc = after[key]['packets'] - before[key]['packets']
        errors = after[key]['sequenceErrors'] - before[key]['sequenceErrors']
        rate = sent / elapsed
        print(f'  sent {sent} frames in {elapsed:.1f} s ({rate:.0f}/s, {late} periods late), '
              f'the service counted {svc}, the sink got {got}; least {after["dmx"]["least"]}, '
              f'most {after["dmx"]["most"]} a universe; syncs {after["dmx"]["syncs"]}; '
              f'longest sink call {after["dmx"]["longestUs"]} µs; heap {after["status"]["heap"]} B, '
              f'lowest {after["status"]["minHeap"]} B')
        check(f'{proto}: the sink got every frame', got == sent, f'{sent - got} of {sent} lost')
        if after['dmx'].get('universes') == args.universes:
            check(f'{proto}: every universe got them all', after['dmx']['least'] == frames,
                  f'least {after["dmx"]["least"]} of {frames}')
        check(f'{proto}: every sync arrived', after['dmx']['syncs'] == frames,
              f'{after["dmx"]["syncs"]} of {frames}')
        check(f'{proto}: no sequence error', errors == 0, f'{errors}')
        check(f'{proto}: no restart', after['status']['uptime'] >= before['status']['uptime'],
              f'restarted ({after["status"]["resetReason"]})')

    if ui:
        ui.stop = True
        check('the page stayed served', ui.reads >= args.seconds * len(args.protocols.split(',')) * 0.8,
              f'{ui.reads} status reads, {ui.lines} notifications')
        print(f'  page: {ui.reads} status reads, {ui.lines} notifications')
    print(f'DONE pass={PASS} fail={FAIL}')
    sys.exit(1 if FAIL else 0)


if __name__ == '__main__':
    main()
