#!/usr/bin/env python3
"""End-to-end checks of updates from GitHub releases (/sys/firmware/github).

Usage:
  python3 test/e2e_github.py http://<ip> --image <newer .bin> --tag v9.9.9 [--user e2e --password e2e-pass]
  python3 test/e2e_github.py http://<ip> --check-only

With --image, the device runs examples/idf/minimal built with -DWITH_GITHUB=1
and GITHUB_REPO set to this script's release endpoint,
"http://<this machine>:8765/latest" (GITHUB_ASSET "fw-{target}.bin"), in
secrets.h. The script serves a release tagged --tag whose asset is --image
(the same example built with -DPROJECT_VER set to the tag), checks it, installs
it, and waits for the device to come back running it and up to date.

With --check-only, the device checks whatever its feed is (GitHub's API, say
wled/WLED, whose 72 KB answer tests streaming over TLS) and the result is
printed: nothing is installed.
"""
import argparse
import http.server
import json
import os
import socket
import sys
import threading
import time
import urllib.error
import urllib.request

PASS = 0
FAIL = 0
PORT = 8765


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

    token = None

    def request(self, method, path, body=None, timeout=10):
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
            status, body = self.request('GET', path, timeout=5)
        except (OSError, urllib.error.URLError):
            return None  # restarting
        return body if status == 200 else None

    def sign_in(self, user, password):
        status, body = self.request('POST', '/sys/auth/login', {'user': user, 'password': password})
        if status == 200:
            self.token = body['token']
        return status


def wait_for(fn, timeout=5.0, step=0.2):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


def local_ip(towards):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((towards, 80))
        return s.getsockname()[0]
    finally:
        s.close()


def serve_release(tag, image, target, base):
    """A latest-release endpoint answering as GitHub's does, and the asset."""
    asset = f'fw-{target}.bin'
    with open(image, 'rb') as f:
        data = f.read()
    release = {
        'tag_name': tag,
        'name': tag,
        'body': 'x' * 5000,
        'assets': [
            {'name': 'fw-other.bin', 'browser_download_url': f'{base}/download/{tag}/fw-other.bin'},
            {'name': asset, 'size': len(data), 'browser_download_url': f'{base}/download/{tag}/{asset}'},
        ],
    }
    hits = {'latest': 0, 'asset': 0}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if self.path == '/latest':
                hits['latest'] += 1
                body = json.dumps(release).encode()
                ctype = 'application/json'
            elif self.path == f'/download/{tag}/{asset}':
                hits['asset'] += 1
                body = data
                ctype = 'application/octet-stream'
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(('0.0.0.0', PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, hits


def check_now(dev):
    t0 = time.time()
    status, body = dev.request('POST', '/sys/firmware/github/check', timeout=20)
    return status, body, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('url')
    ap.add_argument('--image', help='the newer image the release offers')
    ap.add_argument('--tag', default='v9.9.9')
    ap.add_argument('--target', default='esp32', help='the chip, as the asset name has it')
    ap.add_argument('--check-only', action='store_true')
    ap.add_argument('--user')
    ap.add_argument('--password')
    args = ap.parse_args()
    dev = Device(args.url)
    if args.user:
        check('sign in', dev.sign_in(args.user, args.password) == 200)

    fw = dev.value('/sys/firmware')
    if not check('firmware has a release feed', fw and 'github' in fw, fw):
        return
    print(f"running {fw['version']}, feed {fw['github']['repo']}")
    heap_before = dev.value('/sys/status/heap')

    if args.check_only:
        status, body, took = check_now(dev)
        g = dev.value('/sys/firmware/github')
        check('check answers', status == 200, (status, body))
        print(f'check took {took:.1f} s: {body}; status {g["status"]}, error {g.get("error")!r}')
        heap_after = dev.value('/sys/status/heap')
        print(f'free heap {heap_before} before, {heap_after} after; '
              f'minimum {dev.value("/sys/status/minHeap")}')
        return

    target = args.target
    base = f'http://{local_ip(dev.host)}:{PORT}'
    server, hits = serve_release(args.tag, args.image, target, base)
    try:
        status, body, took = check_now(dev)
        check('check finds the release', status == 200 and body == args.tag, (status, body))
        print(f'  check took {took:.1f} s')
        g = dev.value('/sys/firmware/github')
        check('release offered', g['status'] == 'available' and g['latest'] == args.tag, g)
        check('one request to the endpoint', hits['latest'] == 1, hits)

        status, body = dev.request('POST', '/sys/firmware/github/update')
        check('update starts', status == 200, (status, body))
        t0 = time.time()
        seen = set()

        def done():
            st = dev.value('/sys/firmware/state')
            if st:
                seen.add(st)
            return st in ('rebooting', 'failed') or (st is None and 'downloading' in seen)

        wait_for(done, timeout=120, step=0.3)
        print(f'  states seen: {sorted(seen)}; asset served {hits["asset"]} time(s), {time.time() - t0:.1f} s')
        check('image downloaded', hits['asset'] == 1, hits)
        check('no failure', 'failed' not in seen, dev.value('/sys/firmware/error'))

        version = args.tag.lstrip('v')
        back = wait_for(lambda: dev.value('/sys/firmware/version') == version, timeout=60, step=1)
        check(f'device runs {version}', back, dev.value('/sys/firmware/version'))
        check('new image pending until confirmed', dev.value('/sys/firmware/pending') is True,
              dev.value('/sys/firmware/pending'))
        if args.user:
            dev.sign_in(args.user, args.password)
        status, body, _ = check_now(dev)
        g = dev.value('/sys/firmware/github')
        check('up to date afterwards', status == 200 and g['status'] == 'current', (status, body, g))
        check('update not offered', dev.request('POST', '/sys/firmware/github/update')[0] == 405,
              dev.request('POST', '/sys/firmware/github/update'))
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
    print(f'\n{PASS} passed, {FAIL} failed')
    sys.exit(1 if FAIL else 0)
