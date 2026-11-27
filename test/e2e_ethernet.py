#!/usr/bin/env python3
"""End-to-end checks of TesserKIT's Ethernet (DESIGN.md section 5.2) on a device.

Usage:
  python3 test/e2e_ethernet.py http://<ethernet ip> [--static 192.168.1.230] [--unplug]
      [--user e2e --password e2e-pass]
  python3 test/e2e_ethernet.py --link-local http://<station ip> [--peer http://<station ip>]

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
- --link-local: the cable has no DHCP server (DESIGN.md 5.2, "Link-local"),
  so this machine reaches the device over Wi-Fi (the URL is the station's
  address) and checks what that wiring can: an address in 169.254/16, the
  station joined beside it, the internet still reached after a restart
  (SNTP synced from boot: Ethernet without a gateway doesn't take the
  default route, "Two interfaces at once"), mDNS answering with the
  station's address. With --peer, the device at the other end of the cable
  (its station URL, link-local too): each one writes to the other at its
  169.254 address with /sys/udp/send, which only the cable carries, and sees
  the other under /peers by `udp/ethernet`. --static and --unplug don't
  apply there.
Every wait is bounded and prints what it saw when it gives up.
"""
import argparse
import json
import socket
import sys
import time

import e2e_http as e2e
from e2e_http import check


class Device(e2e.Device):
    def __init__(self, base):
        super().__init__(base, timeout=5)

    def at(self, ip):
        self.base = 'http://' + ip


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


class OverWifi(Device):
    """A device this machine reaches over Wi-Fi, where gledopto-d has left
    one HTTP read in a few unanswered for 5 s, mostly in the minute after a
    boot (2026-11-26): a read without an answer is asked again for up to
    20 s, and says so. A wrong answer still fails at once."""

    def value(self, path):
        end = time.time() + 20
        got = super().value(path)
        while got is None and time.time() < end:
            time.sleep(0.5)
            got = super().value(path)
            print(f'  {self.base}{path}: asked again, {"answered" if got is not None else "no answer"}')
        return got


def link_local(ip):
    return isinstance(ip, str) and ip.startswith('169.254.')


def eth_status_ok(st):
    return isinstance(st, dict) and st.get('connected') is True and link_local(st.get('ip'))


def cable_writes(a, b):
    """a writes b's LED brightness at b's 169.254 address over UDP; b's own
    value (read over Wi-Fi) shows the write arrived. Left as it was."""
    a_name, b_name = a.value('/sys/network/host'), b.value('/sys/network/host')
    b_ip = (b.value('/sys/network/ethernet/status') or {}).get('ip')
    before = (b.value('/led') or {}).get('brightness')
    if not check(f'peer_led_{b_name}', isinstance(before, int), b.value('/led')):
        return
    want = 77 if before != 77 else 78
    t = time.time()
    try:
        status, body = a.request('POST', '/sys/udp/send', {'path': '/led/brightness', 'value': json.dumps(want),
                                                            'to': b_ip}, timeout=15)
    except OSError as e:
        status, body = None, str(e)
    took = (time.time() - t) * 1000
    check(f'cable_write_{a_name}_to_{b_name}', status == 200 and body == want, (b_ip, status, body))
    print(f'  {a_name} -> {b_name} at {b_ip}: {took:.0f} ms')
    got = (b.value('/led') or {}).get('brightness')
    check(f'cable_write_applied_{b_name}', got == want, got)
    b.set('/led/brightness', before)
    got = (b.value('/led') or {}).get('brightness')
    check(f'peer_led_restored_{b_name}', got == before, (before, got))
    # The node under /peers by the cable (TesserAPI's route names: the
    # interface whose subnet holds the node's address).
    def route():
        for n in a.value('/sys/peers/status') or []:
            if n.get('name') == b_name and 'udp/ethernet' in (n.get('routes') or ''):
                return n
        return None
    n = wait_for(f'{b_name} under {a_name}/peers by udp/ethernet', route, timeout=20, step=2)
    check(f'peer_route_ethernet_{a_name}_{b_name}', n, a.value('/sys/peers/status'))


def run_link_local(d, args):
    """The cable without a DHCP server: see the module's docstring."""
    st_ip = d.base.split('://', 1)[-1].split('/')[0].split(':')[0]
    st = wait_for('a link-local address', lambda: (lambda s: s if eth_status_ok(s) else None)(
        d.value('/sys/network/ethernet/status')), timeout=30)
    st = st or d.value('/sys/network/ethernet/status')
    if not check('ethernet_tree', isinstance(st, dict), st):
        return
    check('link', st.get('link') is True, st)
    check('connected', st.get('connected') is True, st)
    check('link_local_address', link_local(st.get('ip')), st)
    check('link_local_state', 'link-local' in (st.get('state') or ''), st)
    check('speed', st.get('speed') in (10, 100), st)
    check('mac', len(st.get('mac', '')) == 17, st)
    wifi = d.value('/sys/network/wifi/status')
    check('station_beside', wifi is not None and wifi.get('connected') is True and wifi.get('ip') == st_ip, wifi)
    check('online', d.value('/sys/status/state') == 'online')
    heap = d.value('/sys/status/heap')
    print(f'  free heap: {heap} B, lowest: {d.value("/sys/status/minHeap")} B')
    hostname = d.value('/sys/network/host') or d.value('/sys/network/hostname')
    if d.value('/sys/network/mdns'):
        got = wait_for(f'{hostname}.local', lambda: resolve(f'{hostname}.local'), timeout=15, step=2)
        check('mdns_station', got == st_ip, got)

    peer, cabled = None, False
    if args.peer:
        peer = OverWifi(args.peer)
        if peer.value('/sys/auth/open') is False:
            status, body = peer.request('POST', '/sys/auth/login', {'name': args.user, 'password': args.password})
            if not check('peer_login', status == 200, body):
                return
            peer.token = body['token']
        pst = wait_for('the peer\'s link-local address', lambda: (lambda s: s if eth_status_ok(s) else None)(
            peer.value('/sys/network/ethernet/status')), timeout=30)
        check('peer_link_local_address', pst, peer.value('/sys/network/ethernet/status'))
        if pst:
            cabled = True
            check('distinct_addresses', pst['ip'] != st.get('ip'), (st.get('ip'), pst['ip']))
            cable_writes(d, peer)
            cable_writes(peer, d)
    else:
        print('SKIP cable: no --peer (the device at the other end of the cable)')

    # A restart: the cable's address again, the station beside it, and the
    # clock from SNTP, which a default route over a cable without a gateway
    # never reached (DESIGN.md 5.2, "Two interfaces at once").
    # Over Wi-Fi the reply has once taken longer than 5 s (2026-11-26):
    # 15 s for it, and the uptime says whether the device restarted.
    t0 = time.time()
    try:
        status = d.request('POST', '/sys/restart', timeout=15)[0]
    except OSError as e:
        status = e
    check('restart', status == 200, status)
    time.sleep(3)
    st = wait_for('a link-local address again', lambda: (lambda s: s if eth_status_ok(s) else None)(
        d.value('/sys/network/ethernet/status')), timeout=60)
    check('restart_back', st, d.value('/sys/network/ethernet/status'))
    uptime = d.value('/sys/status/uptime')
    since = time.time() - t0
    print(f'  reachable {since:.1f} s after the restart request; uptime {uptime} s')
    check('restarted', isinstance(uptime, int) and uptime <= since, (uptime, since))
    wifi = d.value('/sys/network/wifi/status')
    check('station_beside_after_boot', wifi is not None and wifi.get('connected') is True, wifi)
    tm = wait_for('the clock from SNTP', lambda: (lambda t: t if t and t.get('synced') and t.get('source') == 'sntp'
                                                   else None)(d.value('/sys/time')), timeout=60, step=2)
    check('internet_beside_cable', tm, d.value('/sys/time'))
    if cabled and st:
        cable_writes(peer, d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('url')
    ap.add_argument('--static', help='a free address on the same /24, for the static test')
    ap.add_argument('--unplug', action='store_true', help='interactive: pull the cable and put it back')
    ap.add_argument('--user', default='e2e')
    ap.add_argument('--password', default='e2e-pass')
    ap.add_argument('--link-local', action='store_true',
                    help='no DHCP server on the cable: the URL is the station\'s address')
    ap.add_argument('--peer', help='with --link-local: the station URL of the device at the cable\'s other end')
    args = ap.parse_args()
    if args.link_local and (args.static or args.unplug):
        ap.error('--static and --unplug need a DHCP server on the cable, not --link-local')
    if args.peer and not args.link_local:
        ap.error('--peer goes with --link-local')
    d = (OverWifi if args.link_local else Device)(args.url)
    eth_ip = args.url.split('://', 1)[-1].split('/')[0].split(':')[0]

    if d.value('/sys/auth/open') is False:
        status, body = d.request('POST', '/sys/auth/login', {'name': args.user, 'password': args.password})
        if not check('login', status == 200, body):
            return 1
        d.token = body['token']

    if args.link_local:
        run_link_local(d, args)
        print(f'\n{e2e.PASS} passed, {e2e.FAIL} failed')
        return 1 if e2e.FAIL else 0

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
        got = wait_for(f'back at {eth_ip}', lambda: d.value('/sys/network/ethernet/status/ip') == eth_ip and eth_ip,
                       timeout=30)
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

    print(f'\n{e2e.PASS} passed, {e2e.FAIL} failed')
    return 1 if e2e.FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
