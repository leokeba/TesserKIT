#!/usr/bin/env python3
"""End-to-end checks of TesserKIT's Files service and stored backups on a device.

Usage:
  python3 test/e2e_files.py http://<ip> [--user admin --password ...]

The device runs examples/idf/full (Files on partitions/tesserkit_4mb_files.csv,
Backup). The test leaves the filesystem as it found it, but for the stored
backups it makes, which it deletes, and before-restore, which a restore
leaves. It restores the settings it saved first, with restartAfter off, so
the device doesn't restart.
"""
import argparse
import json
import os
import socket
import sys
import time
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


class Device:
    def __init__(self, base):
        self.base = base.rstrip('/')

    token = None  # sent as Authorization: Bearer when set

    def raw(self, method, path, data=None, timeout=20):
        headers = {'Authorization': 'Bearer ' + self.token} if self.token else {}
        req = urllib.request.Request(self.base + '/api' + path, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read(), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    def request(self, method, path, body=None, timeout=20):
        data = None if body is None else json.dumps(body).encode()
        status, payload, _ = self.raw(method, path, data, timeout)
        return status, json.loads(payload) if payload else None

    def value(self, path):
        status, body = self.request('GET', path, timeout=5)
        return body if status == 200 else None

    def call(self, path, body=None):
        return self.request('POST', path, body)

    def upload(self, path, data, filename=None):
        if filename is not None:
            path += '?filename=' + urllib.parse.quote(filename)
        status, payload, _ = self.raw('PUT', path, data, timeout=60)
        return status, json.loads(payload) if payload else None

    # Sends only a PUT's headers, announcing `size` bytes; the status.
    def announce(self, path, size):
        host, _, port = self.base.split('://', 1)[1].partition(':')
        with socket.create_connection((host, int(port or 80)), timeout=20) as sock:
            auth = f'Authorization: Bearer {self.token}\r\n' if self.token else ''
            sock.sendall(f'PUT /api{path} HTTP/1.1\r\nHost: {host}\r\n{auth}Content-Length: {size}\r\n\r\n'.encode())
            head = sock.recv(1024).decode(errors='replace')
        return int(head.split(' ', 2)[1]) if head.startswith('HTTP/') else None

    def download(self, path):
        status, payload, headers = self.raw('GET', path, timeout=60)
        return status, payload, headers


def entry(name):
    return '/sys/files/entries/' + urllib.parse.quote(name, safe='')


def names(listing):
    return [e['name'] for e in listing or []]


def test_files(dev):
    print('-- files')
    check('mounted', dev.value('/sys/files/mounted') is True, dev.value('/sys/files'))
    total = dev.value('/sys/files/total')
    check('total', total and total > 300000, total)
    check('dir at the root', dev.call('/sys/files/dir', '/')[0] == 200)
    base = 'e2e-' + str(int(time.time()))
    check('mkdir', dev.call('/sys/files/mkdir', base)[0] == 200)
    check('open', dev.call(entry(base) + '/open')[0] == 200)
    check('dir', dev.value('/sys/files/dir') == '/' + base, dev.value('/sys/files/dir'))

    # A file of several transfer pieces, timed both ways.
    blob = os.urandom(100 * 1024)
    t = time.time()
    status, body = dev.upload('/sys/files/upload', blob, 'blob.bin')
    up = time.time() - t
    check('upload', status == 200, (status, body))
    print(f'     upload {len(blob) / up / 1024:.0f} KB/s')
    listing = dev.value('/sys/files/entries')
    check('listed', 'blob.bin' in names(listing), listing)
    check('size', dev.value(entry('blob.bin') + '/size') == len(blob), dev.value(entry('blob.bin')))
    check('modified', (dev.value(entry('blob.bin') + '/modified') or 0) > 1577836800, dev.value(entry('blob.bin')))
    t = time.time()
    status, data, headers = dev.download(entry('blob.bin') + '/content')
    down = time.time() - t
    check('download', status == 200 and data == blob, (status, len(data)))
    print(f'     download {len(blob) / down / 1024:.0f} KB/s')
    check('download name', 'filename="blob.bin"' in headers.get('Content-Disposition', ''), headers)

    # A name with a space and non-ASCII letters.
    odd = 'été notes.txt'
    status, body = dev.upload('/sys/files/upload', 'bonjour'.encode(), odd)
    check('upload odd name', status == 200, (status, body))
    status, data, headers = dev.download(entry(odd) + '/content')
    check('download odd name', status == 200 and data == b'bonjour', (status, data))
    check('odd name header', "filename*=UTF-8''" in headers.get('Content-Disposition', ''), headers)

    # Refusals.
    check('no name', dev.upload('/sys/files/upload', b'x')[0] == 422)
    check('bad name', dev.upload('/sys/files/upload', b'x', '../escape')[0] == 422)
    check('hidden name', dev.upload('/sys/files/upload', b'x', '.hidden')[0] == 422)
    schema = dev.request('GET', '/sys/files/upload?view=schema')[1]
    check('max size', schema.get('maxSize') == total, schema)
    # Refused on the announced size. A refused body is drained up to 256 KB
    # (then the connection is closed before the client reads the reply): a
    # filler file first, so what is refused is below that.
    filler = b'f' * 120 * 1024
    check('filler', dev.upload('/sys/files/upload', filler, 'filler.bin')[0] == 200)
    free = total - dev.value('/sys/files/used')
    check('too large', dev.upload('/sys/files/upload', b'x' * (free + 1), 'huge.bin')[0] == 413, free)
    check('nothing left behind', 'huge.bin' not in names(dev.value('/sys/files/entries')))
    dev.call(entry('filler.bin') + '/delete')
    check('missing', dev.download(entry('nope.txt') + '/content')[0] == 404)

    # Replace, rename, delete.
    check('replace', dev.upload(entry('blob.bin') + '/content', b'small')[0] == 200)
    check('replaced', dev.download(entry('blob.bin') + '/content')[1] == b'small')
    check('rename', dev.call(entry('blob.bin') + '/rename', 'renamed.bin')[0] == 200)
    check('renamed', names(dev.value('/sys/files/entries')) == ['renamed.bin', odd], dev.value('/sys/files/entries'))
    check('up', dev.call('/sys/files/up')[0] == 200 and dev.value('/sys/files/dir') == '/')
    check('delete dir', dev.call(entry(base) + '/delete')[0] == 200)
    check('deleted', base not in names(dev.value('/sys/files/entries')), dev.value('/sys/files/entries'))


def test_backups(dev):
    print('-- stored backups')
    restart = dev.value('/sys/backup/restartAfter')
    dev.call('/sys/backup/restartAfter', False)
    status, name = dev.call('/sys/backup/save', '')
    check('save, named after the date', status == 200 and name[:2] == '20', (status, name))
    stored = dev.value('/sys/backup/stored')
    check('stored', name in names(stored), stored)
    status, data, headers = dev.download('/sys/backup/stored/' + name + '/content')
    check('download stored', status == 200 and json.loads(data).get('sys') is not None, status)
    check('stored name', name + '.json"' in headers.get('Content-Disposition', ''), headers)
    status, body = dev.call('/sys/backup/stored/' + name + '/restore')
    check('restore stored', status == 200 and body == {'skipped': [], 'restart': False}, (status, body))
    check('before-restore kept', 'before-restore' in names(dev.value('/sys/backup/stored')))
    check('delete stored', dev.call('/sys/backup/stored/' + name + '/delete')[0] == 200)
    check('gone', name not in names(dev.value('/sys/backup/stored')))
    dev.call('/sys/backup/restartAfter', restart)
    # The device is still up: nothing restarted.
    check('still up', dev.value('/sys/status/uptime') is not None)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('base')
    p.add_argument('--user')
    p.add_argument('--password')
    args = p.parse_args()
    dev = Device(args.base)
    if args.user:
        status, body = dev.request('POST', '/sys/auth/login', {'name': args.user, 'password': args.password})
        if not check('login', status == 200, body):
            sys.exit(1)
        dev.token = body['token']
    test_files(dev)
    test_backups(dev)
    print(f'DONE pass={PASS} fail={FAIL}')
    sys.exit(1 if FAIL else 0)


if __name__ == '__main__':
    main()
