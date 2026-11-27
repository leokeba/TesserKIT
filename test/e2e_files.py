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
import urllib.parse
import urllib.request

import e2e_http as e2e
from e2e_http import check


class Device(e2e.Device):
    def __init__(self, base):
        super().__init__(base, timeout=20)

    def raw(self, method, path, data=None, timeout=20):
        """(status, payload, headers); (None, b'', {}) after a FAIL line when
        the device didn't answer."""
        try:
            status, headers, payload = self.exchange(method, path, data, timeout)
            return status, payload, dict(headers)
        except e2e.Unreachable as e:
            e2e.failed(method, path, e)
            return None, b'', {}

    def call(self, path, body=None):
        return self.request('POST', path, body)

    def upload(self, path, data, filename=None):
        if filename is not None:
            path += '?filename=' + urllib.parse.quote(filename)
        status, payload, _ = self.raw('PUT', path, data, timeout=60)
        try:
            return status, e2e.parse(payload)
        except e2e.BadReply as e:
            e2e.failed('PUT', path, e)
            return None, None

    # Sends only a PUT's headers, announcing `size` bytes; the status.
    def announce(self, path, size):
        host, _, port = self.base.split('://', 1)[1].partition(':')
        try:
            with socket.create_connection((host, int(port or 80)), timeout=20) as sock:
                auth = f'Authorization: Bearer {self.token}\r\n' if self.token else ''
                sock.sendall(f'PUT /api{path} HTTP/1.1\r\nHost: {host}\r\n{auth}Content-Length: {size}\r\n\r\n'
                             .encode())
                head = sock.recv(1024).decode(errors='replace')
        except OSError as e:
            e2e.failed('PUT', path, e2e.describe(e))
            return None
        return int(head.split(' ', 2)[1]) if head.startswith('HTTP/') else None

    def download(self, path):
        status, payload, headers = self.raw('GET', path, timeout=60)
        return status, payload, headers


FILE = '/sys/files/file'


def names(listing):
    return [e['name'] for e in listing or []]


def ls(dev, folder):
    status, body = dev.call('/sys/files/list', folder)
    return body['entries'] if status == 200 else None


def entry(dev, folder, name):
    return next((e for e in ls(dev, folder) or [] if e['name'] == name), None)


def get(dev, path):
    return dev.download(FILE + '?filename=' + urllib.parse.quote(path, safe='/'))


def put(dev, path, data):
    return dev.upload(FILE, data, path)


def test_files(dev):
    print('-- files')
    check('mounted', dev.value('/sys/files/mounted') is True, dev.value('/sys/files'))
    total = dev.value('/sys/files/total')
    check('total', total and total > 300000, total)
    base = '/e2e-' + str(int(time.time()))
    check('mkdir', dev.call('/sys/files/mkdir', base)[0] == 200)
    check('mkdir taken', dev.call('/sys/files/mkdir', base)[0] == 422)
    check('listed folder', (entry(dev, '/', base[1:]) or {}).get('dir') is True, ls(dev, '/'))

    # A file of several transfer pieces, timed both ways.
    blob = os.urandom(100 * 1024)
    t = time.time()
    status, body = put(dev, base + '/blob.bin', blob)
    up = time.time() - t
    check('upload', status == 200, (status, body))
    print(f'     upload {len(blob) / up / 1024:.0f} KB/s')
    e = entry(dev, base, 'blob.bin') or {}
    check('listed', e.get('size') == len(blob), ls(dev, base))
    check('modified', (e.get('modified') or 0) > 1577836800, e)
    t = time.time()
    status, data, headers = get(dev, base + '/blob.bin')
    down = time.time() - t
    check('download', status == 200 and data == blob, (status, len(data)))
    print(f'     download {len(blob) / down / 1024:.0f} KB/s')
    check('download name', 'filename="blob.bin"' in headers.get('Content-Disposition', ''), headers)

    # A name with a space and non-ASCII letters.
    odd = 'été notes.txt'
    status, body = put(dev, base + '/' + odd, 'bonjour'.encode())
    check('upload odd name', status == 200, (status, body))
    status, data, headers = get(dev, base + '/' + odd)
    check('download odd name', status == 200 and data == b'bonjour', (status, data))
    check('odd name header', "filename*=UTF-8''" in headers.get('Content-Disposition', ''), headers)

    # Refusals.
    check('no name', dev.upload(FILE, b'x')[0] == 422)
    check('bad name', put(dev, '/../escape', b'x')[0] == 422)
    check('hidden name', put(dev, '/.hidden', b'x')[0] == 422)
    check('no folder', put(dev, '/nowhere/a.txt', b'x')[0] == 404)
    check('no such folder', dev.call('/sys/files/list', '/nowhere')[0] == 404)
    schema = dev.request('GET', FILE + '?view=schema')[1]
    # Guarded: when both reads failed, maxSize == total would be None == None.
    check('max size', isinstance(schema, dict) and schema.get('maxSize') == total and total is not None, schema)
    # Refused on the announced size. A refused body is drained up to 256 KB
    # (then the connection is closed before the client reads the reply): a
    # filler file first, so what is refused is below that.
    # Smaller than Files keeps for the settings (64 KB) and what's stored, so
    # it fits a 384 KB partition beside them.
    filler = b'f' * 64 * 1024
    got = put(dev, base + '/filler.bin', filler)
    check('filler', got[0] == 200, got)
    used = dev.value('/sys/files/used')
    free = total - used if total is not None and used is not None else None
    check('too large', free is not None and put(dev, base + '/huge.bin', b'x' * (free + 1))[0] == 413, free)
    listing = ls(dev, base)
    check('nothing left behind', listing is not None and 'huge.bin' not in names(listing), listing)
    dev.call('/sys/files/delete', base + '/filler.bin')
    check('missing', get(dev, base + '/nope.txt')[0] == 404)

    # Replace, move, delete.
    check('replace', put(dev, base + '/blob.bin', b'small')[0] == 200)
    check('replaced', get(dev, base + '/blob.bin')[1] == b'small')
    check('rename', dev.call('/sys/files/move', {'from': base + '/blob.bin', 'to': base + '/renamed.bin'})[0] == 200)
    check('renamed', names(ls(dev, base)) == ['renamed.bin', odd], ls(dev, base))
    check('move', dev.call('/sys/files/move', {'from': base + '/renamed.bin', 'to': '/moved.bin'})[0] == 200)
    check('moved', 'moved.bin' in names(ls(dev, '/')) and get(dev, '/moved.bin')[1] == b'small')
    dev.call('/sys/files/delete', '/moved.bin')
    check('delete folder', dev.call('/sys/files/delete', base)[0] == 200)
    listing = ls(dev, '/')
    check('deleted', listing is not None and base[1:] not in names(listing), listing)


def test_backups(dev):
    print('-- stored backups')
    restart = dev.value('/sys/backup/restartAfter')
    dev.call('/sys/backup/restartAfter', False)
    status, name = dev.call('/sys/backup/save', '')
    check('save, named after the date', status == 200 and isinstance(name, str) and name[:2] == '20', (status, name))
    if status != 200 or not isinstance(name, str):  # refused (busy on a short heap): nothing to go on with
        dev.call('/sys/backup/restartAfter', restart)
        return
    stored = dev.value('/sys/backup/stored')
    check('stored', name in names(stored), stored)
    status, data, headers = dev.download('/sys/backup/stored/' + name + '/content')
    try:
        stored = json.loads(data) if data else None  # an empty or non-JSON body fails the check below
    except ValueError:
        stored = None
    check('download stored', status == 200 and isinstance(stored, dict) and stored.get('sys') is not None, status)
    check('stored name', name + '.json"' in headers.get('Content-Disposition', ''), headers)
    status, body = dev.call('/sys/backup/stored/' + name + '/restore')
    check('restore stored', status == 200 and body == {'skipped': [], 'restart': False}, (status, body))
    check('before-restore kept', 'before-restore' in names(dev.value('/sys/backup/stored')))
    check('delete stored', dev.call('/sys/backup/stored/' + name + '/delete')[0] == 200)
    stored = dev.value('/sys/backup/stored')
    check('gone', stored is not None and name not in names(stored), stored)
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
    print(f'DONE pass={e2e.PASS} fail={e2e.FAIL}')
    sys.exit(1 if e2e.FAIL else 0)


if __name__ == '__main__':
    main()
