#!/usr/bin/env python3
"""End-to-end checks of TesserKIT's UDP service (/sys/udp) between devices.

Usage:
  python3 test/e2e_udp.py http://<a> http://<b> [http://<c> ...]

Each device runs examples/idf/full (its LED at /led, the blink sequence,
UdpApi on port 8484), all on one LAN: Wi-Fi, Ethernet or both. The devices
are open (no admin account). Each one in turn broadcasts writes to the
others with /sys/udp/send, and writes to one of them; the first one reads
the others through /peers. The LEDs are left off.
"""
import json
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
    return ok


class Device:
    def __init__(self, base):
        self.base = base.rstrip('/')
        self.host = self.base.split('://', 1)[1].split('/')[0].split(':')[0]
        self.name = None

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

    def set(self, path, body):
        return self.request('POST', path, body)[0]

    def send(self, path, value, to=''):
        return self.request('POST', '/sys/udp/send', {'path': path, 'value': json.dumps(value), 'to': to})


def wait_for(fn, timeout=5.0, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    devs = [Device(u) for u in sys.argv[1:]]
    for d in devs:
        d.name = d.value('/sys/network/hostname')
        st = d.value('/sys/udp/status')
        check(f'running_{d.name}', st and st['running'], st)
        # Failures before this run (a peer restarted by another test) don't count.
        d.failed_before = st['failed'] if st else 0

    # Every device shows the others under /peers (by UDP, or NowTP first).
    for d in devs:
        others = [o.name for o in devs if o is not d]
        peers = wait_for(lambda: (lambda p: p if p and all(n in p for n in others) else None)(
            d.value('/peers?depth=1')), 30, step=1)
        check(f'peers_{d.name}', peers and all(n in peers for n in others), peers)

    # Through /peers, the first device reads each other one's state.
    a = devs[0]
    for o in devs[1:]:
        t = time.time()
        state = wait_for(lambda: a.value(f'/peers/{o.name}/sys/status/state'), 10, step=0.5)
        check(f'read_through_peers_{a.name}_{o.name}', state == 'online', state)
        print(f'      {(time.time() - t) * 1000:.0f} ms')

    # Broadcasts: each device in turn switches the others' LEDs, not its own.
    for d in devs:
        others = [o for o in devs if o is not d]
        for lit in (True, False):
            d.set('/led/on', not lit)
            for o in others:
                o.set('/led/on', not lit)
            t = time.time()
            status, body = d.send('/led/on', lit)
            check(f'broadcast_sent_{d.name}_{lit}', status == 200, (status, body))
            got = wait_for(lambda: all((o.value('/led') or {}).get('on') is lit for o in others), 5)
            took = time.time() - t
            check(f'broadcast_applied_{d.name}_{lit}', got, [(o.name, o.value('/led')) for o in others])
            check(f'broadcast_not_self_{d.name}_{lit}', (d.value('/led') or {}).get('on') is (not lit),
                  d.value('/led'))
            print(f'      every other LED {"on" if lit else "off"} within {took * 1000:.0f} ms (HTTP polling)')

    # One node: the write's reply comes back.
    b = devs[1]
    status, body = a.send('/led/brightness', 77, to=b.host)
    check('unicast_reply', status == 200 and body == 77, (status, body))
    check('unicast_applied', (b.value('/led') or {}).get('brightness') == 77, b.value('/led'))
    status, body = a.send('/nope', 1, to=b.host)
    check('unicast_error', status == 404 and body and body.get('error') == 'not_found', (status, body))
    status, body = a.send('/led/on', True, to='not-an-address')
    check('bad_address', status == 422, (status, body))

    # A sequence started on every other node at once, then stopped.
    status, _ = a.send('/sequencer/play', 'on')
    running = wait_for(lambda: all((o.value('/sequencer/status') or {}).get('running') for o in devs[1:]), 5)
    check('sequence_started_everywhere', running,
          [(o.name, (o.value('/sequencer/status') or {}).get('running')) for o in devs[1:]])
    a.send('/sequencer/stop', None)
    stopped = wait_for(lambda: all(not (o.value('/sequencer/status') or {}).get('running') for o in devs[1:]), 5)
    check('sequence_stopped_everywhere', stopped)

    for d in devs:
        d.set('/led/on', False)
        st = d.value('/sys/udp/status')
        check(f'nothing_failed_{d.name}', st and st['failed'] == d.failed_before, st)
    print(f'DONE pass={PASS} fail={FAIL}')
    sys.exit(1 if FAIL else 0)


if __name__ == '__main__':
    main()
