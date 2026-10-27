#!/usr/bin/env python3
"""End-to-end checks of TesserKIT's ArtNet and Mqtt services on a device.

Usage:
  uv run --with paho-mqtt python test/e2e_services.py http://<ip> [--broker <host>[:port]] [--only artnet|mqtt]
      [--user e2e --password e2e-pass]

The device runs examples/idf/full (or examples/idf/minimal built with
-DWITH_ARTNET=1 -DWITH_MQTT=1): Art-Net universe 0, channel 1 mapped to
/led/brightness and channel 2 gating /led/on; Mqtt publishing /led and
taking commands. Without --broker only the Art-Net checks run. The broker
must accept anonymous clients (for instance `uvx --from amqtt amqtt`) and be
reachable at the same address from this machine and from the device: a
device not already connected to it (MQTT_URI in the example's secrets.h) is
pointed at it, and its MQTT settings are put back after. The device restarts
once. A device with an admin account (as test/e2e_device.py leaves it) is
signed in to with --user and --password.
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

    def set(self, path, body):
        return self.request('POST', path, body)[0]


def wait_for(fn, timeout=5.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


# ---- Art-Net ---------------------------------------------------------------

def art_dmx(universe, channels, sequence=1):
    data = bytes(channels) + (b'\0' if len(channels) % 2 else b'')
    return (b'Art-Net\0' + struct.pack('<H', 0x5000) + struct.pack('>H', 14) +
            bytes([sequence, 0]) + struct.pack('<H', universe) + struct.pack('>H', len(data)) + data)


def art_poll():
    return b'Art-Net\0' + struct.pack('<H', 0x2000) + struct.pack('>H', 14) + bytes([0, 0])


def test_artnet(dev):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', 6454))  # replies come to the Art-Net port
    sock.settimeout(2)
    target = (dev.host, 6454)
    before = dev.value('/sys/artnet/status/packets') or 0

    sock.sendto(art_dmx(0, [100, 200]), target)
    led = wait_for(lambda: (lambda v: v if v and v['brightness'] == 100 else None)(dev.value('/led')))
    check('artnet_channel_to_brightness', led and led['brightness'] == 100, led)
    check('artnet_gate_on', led and led['on'] is True, led)

    sock.sendto(art_dmx(0, [37, 10]), target)
    led = wait_for(lambda: (lambda v: v if v and not v['on'] else None)(dev.value('/led')))
    check('artnet_gate_off', led and led['on'] is False and led['brightness'] == 37, led)

    sock.sendto(art_dmx(5, [250, 250]), target)  # another universe
    time.sleep(0.5)
    led = dev.value('/led')
    check('artnet_other_universe_ignored', led and led['brightness'] == 37, led)
    st = dev.value('/sys/artnet/status')
    check('artnet_status', st and st['packets'] >= before + 3 and st['universe'] == 5 and st['age'] >= 0, st)

    # A stream at 40 frames a second, as a console sends.
    t0 = time.time()
    for i in range(80):
        sock.sendto(art_dmx(0, [i + 1, 255], sequence=i % 255 + 1), target)
        time.sleep(0.025)
    led = wait_for(lambda: (lambda v: v if v and v['brightness'] == 80 else None)(dev.value('/led')))
    check('artnet_stream_last_value', led and led['brightness'] == 80 and led['on'], led)
    print(f'     ({80 / (time.time() - t0):.0f} frames/s sent)')

    sock.sendto(art_poll(), target)
    reply = None
    end = time.time() + 3
    while time.time() < end:
        try:
            data, addr = sock.recvfrom(1024)
        except socket.timeout:
            break
        if addr[0] == dev.host and data[8:10] == struct.pack('<H', 0x2100):
            reply = data
            break
    if check('artnet_poll_reply', reply is not None and len(reply) == 239, reply and len(reply)):
        ip = '.'.join(str(b) for b in reply[10:14])
        short = reply[26:44].split(b'\0')[0].decode()
        long_ = reply[44:108].split(b'\0')[0].decode()
        # The short name is the device's name (/sys/name, else its label and
        # MAC end), cut to 17 characters; the long one its hostname.
        name = dev.value('/sys/name') or ''
        host = dev.value('/sys/network/host') or dev.value('/sys/network/hostname')
        check('artnet_poll_reply_fields', ip == dev.host and short and (not name or short == name[:17]) and
              long_ == host, (ip, short, long_, name, host))
    sock.close()


# ---- MQTT ------------------------------------------------------------------

class Broker:
    """A client of the broker, recording every message on the device's topics."""

    def __init__(self, host, port):
        import paho.mqtt.client as mqtt
        self.messages = []
        self.lock = threading.Lock()
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_message = self.on_message
        self.client.connect(host, port)
        self.client.loop_start()

    def on_message(self, client, userdata, msg):
        with self.lock:
            self.messages.append((msg.topic, msg.payload.decode(errors='replace'), msg.retain, time.time()))

    def subscribe(self, topic):
        self.client.subscribe(topic, qos=1)

    def publish(self, topic, payload):
        self.client.publish(topic, payload, qos=1)

    def last(self, topic, since=0):
        with self.lock:
            for t, p, _, at in reversed(self.messages):
                if t == topic and at >= since:
                    return p
        return None

    def wait(self, topic, payload=None, since=0, timeout=5.0):
        return wait_for(lambda: (lambda v: v if v is not None and (payload is None or v == payload) else None)(
            self.last(topic, since)), timeout)


def test_mqtt(dev, host, port):
    hostname = dev.value('/sys/network/host') or dev.value('/sys/network/hostname')
    pre = f'tesser/{hostname}'
    # Pointed at the broker unless it already is (MQTT_URI), and put back after.
    before = dev.value('/sys/mqtt') or {}
    ip = socket.gethostbyname(host)
    uri = f'mqtt://{ip}:{port}'
    restore = None
    if not (before.get('enabled') and before.get('uri') in (uri, f'mqtt://{host}:{port}', f'mqtt://{host}') and
            not before.get('prefix')):
        restore = {k: before[k] for k in ('uri', 'enabled', 'prefix') if k in before}
        status = dev.request('PATCH', '/sys/mqtt', {'uri': uri, 'enabled': True, 'prefix': ''})[0]
        check('mqtt_configured', status == 200, status)
    st = wait_for(lambda: (lambda v: v if v and v['connected'] else None)(dev.value('/sys/mqtt/status')), 15)
    check('mqtt_connected', st and st['connected'], st)

    b = Broker(host, port)
    b.subscribe(pre + '/#')
    check('mqtt_online_retained', b.wait(pre + '/status', 'online') == 'online', b.last(pre + '/status'))
    led = dev.value('/led')
    check('mqtt_leaves_retained', b.wait(pre + '/led/brightness') == str(led['brightness']),
          (b.last(pre + '/led/brightness'), led))
    check('mqtt_status_published', b.wait(pre + '/sys/status/state') == '"online"', b.last(pre + '/sys/status/state'))

    t = time.time()
    dev.set('/led/brightness', 77)
    check('mqtt_change_published', b.wait(pre + '/led/brightness', '77', since=t) == '77', b.last(pre + '/led/brightness'))

    t = time.time()
    b.publish(pre + '/led/brightness/set', '42')
    result = b.wait(pre + '/led/brightness/result', since=t)
    check('mqtt_command_result', result and json.loads(result)['status'] == 'ok', result)
    check('mqtt_command_applied', wait_for(lambda: (dev.value('/led') or {}).get('brightness') == 42), dev.value('/led'))

    t = time.time()
    b.publish(pre + '/led/on/set', 'false')
    b.wait(pre + '/led/on/result', since=t)
    check('mqtt_command_boolean', (dev.value('/led') or {}).get('on') is False, dev.value('/led'))

    t = time.time()
    b.publish(pre + '/sys/mqtt/enabled/set', 'false')
    result = b.wait(pre + '/sys/mqtt/enabled/result', since=t)
    check('mqtt_command_user_access', result and json.loads(result)['status'] == 'unauthorized', result)
    check('mqtt_still_enabled', dev.value('/sys/mqtt/enabled') is True)

    # New prefix: the session restarts, the old one says offline.
    t = time.time()
    b.subscribe('e2e/lamp/#')
    check('mqtt_prefix_set', dev.set('/sys/mqtt/prefix', 'e2e/lamp') == 200)
    check('mqtt_old_session_offline', b.wait(pre + '/status', 'offline', since=t, timeout=10) == 'offline',
          b.last(pre + '/status'))
    check('mqtt_new_prefix_online', b.wait('e2e/lamp/status', 'online', since=t, timeout=15) == 'online',
          b.last('e2e/lamp/status'))
    check('mqtt_new_prefix_leaves', b.wait('e2e/lamp/led/brightness', since=t) == '42', b.last('e2e/lamp/led/brightness'))
    dev.set('/sys/mqtt/prefix', '')
    check('mqtt_prefix_back', b.wait(pre + '/status', 'online', since=time.time(), timeout=15) == 'online')

    # A restart: the device comes back online. It reconnects with the same
    # client ID before the broker misses it, so whether the will shows in
    # between is the broker's choice (a session taken over).
    t = time.time()
    dev.set('/sys/restart', None)
    time.sleep(3)
    check('mqtt_online_after_restart', b.wait(pre + '/status', 'online', since=t, timeout=60) == 'online',
          b.last(pre + '/status'))
    if restore is not None:
        check('mqtt_settings_restored', dev.request('PATCH', '/sys/mqtt', restore)[0] == 200)
    b.client.loop_stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('url')
    ap.add_argument('--broker', help='host[:port] of an anonymous MQTT broker; without it, MQTT is skipped')
    ap.add_argument('--only', choices=['artnet', 'mqtt'])
    ap.add_argument('--user', default='e2e')
    ap.add_argument('--password', default='e2e-pass')
    args = ap.parse_args()
    dev = Device(args.url)
    if dev.value('/sys/auth/open') is False:
        status, body = dev.request('POST', '/sys/auth/login', {'name': args.user, 'password': args.password})
        if not check('login', status == 200, body):
            sys.exit(1)
        dev.token = body['token']
    if args.only == 'mqtt' and not args.broker:
        sys.exit('--only mqtt needs --broker')
    if args.only in (None, 'artnet'):
        test_artnet(dev)
    if args.only in (None, 'mqtt'):
        if args.broker:
            host, _, port = args.broker.partition(':')
            test_mqtt(dev, host, int(port or 1883))
        else:
            print('SKIP mqtt: no --broker')
    print(f'DONE pass={PASS} fail={FAIL}')
    sys.exit(1 if FAIL else 0)


if __name__ == '__main__':
    main()
