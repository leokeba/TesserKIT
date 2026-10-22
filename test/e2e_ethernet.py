#!/usr/bin/env python3
"""End-to-end checks of TesserKIT's Ethernet (DESIGN.md section 5.2) on a device.

Usage:
  python3 test/e2e_ethernet.py http://<ethernet ip> [--static 192.168.1.230] [--unplug]
      [--user e2e --password e2e-pass]

The device runs examples/idf/full built with -DETHERNET_BOARD=<preset> (a
Gledopto controller: gledopto), a saved Wi-Fi network, and a cable to the
same network as this machine, whose DHCP server gives it the address above.

- Always: the status (link, address, speed, MAC), the station kept away, the
  device online, <hostname>.local resolving to Ethernet's address, a restart
  coming back over the cable.
- --static ADDR: a static address on the same /24, reached there, then DHCP
  again (the lease's address back).
- --unplug: asks for the cable to be pulled, watches the station take over
  (found through mDNS), then asks for it back and watches the station leave.
Every wait is bounded and prints what it saw when it gives up.
"""
import argparse
import json
import socket
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
    token = None  # sent as Authorization: Bearer when set

    def __init__(self, base):
        self.base = base.rstrip('/')

    def at(self, ip):
        self.base = 'http://' + ip

    def request(self, method, path, body=None, timeout=5):
        data = None if body is None else json.dumps(body).encode()
        headers = {'Authorization': 'Bearer ' + self.token} if self.token else {}
        req = urllib.request.Request(self.base + '/api' + path, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                payload = r.read()
                return r.status, json.loads(payload) if payload else None
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b'null')

    def value(self, path):
        try:
            status, body = self.request('GET', path)
        except (OSError, urllib.error.URLError):
            return None  # unreachable, or restarting
        return body if status == 200 else None

    def set(self, path, body=None):
        try:
            return self.request('POST', path, body)[0]
        except (OSError, urllib.error.URLError):
            return None


def wait_for(what, fn, timeout=30.0, step=1.0):
    """fn() until it is truthy, for at most `timeout` seconds, with progress."""
    start = time.time()
    last = None
    while time.time() - start < timeout:
        last = fn()
        if last:
            print(f'  {what}: after {time.time() - start:.1f} s')
            return last
        time.sleep(step)
    print(f'  {what}: timed out after {timeout:.0f} s (last: {last!r})')
    return None


def resolve(name):
    try:
        return socket.getaddrinfo(name, 80, socket.AF_INET)[0][4][0]
    except OSError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('url')
    ap.add_argument('--static', help='a free address on the same /24, for the static test')
    ap.add_argument('--unplug', action='store_true', help='interactive: pull the cable and put it back')
    ap.add_argument('--user', default='e2e')
    ap.add_argument('--password', default='e2e-pass')
    args = ap.parse_args()
    d = Device(args.url)
    eth_ip = args.url.split('://', 1)[-1].split('/')[0].split(':')[0]

    if d.value('/sys/auth/open') is False:
        status, body = d.request('POST', '/sys/auth/login', {'name': args.user, 'password': args.password})
        if not check('login', status == 200, body):
            return 1
        d.token = body['token']

    st = d.value('/sys/network/ethernet/status')
    if not check('ethernet_tree', isinstance(st, dict), st):
        return 1
    check('link', st.get('link') is True, st)
    check('connected', st.get('connected') is True, st)
    check('address', st.get('ip') == eth_ip, st)
    check('speed', st.get('speed') in (10, 100), st)
    check('mac', len(st.get('mac', '')) == 17, st)
    wifi = d.value('/sys/network/wifi/status')
    check('station_kept_away', wifi is not None and wifi.get('connected') is False, wifi)
    check('online', d.value('/sys/status/state') == 'online')
    heap = d.value('/sys/status/heap')
    print(f'  free heap: {heap} B, lowest: {d.value("/sys/status/minHeap")} B')
    hostname = d.value('/sys/network/host') or d.value('/sys/network/hostname')
    if d.value('/sys/network/mdns'):
        got = wait_for(f'{hostname}.local', lambda: resolve(f'{hostname}.local'), timeout=15, step=2)
        check('mdns', got == eth_ip, got)

    # A restart comes back over the cable.
    t0 = time.time()
    check('restart', d.set('/sys/restart') == 200)
    time.sleep(3)
    up = wait_for('back over Ethernet', lambda: (d.value('/sys/network/ethernet/status') or {}).get('connected'),
                  timeout=40)
    check('restart_back', up, d.value('/sys/network/ethernet/status'))
    print(f'  reachable {time.time() - t0:.1f} s after the restart request; uptime {d.value("/sys/status/uptime")} s')
    wifi = d.value('/sys/network/wifi/status')
    check('station_kept_away_after_boot', wifi is not None and wifi.get('connected') is False, wifi)

    if args.static:
        prefix = '.'.join(eth_ip.split('.')[:3])
        gateway = prefix + '.254' if prefix == '192.168.1' else prefix + '.1'
        status = d.set('/sys/network/ethernet/static',
                       {'enabled': True, 'ip': args.static, 'mask': '255.255.255.0', 'gateway': gateway})
        check('static_set', status == 200, status)
        d.at(args.static)
        got = wait_for(f'reachable at {args.static}', lambda: d.value('/sys/network/ethernet/status/ip'), timeout=20)
        check('static_address', got == args.static, got)
        check('static_persisted', (d.value('/sys/network/ethernet/static') or {}).get('enabled') is True)
        status = d.set('/sys/network/ethernet/static/enabled', False)
        check('dhcp_again', status == 200, status)
        d.at(eth_ip)
        got = wait_for(f'back at {eth_ip}', lambda: d.value('/sys/network/ethernet/status/ip'), timeout=30)
        check('dhcp_address', got == eth_ip, got)

    if args.unplug:
        input('Pull the Ethernet cable, then press Enter... ')
        name = f'{hostname}.local'

        def over_wifi():
            ip = resolve(name)
            if not ip or ip == eth_ip:
                return None
            d.at(ip)
            return (d.value('/sys/network/wifi/status') or {}).get('connected') and ip

        wifi_ip = wait_for('the station took over', over_wifi, timeout=60, step=2)
        check('station_takes_over', wifi_ip, d.value('/sys/network/wifi/status'))
        if wifi_ip:
            st = d.value('/sys/network/ethernet/status')
            check('ethernet_down', st is not None and st.get('link') is False and st.get('ip') == '', st)
            check('online_over_wifi', d.value('/sys/status/state') == 'online')
            input('Plug the cable back in, then press Enter... ')
            d.at(eth_ip)
            st = wait_for('Ethernet back', lambda: (d.value('/sys/network/ethernet/status') or {}).get('connected'),
                          timeout=30)
            check('ethernet_back', st)
            wifi = wait_for('the station left',
                            lambda: (lambda w: w is not None and w.get('connected') is False)(
                                d.value('/sys/network/wifi/status')), timeout=10)
            check('station_leaves', wifi)

    print(f'\n{PASS} passed, {FAIL} failed')
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
