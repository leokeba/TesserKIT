#!/usr/bin/env python3
"""Hardware in the loop: build, flash and test the boards on this machine.

  uv run --with requests --with websocket-client --with pyserial \\
      python test/hil.py test/hil_boards.json [--skip-build] [--skip-flash] [--only udp,device]

Builds examples/idf/full (and its Ethernet variant for boards that have one)
against the sibling commits deps.lock pins, flashes each board after reading
its MAC, waits for every board on the network, then runs the end-to-end
suites. After each suite it reads every board's lowest free heap since boot
(/sys/status/minHeap) and fails when one fell below the floor: an
out-of-memory abort is usually one allocation away from there (DESIGN.md
section 12, docs/review-2026-10-14-open.md, Roadmap › Phase 1).

The boards file (test/hil_boards.example.json) names each board's role, MAC,
serial port, address and build. Waits are bounded; every step prints what it
saw. Exit status: 0 when every suite passed and no board fell below the floor.
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAILURES = []


def log(msg):
    print(msg, flush=True)


def run(cmd, cwd=ROOT, env=None, timeout=None, out=None):
    log('$ ' + ' '.join(cmd))
    with open(out, 'w') if out else open(os.devnull, 'w') as f:
        p = subprocess.run(cmd, cwd=cwd, env=env, timeout=timeout,
                           stdout=f if out else None, stderr=subprocess.STDOUT if out else None)
    return p.returncode


def pinned():
    deps = {}
    with open(os.path.join(ROOT, 'deps.lock')) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                repo, sha = line.split('@')
                deps[repo.split('/')[1]] = (repo, sha)
    return deps


def fetch_siblings(work):
    """The pinned commits, checked out in `work`/<name>, fetched once."""
    paths = {}
    for name, (repo, sha) in pinned().items():
        d = os.path.join(work, 'deps', name)
        if not os.path.isdir(os.path.join(d, '.git')):
            os.makedirs(d, exist_ok=True)
            run(['git', 'init', '-q'], cwd=d)
        have = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=d, capture_output=True, text=True).stdout.strip()
        if have != sha:
            if run(['git', 'fetch', '-q', '--depth', '1', f'https://github.com/{repo}', sha], cwd=d) or \
               run(['git', 'checkout', '-q', 'FETCH_HEAD'], cwd=d):
                sys.exit(f'could not fetch {repo}@{sha}')
        paths[name] = d
        log(f'  {repo} {sha[:7]}')
    return paths


def build(work, siblings, variant, args):
    """examples/idf/full built into work/build-<variant>; returns the app image."""
    out = os.path.join(work, f'build-{variant}')
    env = dict(os.environ, TESSERAPI_PATH=siblings['TesserAPI'], TESSERUI_PATH=siblings['TesserUI'],
               NOWTP_PATH=siblings['NowTP'])
    # ESP-IDF's export.sh picks the first python on PATH: not uv's, which
    # runs this script, but its own environment's.
    venv = env.pop('VIRTUAL_ENV', None)
    env.pop('PYTHONHOME', None)
    env['PATH'] = os.pathsep.join(p for p in env['PATH'].split(os.pathsep)
                                  if not (venv and p.startswith(venv)) and os.sep + 'uv' + os.sep not in p)
    idf = os.path.expanduser('~/esp/esp-idf/export.sh')
    cmd = ['bash', '-c', f'. "{idf}" > /dev/null && idf.py -C examples/idf/full -B "{out}" '
           f'-DSDKCONFIG="{out}/sdkconfig" {" ".join(args)} set-target esp32 build']
    logf = out + '.log'
    os.makedirs(work, exist_ok=True)
    if run(cmd, env=env, out=logf):
        sys.exit(f'build {variant} failed: see {logf}')
    with open(logf) as f:
        size = [l for l in f if 'binary size' in l]
    if size:
        log('  ' + size[-1].strip())
    return os.path.join(out, 'tesserkit_full.bin')


def mac_of(port):
    p = subprocess.run(['uvx', 'esptool', '--chip', 'esp32', '--port', port, '--baud', '115200', 'read-mac'],
                       capture_output=True, text=True, timeout=60)
    for line in p.stdout.splitlines():
        if line.startswith('MAC:'):
            return line.split()[1]
    return None


def flash(board, image):
    got = mac_of(board['port'])
    if got != board['mac']:
        sys.exit(f"{board['name']}: {board['port']} has MAC {got}, expected {board['mac']}: not flashing")
    rc = run(['uvx', 'esptool', '--chip', 'esp32', '--port', board['port'], '--baud', str(board.get('baud', 460800)),
              'write-flash', '0x10000', image], timeout=600)
    if rc:
        sys.exit(f"{board['name']}: flashing failed")
    # write-flash's own reset has left boards outside their app (seen on
    # all three after a downgrade, 2026-10-15): reset into it explicitly.
    run(['uvx', 'esptool', '--chip', 'esp32', '--port', board['port'], '--baud', '115200', 'run'], timeout=60)
    log(f"  {board['name']} flashed")


def status(board, timeout=3):
    try:
        with urllib.request.urlopen(board['url'] + '/api/sys/status', timeout=timeout) as r:
            return json.loads(r.read())
    except Exception:
        return None


def wait_up(boards, settle):
    end = time.time() + 180
    while time.time() < end:
        down = [b['name'] for b in boards if not status(b)]
        if not down:
            break
        time.sleep(5)
    else:
        sys.exit(f'not on the network after 180 s: {", ".join(down)}')
    log(f'  all up; settling {settle} s')
    time.sleep(settle)


def check_heap(boards, floor, after):
    for b in boards:
        s = status(b)
        if not s:
            FAILURES.append(f'{after}: {b["name"]} not answering')
            log(f'  {b["name"]}: not answering')
            continue
        line = f'  {b["name"]}: heap {s["heap"]}, lowest {s["minHeap"]}, uptime {s["uptime"]} s, reset {s["resetReason"]}'
        log(line)
        if s['minHeap'] < floor:
            FAILURES.append(f'{after}: {b["name"]} lowest heap {s["minHeap"]} < {floor}')
        if s['resetReason'] in ('panic', 'watchdog', 'brownout'):
            FAILURES.append(f'{after}: {b["name"]} restarted by {s["resetReason"]}')


def suite(name, cmd, boards, floor, work):
    logf = os.path.join(work, f'suite-{name}.log')
    rc = run(['uv', 'run', '-q', '--with', 'requests', '--with', 'websocket-client', '--with', 'pyserial',
              'python', '-u'] + cmd, out=logf, timeout=1800)
    with open(logf) as f:
        text = f.read()
    passed, failed = text.count('\nPASS'), text.count('\nFAIL')
    log(f'== {name}: exit {rc}, {passed} passed, {failed} failed ({logf})')
    for line in text.splitlines():
        if line.startswith('FAIL'):
            log('  ' + line[:200])
    if rc:
        FAILURES.append(f'{name}: exit {rc}')
    check_heap(boards, floor, name)


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    with open(args[0]) as f:
        cfg = json.load(f)
    only = set(args[args.index('--only') + 1].split(',')) if '--only' in args else None
    work = os.path.expanduser(cfg.get('work', '/tmp/tesserkit-hil'))
    floor = cfg.get('heapFloor', 8192)
    boards = cfg['boards']
    by_role = {b['role']: b for b in boards}

    log('== siblings (deps.lock)')
    siblings = fetch_siblings(work)
    images = {}
    if '--skip-build' not in args:
        for b in boards:
            variant = b.get('build', 'full')
            if variant not in images:
                images[variant] = build(work, siblings, variant, cfg.get('builds', {}).get(variant, []))
    if '--skip-flash' not in args:
        log('== flash')
        for b in boards:
            image = images.get(b.get('build', 'full')) or os.path.join(work, f"build-{b.get('build', 'full')}",
                                                                       'tesserkit_full.bin')
            flash(b, image)
    log('== boards')
    wait_up(boards, cfg.get('settle', 60))
    check_heap(boards, floor, 'boot')

    def want(n):
        return only is None or n in only

    gw, peer, eth = by_role.get('gateway'), by_role.get('peer'), by_role.get('ethernet')
    if gw and want('device'):
        suite('device', ['test/e2e_device.py', gw['url'], '--only',
                         'test_status,test_ui,test_websocket,test_time,test_persistence,test_backup,test_auth,test_timers'],
              boards, floor, work)
    if eth and want('ethernet'):
        cmd = ['test/e2e_ethernet.py', eth['url']]
        if eth.get('staticIp'):
            cmd += ['--static', eth['staticIp']]
        suite('ethernet', cmd, boards, floor, work)
    if gw and peer and want('transfers'):
        cmd = ['test/e2e_transfers.py', '--http', gw['url'], '--peer', peer['hostname'],
               '--image', images.get(gw.get('build', 'full'), os.path.join(work, 'build-full', 'tesserkit_full.bin'))]
        if gw.get('port'):
            cmd += ['--serial', gw['port']]
        suite('transfers', cmd, boards, floor, work)
    if len(boards) >= 2 and want('udp'):
        suite('udp', ['test/e2e_udp.py'] + [b['url'] for b in boards], boards, floor, work)

    log('== result')
    for f in FAILURES:
        log('  FAIL ' + f)
    log('  passed' if not FAILURES else f'  {len(FAILURES)} failures')
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
