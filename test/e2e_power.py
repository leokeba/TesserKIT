#!/usr/bin/env python3
"""End-to-end checks of Log, Sleep, Battery and Home Assistant discovery.

Usage:
  uv run --with paho-mqtt python test/e2e_power.py http://<ip> --broker <host>[:port]
      [--only log|discovery|sleep|battery] [--user e2e --password e2e-pass]

The device runs examples/idf/full (Log, Sleep woken by GPIO 0, Mqtt with
Home Assistant discovery), built with -DBATTERY_GPIO=<n> for the battery
checks. The broker must accept anonymous clients (`uvx --from amqtt
amqtt`); the device is pointed at it for the discovery checks and its MQTT
settings are put back after. The device sleeps once, for a few seconds.
"""
import argparse
import json
import socket
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

    token = None

    def headers(self):
        return {'Authorization': 'Bearer ' + self.token} if self.token else {}

    def request(self, method, path, body=None, timeout=10):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.base + '/api' + path, data=data, method=method, headers=self.headers())
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                payload = r.read()
                ctype = r.headers.get('Content-Type', '')
                if payload and 'json' in ctype:
                    return r.status, json.loads(payload)
                return r.status, payload.decode(errors='replace') if payload else None
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b'null')

    def value(self, path):
        try:
            status, body = self.request('GET', path, timeout=5)
        except (OSError, urllib.error.URLError):
            return None  # asleep or restarting
        return body if status == 200 else None

    def set(self, path, body):
        return self.request('POST', path, body)[0]


def wait_for(fn, timeout=5.0, step=0.2):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


class Stream:
    """The device's streamed subscription to `path` (GET ?sub): its events."""

    def __init__(self, dev, path):
        self.events = []
        self.lock = threading.Lock()
        url = dev.base + '/api' + path + '?sub&changes=false&snapshot=false&interval=60000'
        req = urllib.request.Request(url, headers=dev.headers())
        self.resp = urllib.request.urlopen(req, timeout=30)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        try:
            for line in self.resp:
                line = line.strip()
                if not line:
                    continue
                msg = json.loads(line)
                if msg.get('op') == 'event':
                    with self.lock:
                        self.events.append(msg.get('body'))
        except (OSError, ValueError, AttributeError):  # closed under it
            pass

    def take(self):
        with self.lock:
            out, self.events = self.events, []
        return out

    def close(self):
        try:
            self.resp.close()
        except OSError:
            pass


class Broker:
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
            self.messages.append((msg.topic, msg.payload.decode(errors='replace'), time.time()))

    def subscribe(self, topic):
        self.client.subscribe(topic, qos=1)

    def publish(self, topic, payload):
        self.client.publish(topic, payload, qos=1)

    def last(self, topic, since=0):
        with self.lock:
            for t, p, at in reversed(self.messages):
                if t == topic and at >= since:
                    return p
        return None

    def wait(self, topic, payload=None, since=0, timeout=10.0):
        return wait_for(lambda: (lambda v: v if v is not None and (payload is None or v == payload) else None)(
            self.last(topic, since)), timeout)


# ---- Log -------------------------------------------------------------------

def test_log(dev):
    log = dev.value('/sys/log')
    check('log_level', log and log['level'] in ('error', 'warn', 'info', 'debug', 'verbose'), log)
    status, recent = dev.request('GET', '/sys/log/recent')
    check('log_recent_download', status == 200 and isinstance(recent, str) and '\n' in recent, (status, recent))
    check('log_recent_without_colours', isinstance(recent, str) and '\x1b' not in recent, repr(recent)[:200])
    first = recent.splitlines()[0] if isinstance(recent, str) and recent else ''
    check('log_recent_lines', first[:3] in ('E (', 'W (', 'I (', 'D (', 'V ('), first)

    # Above what the build keeps: refused (the example logs warnings).
    if log and log['level'] == 'warn':
        check('log_level_above_build_refused', dev.set('/sys/log/level', 'verbose') == 422)
    check('log_level_set', dev.set('/sys/log/level', 'error') == 200 and dev.value('/sys/log/level') == 'error')
    check('log_level_back', dev.set('/sys/log/level', log['level'] if log else 'warn') == 200)

    # Lines as events: an MQTT broker that can't be resolved logs errors.
    mqtt = dev.value('/sys/mqtt') or {}
    s = Stream(dev, '/sys/log')
    time.sleep(0.5)
    dev.request('PATCH', '/sys/mqtt', {'uri': 'mqtt://nowhere.invalid:1883', 'enabled': True})
    got = wait_for(lambda: [e for e in s.take() if isinstance(e, str)] or None, timeout=20)
    check('log_lines_as_events', got and any('mqtt' in e.lower() or 'tls' in e.lower() for e in got), got)
    check('log_event_shape', got and got[0][:3] in ('E (', 'W (', 'I (') and not got[0].endswith('\n'),
          got[0] if got else None)
    s.close()
    dev.request('PATCH', '/sys/mqtt', {'uri': mqtt.get('uri', ''), 'enabled': mqtt.get('enabled', False)})
    check('log_nothing_dropped', (dev.value('/sys/log/dropped') or 0) == 0, dev.value('/sys/log/dropped'))


# ---- Home Assistant discovery ----------------------------------------------

def test_discovery(dev, host, port):
    hostname = dev.value('/sys/network/host') or dev.value('/sys/network/hostname')
    before = dev.value('/sys/mqtt') or {}
    b = Broker(host, port)
    b.subscribe('homeassistant/#')
    pre = f'tesser/{hostname}'
    b.subscribe(pre + '/#')
    mac_ip = socket.gethostbyname(host) if not host[0].isdigit() else host
    t = time.time()
    status = dev.request('PATCH', '/sys/mqtt',
                         {'uri': f'mqtt://{mac_ip}:{port}', 'enabled': True, 'homeAssistant': True, 'prefix': ''})[0]
    check('discovery_configured', status == 200, status)
    st = wait_for(lambda: (lambda v: v if v and v['connected'] else None)(dev.value('/sys/mqtt/status')), 20)
    check('discovery_connected', st and st['connected'], st)

    def config(component, oid):
        raw = b.wait(f'homeassistant/{component}/{hostname}/{oid}/config', since=t, timeout=15)
        try:
            return json.loads(raw) if raw else None
        except ValueError:
            return None

    sw = config('switch', 'led_on')
    check('discovery_switch', sw and sw['command_topic'] == pre + '/led/on/set' and sw['state_topic'] == pre + '/led/on'
          and sw['payload_on'] == 'true', sw)
    check('discovery_device', sw and sw['device']['identifiers'] == [hostname] and
          sw['device'].get('configuration_url', '').startswith('http://'), sw and sw['device'])
    num = config('number', 'led_brightness')
    check('discovery_number', num and num['min'] == 0 and num['max'] == 255 and num['step'] == 5, num)
    heap = config('sensor', 'sys_status_heap')
    check('discovery_sensor', heap and heap['unit_of_measurement'] == 'B' and heap['entity_category'] == 'diagnostic',
          heap)
    state = config('sensor', 'sys_status_state')
    check('discovery_enum_sensor', state and state.get('device_class') == 'enum' and 'online' in state['options'],
          state)
    count = (dev.value('/sys/mqtt/status') or {}).get('entities')
    check('discovery_entities', count and count >= 10, count)
    check('discovery_nothing_dropped', (dev.value('/sys/mqtt/status') or {}).get('dropped') == 0,
          dev.value('/sys/mqtt/status'))

    # Home Assistant switches the LED as it would: "true" on the command topic.
    t2 = time.time()
    b.publish(pre + '/led/on/set', 'true')
    check('discovery_command', wait_for(lambda: (dev.value('/led') or {}).get('on') is True, 5), dev.value('/led'))
    check('discovery_state_back', b.wait(pre + '/led/on', 'true', since=t2) == 'true', b.last(pre + '/led/on'))
    t3 = time.time()
    b.publish(pre + '/led/brightness/set', '100')  # what the config's command_template makes of 100.0
    check('discovery_number_command', wait_for(lambda: (dev.value('/led') or {}).get('brightness') == 100, 5),
          (dev.value('/led'), b.last(pre + '/led/brightness/result', since=t3)))

    # Off: every config emptied, which removes the entities.
    t4 = time.time()
    dev.set('/sys/mqtt/homeAssistant', False)
    check('discovery_removed', b.wait(f'homeassistant/switch/{hostname}/led_on/config', '', since=t4) == '',
          b.last(f'homeassistant/switch/{hostname}/led_on/config', since=t4))
    check('discovery_entities_zero', wait_for(lambda: (dev.value('/sys/mqtt/status') or {}).get('entities') == 0),
          dev.value('/sys/mqtt/status'))
    dev.request('PATCH', '/sys/mqtt', {k: before[k] for k in ('uri', 'enabled', 'homeAssistant', 'prefix') if k in before})
    b.client.loop_stop()


# ---- Sleep -----------------------------------------------------------------

def test_sleep(dev):
    sl = dev.value('/sys/sleep')
    check('sleep_tree', sl and 'wakePin' in sl and 'wakeCause' in sl, sl)
    pin = sl['wakePin'] if sl else 0
    dev.set('/sys/sleep/wakePin', -1)
    check('sleep_needs_a_wake', dev.set('/sys/sleep/sleep', 0) == 422)
    check('sleep_bad_pin', dev.set('/sys/sleep/wakePin', 6) == 422)  # flash pins can't wake it
    dev.set('/sys/sleep/wakePin', pin)
    dev.set('/led/on', True)
    time.sleep(2.5)  # saved
    t = time.time()
    check('sleep_accepted', dev.set('/sys/sleep/sleep', 8) == 200)
    gone = wait_for(lambda: dev.value('/sys/status/uptime') is None, 5)
    check('sleep_unreachable', gone)
    asleep = time.time() - t
    back = wait_for(lambda: dev.value('/sys/status'), 90, step=1)
    took = time.time() - t
    check('sleep_back', back is not None, f'after {took:.0f} s')
    print(f'      asleep after {asleep:.1f} s, back after {took:.0f} s, uptime {back and back["uptime"]} s')
    check('sleep_reset_reason', back and back['resetReason'] == 'deep-sleep', back)
    check('sleep_woken_by_timer', dev.value('/sys/sleep/wakeCause') == 'timer', dev.value('/sys/sleep/wakeCause'))
    check('sleep_long_enough', took >= 8, took)
    check('sleep_parked_the_led', (dev.value('/led') or {}).get('on') is False, dev.value('/led'))

    # Woken by the pin: a devkit's BOOT button (GPIO 0) has a pull-up, so
    # waking on high wakes it at once.
    if pin == 0:
        dev.set('/sys/sleep/wakeLevel', 'high')
        t = time.time()
        check('sleep_until_pin', dev.set('/sys/sleep/sleep', 0) == 200)
        wait_for(lambda: dev.value('/sys/status/uptime') is None, 5)
        back = wait_for(lambda: dev.value('/sys/status'), 90, step=1)
        print(f'      back after {time.time() - t:.0f} s')
        check('sleep_woken_by_pin', back and back['resetReason'] == 'deep-sleep' and
              dev.value('/sys/sleep/wakeCause') == 'pin', (back, dev.value('/sys/sleep/wakeCause')))
        dev.set('/sys/sleep/wakeLevel', 'low')


# ---- Battery ---------------------------------------------------------------

def test_battery(dev):
    bat = dev.value('/sys/battery')
    check('battery_tree', bat and 'soc' in bat and 'voltage' in bat, bat)
    check('battery_soc_range', bat and 0 <= bat['soc'] <= 100, bat)
    check('battery_voltage', bat and isinstance(bat['voltage'], (int, float)) and bat['voltage'] >= 0, bat)
    samples = []
    for _ in range(5):
        time.sleep(1.1)
        v = dev.value('/sys/battery/voltage')
        if v is not None:
            samples.append(v)
    check('battery_sampled', len(samples) == 5, samples)
    print(f'      voltage samples: {samples}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('--broker', default=None)
    ap.add_argument('--only', choices=['log', 'discovery', 'sleep', 'battery'])
    ap.add_argument('--user')
    ap.add_argument('--password')
    args = ap.parse_args()
    dev = Device(args.base)
    if args.user:
        status, body = dev.request('POST', '/sys/auth/login', {'name': args.user, 'password': args.password})
        if status != 200:
            sys.exit(f'sign-in failed: {status} {body}')
        dev.token = body if isinstance(body, str) else body.get('token')
    if not dev.value('/sys/status'):
        sys.exit(f'no device at {args.base}')
    if args.only in (None, 'log'):
        test_log(dev)
    if args.only in (None, 'discovery'):
        if args.broker:
            host, _, port = args.broker.partition(':')
            test_discovery(dev, host, int(port or 1883))
        else:
            print('SKIP discovery: no --broker')
    if args.only == 'battery' or (args.only is None and dev.value('/sys/battery')):
        test_battery(dev)
    if args.only in (None, 'sleep'):
        test_sleep(dev)
    print(f'DONE pass={PASS} fail={FAIL}')
    sys.exit(1 if FAIL else 0)


if __name__ == '__main__':
    main()
