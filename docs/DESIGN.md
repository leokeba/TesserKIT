# TesserKIT design

Status: draft, written before any code. Everything here is the specification the code will follow; when the two disagree, fix one of them. Sections marked *(later)* are committed to but not part of the first release. TesserAPI and TesserUI references point to their own DESIGN.md: [TesserAPI](https://github.com/leokeba/TesserAPI/blob/main/docs/DESIGN.md), [TesserUI](https://github.com/leokeba/TesserUI/blob/main/docs/DESIGN.md).

## 1. Goals

TesserKIT is the device framework built on [TesserAPI](https://github.com/leokeba/TesserAPI) (the tree and its transports), [TesserUI](https://github.com/leokeba/TesserUI) (the interface rendered from the tree's schema) and, optionally, [NowTP](https://github.com/leokeba/NowTP) (ESP-NOW). It provides what every connected device needs besides its own function: network setup, time, firmware updates, backups, users, MQTT, system status. It aims at most of [ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit)'s functionality at a fraction of its footprint.

- **A library, not a template.** Applications depend on TesserKIT; they don't fork it. Upstream fixes arrive by updating a version, not by merging.
- **Every service is a subtree.** A service describes itself into the TesserAPI tree under `/sys`, with values, actions, events, lists and files. It is then reachable over every TesserAPI transport (HTTP, WebSocket, serial, NowTP), through gateways, and from MQTT, with no endpoint code of its own.
- **Zero UI code.** Services carry TesserUI hints (labels, units, widgets, confirmations, layout); the generic renderer draws their pages. No service has a hand-written page, and neither does an application unless it wants one (custom elements, TesserUI §9.8).
- **Pay for what you use.** A service that isn't instantiated isn't linked. There are no feature flags.
- **Small.** The whole interface is TesserUI's bundle (≤ 50 KB gzipped, against ESP32-sveltekit's ~240 KB). Settings live in NVS: no filesystem partition. Measured budgets in §11.
- **Host-tested.** Service logic is platform-independent and runs against a fake platform on the host (§12).

### Non-goals

- Being an HTTP framework. The HTTP server is TesserAPI's (`esp_http_server`); applications that need other routes register them on the same handle.
- TLS for the device's own server. Like ESP32-sveltekit, devices serve plain HTTP on the local network. TLS client connections (MQTT, update downloads) are supported.
- Arduino-ESP32 2.x, ESP8266, non-ESP chips.
- Replacing applications' own control logic: TesserKIT gives the scaffolding around it.

## 2. Constraints

| Item | Decision |
|---|---|
| Language | C++17, TesserAPI's rules: no exceptions, no RTTI, never `throw`/`dynamic_cast`/`typeid`; calling an empty `std::function` aborts. |
| Frameworks | ESP-IDF ≥ 5.1 and Arduino-ESP32 ≥ 3.0, like TesserAPI. |
| Chips | Classic ESP32 without PSRAM, 4 MB flash (reference); ESP32-C3; ESP32-S3. |
| Partitions | Every service fits a 1.25 MB app slot, so Arduino's default 4 MB table (two OTA slots, no `min_spiffs`) works. TesserKIT ships a table without a filesystem for more headroom (§11). |
| Dependencies | TesserAPI, TesserUI (C++ part: `WebUi`), ArduinoJson 7. Optional: NowTP. ESP-IDF components only (`esp_wifi`, `esp_netif`, `mdns`, `esp_https_ota`, `mqtt`, `esp_http_client`, `nvs_flash`, `app_update`): the same code on Arduino and ESP-IDF. |
| Platform code | Only in `src/tesserkit/platform/`. Services see a narrow interface (§3.3) with an ESP implementation and a host fake. |
| Threading | One task of TesserKIT's own (§4.2), plus the transports' tasks. Everything a service does to the tree runs under the API lock. |

## 3. Architecture

```
 application tree ─┐
                   ├─▶ tesser::Api ──▶ HttpServer (+ WebSocket) ──▶ browser: TesserUI bundle (self-hosted)
 TesserKIT /sys ───┘        │     ──▶ LineTransport (serial)     ──▶ Web Serial, scripts
   Network, Time,           │     ──▶ NowTpTransport             ──▶ peers, gateways, UI hosts
   Firmware, ...            │     ──▶ Mqtt bridge (§5.8)         ──▶ brokers, Home Assistant
                            └─▶ NVS (one persisted document)
```

### 3.1 Device

`tesserkit::Device` owns what every node has: the `tesser::Api`, the HTTP server with WebSocket, the TesserUI page and bundle, persistence in NVS, and TesserKIT's task. Services are separate objects, given the device:

```cpp
#include <TesserKIT.h>

tesserkit::Device dev("Heliostat");     // the device's label; hostname "heliostat-a67c"
tesserkit::System sys(dev);              // /sys/status, restart, factory reset, core dump
tesserkit::Network net(dev);             // /sys/network: Wi-Fi, access point, captive portal, mDNS
tesserkit::Time clock(dev);              // /sys/time: SNTP, time zone, set from the browser
tesserkit::Firmware fw(dev);             // /sys/firmware: upload, update from a URL, rollback
tesserkit::Backup backup(dev);           // /sys/backup: download and restore the settings

void setup() {
    describe(dev.api().object("heliostat").label("Heliostat"), heliostat);   // the application's tree
    dev.begin();                         // loads settings, starts Wi-Fi, HTTP, the UI and the task
}
void loop() {}                           // nothing to do: TesserKIT runs in its own task
```

- **Construction** declares nothing yet: services register with the device. `begin()` creates `/sys` and lets each service describe itself in registration order, then loads the persisted state, then starts the platform side of each service, then the transports.
- **The application's tree** is declared before `begin()`, so `load()` restores its persisted values with the services'. Nodes added after `begin()` need the API lock (TesserAPI §4.6).
- **Options** (before `begin()`): `dev.hostname("helio")`, `dev.version("1.4.0")`, `dev.ui(false)` (headless: no page or bundle), `dev.httpPort(80)`, `dev.cors("*")`, `dev.task(false)` (the application calls `dev.loop()` itself), `dev.storage(storage)` (instead of NVS).
- **Accessors:** `dev.api()`, `dev.http()`, `dev.web()` (TesserUI's `WebUi`, for its options), `dev.sys()` (the `/sys` object, for an application's own system-level nodes).

### 3.2 The `/sys` tree

```
/sys                    label "System", ui.page, ui.icon "settings", a description
  status/               System (§5.1): read-only, watched values
  restart, factoryReset System's actions
  network/              Network (§5.2)
  time/                 Time (§5.3)
  firmware/             Firmware (§5.4)
  backup/               Backup (§5.5)
  timers                Timers (§5.6), a keyed list
  auth/                 Auth (§5.7)
  mqtt/                 Mqtt (§5.8)
```

- The root stays the application's: `/sys` is one child of it, rendered by TesserUI as a link to its own page, where each service is a card.
- Service subtrees are stable API: other devices, scripts and MQTT topics address them by path. Renaming a node is a breaking change, listed in the changelog.
- Every service describes itself through a public `describe(tesser::Object&)`, so an application that wants a different layout (no `/sys`, or a service under its own object) mounts it itself: `api.mount("wifi", net)` and `dev.begin(tesserkit::NoSys)`.

### 3.3 Platform layer

Services never call ESP-IDF directly. They call `tesserkit::Platform`, a set of narrow interfaces: `Core` (chip information, heap, restart, tasks), `Wifi` (station, access point, captive portal), `Mdns`, `Clock` (the clock, the time zone, SNTP), `Ota` (the OTA slots), `HttpClient` (downloads) and `Coredump`:

- `src/tesserkit/platform/esp/` implements them on ESP-IDF APIs, which Arduino-ESP32 3.x also ships, so one implementation serves both.
- `test/fake_platform.*` implements them in memory for host tests: a Wi-Fi radio whose scans and connections the test scripts, a clock the test advances, an OTA partition that's a byte vector.
- **Linked when used:** `Device::platform()` holds a pointer per interface. A service fills the ones it uses with the ESP implementation when the application (or a test) didn't, so an interface whose service isn't instantiated isn't linked. Events from platform tasks (Wi-Fi, SNTP) are posted to the device and handed to the services in its task, under the API lock.

Service code is then plain state machines driven by `tick(nowMs)` and platform callbacks, testable on the host like TesserAPI's core.

### 3.4 What each layer owns

| Concern | Owner |
|---|---|
| Tree, schema, validation, subscriptions, persistence, access levels, files | TesserAPI |
| Web rendering, widgets, layout, the bundle | TesserUI |
| ESP-NOW transport, discovery | NowTP (through TesserAPI's `NowTpTransport`) |
| Device bootstrap, the services of §5, the platform layer, defaults, the partition table, examples | TesserKIT |

New mechanisms go to the lowest layer that can host them, generically. A service needing something TesserAPI lacks adds it to TesserAPI (§14), not a workaround here.

## 4. The device

### 4.1 Boot sequence

1. `begin()` creates `/sys`, has every service describe itself, and sets the device label and the schema metadata.
2. Opens NVS (namespace `tesserkit`, one record per top-level node, §6.1) and loads the persisted state (TesserAPI §12). An NVS partition NVS can't use (`no free pages` or `new version found`, as a firmware with another partition table leaves it) is erased and initialized again: nothing in it could be read, and Wi-Fi can't start without NVS. Values that don't fit any more are skipped with a warning: a firmware update never breaks a boot.
3. Applies factory defaults (§6.2) to whatever the stored state didn't set.
4. Starts each service's platform side: Network brings up Wi-Fi, Time starts SNTP, Firmware checks the rollback state.
5. Starts the HTTP server: the API at `/api`, WebSocket at `/ws`, the UI at `/` and `/ui.js` (TesserUI §9.4), the captive portal's error handler (§5.2). The server's URI handler budget is computed from what's used, never set by hand: the API's, the UI's, plus `dev.extraHandlers(n)` (default 4) for the application's own routes and TesserUI assets.
6. Starts TesserKIT's task.

### 4.2 Task

One FreeRTOS task (`tesserkit`, 6 KB stack, priority 3) runs every 20 ms:

- `api.poll()`: change notifications, watched values, debounced persistence
- each service's `tick(nowMs)`: Wi-Fi state machine, timers, MQTT publishing

Platform callbacks (Wi-Fi events, SNTP sync) only post to a small queue that the task drains, so services never run in the Wi-Fi task and always hold the API lock while they touch the tree. With `dev.task(false)` the application calls `dev.loop()` from its own loop instead.

### 4.3 State

`dev.state()` and `dev.onState(fn)` give the device's overall state, for a status LED or a display: `booting`, `provisioning` (access point up, no network configured), `connecting`, `online`, `updating`, `error`. It is also a read-only value, `/sys/status/state`.

### 4.4 Identity

- **Label:** `Device("Heliostat")` sets `api.label()`, shown by TesserUI as the device's name.
- **Hostname:** the label lowercased, non-alphanumerics replaced by `-`, plus the last two MAC bytes (`heliostat-a67c`), unless `dev.hostname()` or the persisted `/sys/network/hostname` says otherwise. It is the DHCP hostname, the mDNS name and the NowTP discovery name.
- **Version:** `dev.version("1.4.0")`, else the ESP-IDF app description's version. Shown in `/sys/firmware/version`, advertised over mDNS.

## 5. Services

Each service lists its tree, its behaviour and its access levels (§7). "Admin" means the node's `writeAccess(Admin)`; reads are public unless stated. Persisted nodes are marked *(p)*. Settings are checked with TesserAPI validators (`.validate()`), before anything is applied, so a refused patch, form or list write changes nothing, and a boot load or a restore skips what no longer passes.

### 5.1 System

```
/sys/status
  state        enum: booting, provisioning, connecting, online, updating, error
  uptime       integer, s, watched
  heap         integer, B, watched, ui.chart        minHeap  integer, B
  psram        integer, B (only with PSRAM)
  resetReason  enum: power-on, software, panic, watchdog, brownout, deep-sleep, other
  chip         string: "ESP32-D0WD-V3 rev 3.0, 2 cores, 240 MHz"
  flash        integer, B        appSize, appSlot  integer, B
  idf          string            mac  string
  temperature  number, °C (chips with a sensor), watched
/sys/restart        action, Admin, ui.confirm, ui.style danger
/sys/factoryReset   action, Admin, ui.confirm, ui.style danger: erases the persisted state, then restarts
/sys/coredump       file, Admin, download only (<hostname>-coredump.bin, the partition's raw data for espcoredump.py); present only when the core dump partition exists
/sys/coredumpPresent boolean          /sys/clearCoredump action, Admin
```

- Values that only change at boot (chip, flash, idf) are constants: zero cost per read.
- Watched values (heap, uptime, temperature) are sampled only while someone is subscribed (TesserAPI §11), so a UI chart (TesserUI §7.7) replaces ESP32-sveltekit's analytics service and its device-side history.
- `restart` answers first, then restarts after 500 ms, so the client sees the reply.
- *(later)* `/sys/log`: an event carrying ESP log lines (`esp_log_set_vprintf`), at a configurable level, for a log view in the UI.

### 5.2 Network

```
/sys/network
  hostname *(p)        string ≤ 32, Admin
  wifi/
    networks *(p)      keyed list (key "ssid"), ≤ 5, Admin, readAccess User:
                         ssid ≤ 32, password (secret) ≤ 64,
                         static/ { enabled, ip, mask, gateway, dns } ui.collapsed
    scan               deferred action, Admin → [{ssid, rssi, channel, secure}]
    status/            connected, ssid, ip, rssi (watched, dBm), channel, bssid
  ap/
    mode *(p)          enum: always, fallback, never (default fallback), Admin
    ssid *(p)          string ≤ 32 (default: the hostname), Admin
    password *(p)      secret, empty or ≥ 8 characters, Admin
    channel *(p)       integer 1–13, Admin
    hidden *(p)        boolean, Admin
    status/            active, ip, clients
  mdns *(p)            boolean (default true), Admin
```

**Station:**
- At start and after a disconnect, it scans and joins the strongest saved network in range, then the next ones; when none is in range, it tries every saved network in order (hidden networks don't show in scans). An attempt that gets no answer is abandoned after 10 s. A round that fails is retried with backoff (1, 2, 5, 10, 30 s); a scan finds networks that came back.
- A write to `networks` reconnects if the current network changed or was removed.
- `static.enabled` replaces DHCP for that network.
- `scan` is deferred (TesserAPI §4.2): the reply comes when the scan completes, within TesserAPI's `deferTimeoutMs`. The radio can't scan while it joins, so a scan requested during an attempt waits for it to end (at most 10 s), and one requested during the join's own scan shares it.
- `net.txPower(dBm)` caps the transmit power, for boards whose supply browns out at the default 20 dBm.

**Access point:**
- `fallback`: up when no saved network is joined within 15 s of trying, and down 30 s after the station connects (time for a phone on the AP to follow the device's new address). `always`: always up (AP+STA). `never`: never, except in recovery (below).
- **Recovery:** with no saved network, the AP comes up whatever the mode, so a fresh device is always reachable.
- **Captive portal:** while the AP is up and the station isn't connected, a DNS server answers every name with the AP's address, and the HTTP server's 404 handler redirects to `/` (`302`). OS connectivity probes (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`) thus all land on the UI, so phones open it on joining. This costs no URI handler slot.

**mDNS:** `<hostname>.local`, with `_http._tcp` and TXT records `api=/api`, `ws=/ws`, `version=<version>`, `tesser=1`, on ESP-IDF's `mdns` component (about 34 KB of flash). On by default, since it is how a provisioned device is found; `-DTESSERKIT_NO_MDNS` leaves it out, with the `mdns` setting.

**Channels and NowTP:** a station follows its router's channel, and ESP-NOW must share it (TesserAPI §14). Network reports channel changes to `dev.onChannel(fn)`; with NowTP present, the device moves NowTP's channel there. This needs NowTP's channel following (§14). Without a router, the AP's `channel` is the field's ESP-NOW channel.

**Not in the first release:** Ethernet (W5500 and RMII, *(later)*, same tree under `/sys/network/ethernet`), WPA2-Enterprise, IPv6 configuration.

### 5.3 Time

```
/sys/time
  now           string, local ISO 8601, watched every second while subscribed
  epoch         integer, s since 1970 (UTC)
  synced        boolean     source  enum: none, sntp, set, external
  tz *(p)       string, POSIX TZ ("CET-1CEST,M3.5.0,M10.5.0/3"), Admin, ui.widget "timezone"
  sntp/
    enabled *(p) boolean (default true), Admin
    server *(p)  string (default "pool.ntp.org"), Admin
  set           action(integer ms since 1970), Admin, ui.value "$now", label "Set from this device"
```

- `tz` is applied with `setenv("TZ")` + `tzset()`, so `localtime()` follows it everywhere in the firmware.
- `set` takes the browser's clock (TesserUI's `$now` hint value): a device without internet gets the time from whoever opens its page. helionext's "Sync Time" button.
- `clock.external(epochMs)` lets the application feed another source (GPS, RTC); `source` says which one set the clock last. SNTP, when enabled and reachable, wins over `set` but not over `external`.
- `external()` takes the API lock (recursive): call it from `dev.onTick()`, a handler or any task that may wait as long as the longest API operation, never from a time-critical one such as a control loop. Such a task hands the time over (a copy under its own lock) and the application calls `external()` from `onTick`.
- `dev.onTimeSynced(fn)` fires on the first valid time after boot (timers and sun trackers wait for it).
- SNTP's first request usually goes out before Wi-Fi is up; when the device comes online unsynced, Time asks again at once rather than at SNTP's next retry.
- The `timezone` widget (TesserUI follow-up, §14) offers IANA names and writes the POSIX string the browser derives from them; until it exists, `tz` is a text field with an example placeholder.

### 5.4 Firmware

```
/sys/firmware
  version       string            project  string        built  string (date)
  slot          string ("ota_0")  slotSize integer, B
  image         file, Admin, upload only, maxSize = slotSize, type application/octet-stream
  download      action(string url), Admin: fetch an image over HTTP(S) and install it (with fw.downloads())
  state         enum: idle, receiving, downloading, verifying, rebooting, failed; watched
  progress      integer, %, watched, ui.widget "bar"
  error         string
  pending       boolean: running a new image that isn't confirmed yet
  confirm       action, Admin: mark the running image valid now
  rollback      action, Admin, ui.confirm: boot the previous image
  github/       (with fw.github("owner/repo", "asset-{target}.bin"))
    latest      string          available  boolean
    check       deferred action → latest version
    update      action: download the latest release's asset
```

- **Upload:** a `PUT /api/sys/firmware/image` with the raw `.bin` as body, streamed to the next OTA slot through a TesserAPI file node (§14). TesserUI's file widget shows progress. The image header is checked on the first chunk (wrong chip or not an app image is `invalid_value` before anything is written). A completed upload sets the boot slot and restarts after the reply. The announced size is erased in 64 KB blocks just ahead of the bytes written: a 1 MB image uploads in 12 to 15 s on a classic ESP32 (25 s erasing sector by sector as it arrives). Erasing the whole image at once is as fast but keeps the idle task from running for over 5 s, which trips the task watchdog (a panic on Arduino).
- **Download** (`fw.downloads()`, before `begin()`): `esp_http_client` in a task of its own (8 KB of stack), following redirects (release assets redirect to a CDN), streaming through the same checks as an upload, with progress in `progress`. HTTPS uses ESP-IDF's certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`). TLS and the client cost about 130 KB of flash, hence the opt-in: without `downloads()`, neither the action nor the client is linked. The action replies at once; completion shows in `state`.
- **Rollback:** with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a new image starts as `pending`. It is confirmed automatically once the device has been `online` (or serving, without Wi-Fi) for 60 s, or by `confirm`. A crash or reset before that boots the previous image. `rollback` makes the previous image boot next; it then starts `pending` too. ESP-IDF refuses an update while the running image is unconfirmed, so starting one confirms it: it is evidently working. Arduino-ESP32 confirms every new image at boot unless the application defines `verifyRollbackLater()`; TesserKIT defines it (returning true) whenever Firmware is linked, so the same rule applies there.
- **GitHub releases** *(later)*: the device asks GitHub's API for the latest release (so the browser needs no internet, unlike ESP32-sveltekit), compares its tag with `version` (semver), and installs the asset whose name matches the pattern with `{target}` replaced (`esp32`, `esp32c3`...).
- **Fleets:** a gateway reaches its Wi-Fi peers' `/sys/firmware/download` like any other path (`/peers/<name>/sys/firmware/download`), so "update all" is one write per peer, without the browser fanning out (spin-seq's Remotes page). NowTP-only peers need NowTP streaming *(later, §14)*.

### 5.5 Backup

```
/sys/backup
  settings      file, Admin: download = the persisted state as JSON; upload = restore it
  restartAfter *(p) boolean (default true)
```

- **Download:** `api.persistedState()`, secrets included (it is Admin-only), as `<hostname>-settings.json`.
- **Restore:** applied with `api.restore(json, &skipped)` (TesserAPI §12): lenient like a boot load, so a backup from an older firmware restores whatever still fits; the reply (`{"skipped": [...], "restart": bool}`) lists the skipped paths. Every record is saved at once, then the device restarts one second later if `restartAfter` was set when the restore began (the backup's own `restartAfter` applies to the next one).
- This replaces spin-seq's hand-written export and import, for every application.

### 5.6 Timers

```
/sys/timers *(p)    keyed list (key "name"), ≤ 16, Admin:
  name          string ≤ 24
  enabled       boolean
  time          string "HH:MM", local time
  days          integer bitmask, bit 0 = Monday, ui.widget "flags", ui.labels "Mon,Tue,Wed,Thu,Fri,Sat,Sun"
  path          string: the node to set or call
  value         string: its JSON value or argument ("null" for an action without one)
  last          string, read-only: the last run's result
/sys/timersCatchUp *(p)  boolean: at boot, run each timer's most recent missed occurrence of today
```

- At the given local time on the given days, a timer performs `set <path> <value>` through the API, as an Admin local client, exactly as if a client had sent it. Its result is logged and kept in a read-only `last` (`"ok"` or the error).
- A write to a timer checks that `path` resolves and that `value` parses; whether the value fits the node is checked when it runs (the node may accept different values by then). Below a list, an array or a remote node, the path can't be checked further and is accepted. The check applies once the service has started: a boot load keeps stored timers whose node the application adds after `begin()`, and a run then reports `not_found`.
- Timers wait for a valid time (§5.3). They are checked every second, and a time change (DST, a sync) never runs a timer twice in one minute: a run is remembered by its local date and minute.
- Factory defaults: `timers.add(timer)`, `timers.catchUp(true)`.
- This generalizes spin-seq's daily timers: any action or value of the tree can be scheduled, not just sequencer commands.

### 5.7 Auth

```
/sys/auth
  users *(p)    keyed list (key "name"), ≤ 8, readAccess Admin:
                  name ≤ 24, password (secret; stored as a salted PBKDF2-SHA256 hash), role enum: user, admin
  login         action({name, password}) → {token, access}; public, ui.auth "login"
  whoami        action → {access}; public, ui.auth "whoami"
  open          boolean: no admin account yet
  logoutAll     action, Admin: rotates the signing key, invalidating every token
  sessionHours *(p)  integer 1–720 (default 168), Admin
```

- **Levels:** TesserAPI's access levels (§7): `public`, `user`, `admin`. A node requires its ancestors' levels too, so `/sys/auth` itself stays public and only `users` is protected. Without the Auth service, every client is Admin (the device is open, as ESP32-sveltekit with `FT_SECURITY=0`).
- **The UI** finds `login` and `whoami` by their `ui.auth` hints (TesserUI §7.4): its sign-in dialog shows `login`'s fields, and it asks `whoami` for its level to disable what it can't change.
- **Passwords** are stored as `$pbkdf2-sha256$<iterations>$<salt>$<hash>` (2000 iterations, 16-byte random salt): a sign-in takes about 280 ms on a classic ESP32, network included. Writing a password stores its hash; writing a stored hash (a restored backup) keeps it. SHA-256, HMAC and PBKDF2 are TesserKIT's own (`crypto.h`, checked against the RFC vectors), so the host tests run the chip's code.
- **Tokens** are stateless: `<name>.<expiry>.<HMAC-SHA256 of both, truncated to 16 bytes, hex>`, signed with a random key kept in the persisted state (`/sys/auth/key`, a hidden secret). They survive restarts; `logoutAll` revokes them all, and removing an account revokes its. The expiry is the clock's time plus `sessionHours`; a token issued while the clock isn't set has none (0) and lasts until `logoutAll`. The device registers a token check with TesserAPI (`api.authenticate()`), so HTTP and WebSocket clients present it as `Authorization: Bearer` or `?token=` (TesserAPI §13).
- **First boot:** until an admin user exists, every client is Admin and `/sys/auth/open` is `true` (hidden, for conditions); `openWarning`, a read-only text with `ui.style "warning"` and `visibleWhen "open=true"`, is TesserUI's warning callout. A factory default account (`auth.defaultAdmin("admin", "secret")`, before `begin()`) avoids the open window for devices shipped to others.
- **Brute force:** after 5 failed logins, `login` answers `busy` for 30 s. An unknown name costs a hash like a wrong password, so timing doesn't tell which names exist.
- Passwords are never readable: secret values render as `null` (TesserAPI §4.1). ESP32-sveltekit returned them in plain text.
- Serial clients stay Admin (physical access). NowTP peers are Admin when trusted (TesserAPI §13): a gateway's own access checks apply to its browser clients before it forwards.

### 5.8 Mqtt

```
/sys/mqtt
  enabled *(p)      boolean, Admin
  uri *(p)          string ("mqtts://broker:8883"), Admin
  user *(p), password *(p) (secret), clientId *(p) (default: hostname), Admin
  prefix *(p)       string (default "tesser/<hostname>"), Admin
  publish *(p)      list ≤ 8 of {path, interval ms (default 1000)}, Admin
  homeAssistant *(p) boolean: publish discovery configs, Admin
  status/           connected, error
```

- **Bridge:** for each `publish` entry, the service holds a local subscription (TesserAPI `LocalClient`) to that path, with its interval, and publishes each changed leaf to `<prefix>/<path>` as JSON, retained. Events go to `<prefix>/<path>` unretained.
- **Commands:** it subscribes to `<prefix>/+/set` (and deeper), and turns a message on `<prefix>/<path>/set` into `set <path> <payload>`, with User access. The reply goes to `<prefix>/<path>/result`.
- **Availability:** `<prefix>/status`, `online` and a last will of `offline`, retained.
- **Home Assistant discovery** *(later)*: for each published leaf, a config under `homeassistant/<component>/<hostname>/<path>/config`, the component chosen from the schema: writable boolean → `switch`, read-only boolean → `binary_sensor`, number with a range → `number`, read-only number → `sensor` (with `unit`), enum → `select`, action without argument → `button`.
- One generic bridge replaces ESP32-sveltekit's per-service `MqttEndpoint`s. Neither helionext nor spin-seq uses MQTT today, hence its place in the roadmap.

### 5.9 Later

| Service | Content |
|---|---|
| Battery | `soc` (%), `charging`, `voltage`; fed by the application (`battery.update(...)`) or an ADC pin with a divider. |
| Sleep | `sleep` action (deep sleep), wake-up pin and level *(p)*, `dev.onSleep(fn)` for the application to park its hardware. |
| Ethernet | §5.2. |
| Log | §5.1. |
| ArtNet | `/sys/artnet` *(proposed)*: `enabled`, `universe`, and `mappings`, a list ≤ 8 of `{path, coarse, fine, min, max}` (channels 1–512, 0 unused, `fine` 0 for 8 bits), each written to its path through the API as `min + v/65535·(max−min)`, on change only, as Timers writes to paths; a mapping with a threshold writes a boolean (a gate). Answers ArtPoll (short name the label, long name the hostname) on UDP 6454. `status/`: packets, age, last universe. For tiny-heliostat (§10.1) and lighting-controlled devices. |

## 6. Persistence

### 6.1 Storage

- **Records:** TesserAPI's persistence (§12 there) saves the persisted nodes of the tree as one NVS blob per top-level node, debounced 2 s after the last change: `/sys` (every service's settings) is one record, each of the application's top-level objects another. A change rewrites only its own record, and NVS spreads writes across its pages, so frequent changes don't wear one sector.
- **No filesystem:** nothing needs LittleFS or SPIFFS. Applications that want files mount one themselves.
- **Factory reset:** `Storage::erase()` removes every record, then the device restarts. The application's persisted values return to their defaults too.

### 6.2 Factory defaults

Defaults are the values the C++ variables are initialized with. Settings that vary per product are set before `begin()`:

```cpp
net.defaultNetwork("Workshop", WORKSHOP_PASSWORD);   // from a gitignored header, never committed
net.apMode(tesserkit::ApMode::Always);
clock.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
```

They apply only when the stored state doesn't set the value, so a user's change survives updates, and a factory reset returns to them. ESP32-sveltekit's `factory_settings.ini` placeholders (`#{unique_id}`) become ordinary code (`dev.hostname()` already has the MAC).

## 7. Security

- **Access levels** come from TesserAPI (§13 there): every client has one (`public`, `user`, `admin`), every node may require one to read and to write, inherited by its subtree. Nodes a client can't read are left out of its reads, schemas and notifications; writing one it can't write is `unauthorized`.
- **Defaults of the services:** status is public; settings read as public (secrets never readable) and write as Admin; network lists and auth read as User and Admin respectively.
- **Applications** mark their own nodes: `heliostat.writeAccess(tesser::Access::User)`.
- **Who has which level:** §5.7. Without the Auth service, everyone is Admin.
- **Plain HTTP:** tokens cross the local network in clear. This is ESP32-sveltekit's model too; a device that needs more uses a TLS-terminating gateway, or Web Serial.
- **NowTP** frames aren't authenticated: trusting peers by MAC is spoofable. NowTP's authenticated pairing (§14) is the fix.

## 8. User interface

The UI is TesserUI's generic renderer, self-hosted by the device (TesserUI §9.4): `/` and `/ui.js`, about 41 KB of flash. Nothing is TesserKIT-specific in the bundle:

- **Services' pages** are their trees with hints: labels, units, `confirm` on destructive actions, `style danger`, `advanced` on rarely used settings, `form` where several fields must change together (the access point), `visibleWhen` for conditional fields (static IP), `chart` on heap and RSSI.
- **The application's pages** are its tree, plus TesserUI views (§8 there) for dashboards and custom elements (§9.8 there) for what generic widgets can't draw.
- **Login:** when a request answers `unauthorized`, TesserUI's sign-in dialog calls `/sys/auth/login` with a name and password (TesserUI §7.4). Without the Auth service it never needs to: everyone is Admin.
- **Theming and text:** `dev.web().theme(...)`, `.text(...)`, `.title(...)` (TesserUI §9.4).

TesserUI renders everything TesserKIT's services declare: files (§5.4, §5.5), secrets, access levels and sign-in, flags (§5.6), the time zone widget and `$now` (§5.3).

## 9. Several nodes

TesserKIT devices are TesserAPI nodes, so the multi-node features are TesserAPI's and TesserUI's:

- **UI host:** a TesserKIT device with NowTP mounts its peers (`dev.peers()`, which calls TesserAPI's `mountPeers` on `/peers` and advertises the device). The UI shows a page per peer, forwarded over ESP-NOW (TesserUI §9.5).
- **NowTP-only nodes** run TesserAPI and NowTP without TesserKIT's Wi-Fi services, or with a subset: `System` and `Firmware` work over any transport, except uploads (HTTP only).
- **Fleet operations** are writes through the gateway to each peer: update every peer's firmware (§5.4), set a value on all of them, trigger a sequence. A view (TesserUI §8) with `each("/peers", ...)` shows a fleet dashboard.
- **Synchronized actions** (spin-seq's "start together") need a shared clock: NowTP's clock offset (§14) lets a gateway say "start at T" to every peer, instead of re-broadcasting with retries.

## 10. Porting from ESP32-sveltekit

| ESP32-sveltekit | TesserKIT |
|---|---|
| `StatefulService<T>` + `read`/`update` static functions | `describe(tesser::Object&, T&)`: one line per field (TesserAPI §4) |
| `HttpEndpoint` (GET/POST on `/rest/x`) | none: every node is at `/api/<path>`, with get, set, patches and shapes |
| `FSPersistence` (`/config/x.json`) | `.persist()` on the node or its object |
| `EventSocket` / `EventEndpoint` (`/ws/events`) | WebSocket subscriptions and events (TesserAPI §11), with `interval` coalescing |
| `MqttEndpoint` | the Mqtt bridge's `publish` list (§5.8) |
| `features.ini` (`FT_*`) | instantiate the service, or don't |
| `factory_settings.ini` | initializers and `default...()` calls (§6.2) |
| `SecurityManager` predicates `NONE_REQUIRED`, `IS_AUTHENTICATED`, `IS_ADMIN` | `readAccess`/`writeAccess` `Public`, `User`, `Admin` |
| `NotificationService::pushNotification` | an event with `ui.toast` (TesserUI §5.2) |
| `AnalyticsService` | watched values with `ui.widget "chart"` |
| Svelte pages per service, menu entries | none for services; the application's tree, views and custom elements |
| `WWWData.h` (one handler per asset, `immutable` caching) | TesserUI's bundle: two handlers, ETag revalidation, so a firmware update never serves a stale UI |
| `addLoopFunction()` | the application's own `loop()` or task; `dev.onTick(fn)` runs in TesserKIT's task |

### 10.1 helionext (open-heliostat)

The heliostat is ported in its own repository, [tiny-heliostat](https://github.com/leokeba/tiny-heliostat), whose `docs/DESIGN.md` is the spec of the port. In short:

- Classic ESP32 DevKitC, ESP-IDF only. Two DC servo axes with magnetic encoders, GPS.
- Motors, encoders, axes, sequencers and the GPS come from [TesserMotion](https://github.com/leokeba/TesserMotion), a sibling library on TesserAPI, so devices without motors don't carry them. `NmeaGps` feeds `clock.external()` (§5.3) through the application's `onTick`.
- `/heliostat` holds the sun position, the modes, and the named directions: one keyed list `directions` (helionext's `targetsMap`), with `source` and `target` naming entries and a `mode` enum.
- Art-Net is received into `/heliostat/artnet` (`azimuth`, `elevation`); the heliostat decides what the values mean. A generic receiver mapping channels to tree paths is proposed as `/sys/artnet` (§5.9).
- The calibration routines, the mount orientation, autotune, several heliostats and settings migration are dropped for now.
- TesserKIT provides `/sys`: Device, System, Network, Time, Firmware (upload), Backup, Timers, Auth; the interface uses `dev.web().asset()` for the custom elements `x-sky` and `x-keyframes`, and TesserUI's `Views`.

### 10.2 spin-seq (spin-sequencer)

spin-seq (ESP32-S3 XIAO, TMC5160 stepper, magnetic encoder, master and remotes) ports with a different multi-node architecture:

| Today | TesserKIT |
|---|---|
| `SpinSequencerService` `/rest/spin-seq` + UDP JSON router (port 9600) | `/controller`, `/sequencer`, `/io`, `/welcome`; the UDP router is replaced by TesserAPI over NowTP (and a UDP datagram transport if older installations must keep working, §14) |
| Sequencer commands: an array of free-form JSON | a list of typed commands `{c: enum, d, n, p: {pos, dist, speed, acc, en}, b, ...}`, so the table is generic and validated; `execute` and `select` are actions with typed arguments |
| `bootCommandIndex`, `isRunning` | persisted values |
| Remotes list from mDNS, `ESPNowService`, `UdpSenderService` | the master is a UI host: NowTP discovery mounts the remotes under `/peers`; the console and stats come from NowTP's diagnostics |
| Master page fanning out over HTTP from the browser | a view with `each("/peers", "$/sequencer/status")` and actions on the master that write to every peer |
| Fleet OTA via GitHub | `/sys/firmware` on each peer, driven through the gateway (§5.4) |
| `JsonTimer` daily timers | `/sys/timers` with `path` `/sequencer/execute` |
| Config export and import | `/sys/backup` |
| Front-panel button and LED | the application's own; `dev.onState()` for the LED's network states |

Commands broadcast to every remote (`b`) become writes to each peer through the gateway, or a scheduled start with NowTP's shared clock (§9).

## 11. Footprint

Budgets, measured on a classic ESP32 (ESP-IDF, same method as TesserAPI §15: an example against a baseline build) and enforced in CI:

| Item | Budget |
|---|---|
| Flash: Device + System + Network + Time + Firmware + Backup, above TesserAPI + TesserUI | ≤ 60 KB (mDNS and the DNS server included; ESP-IDF's Wi-Fi stack excluded, it is in every baseline) |
| Flash: the UI | TesserUI's bundle, ≤ 50 KB |
| Flash: an application with every first-release service, Wi-Fi and HTTP | ≤ 1.25 MB, to fit Arduino's default partition table |
| RAM at rest, above Wi-Fi and `esp_http_server` | ≤ 12 KB, the task's stack included |
| Stack | TesserKIT's task: 6 KB |

**Measured** (2026-10-10, ESP-IDF 6.1-dev, ESP32, `-Os`): `examples/idf/minimal` against its `-DFOOTPRINT_BASELINE=1` build, the same application on TesserAPI, TesserUI and a plain Wi-Fi station.

| Build | Above the baseline |
|---|---|
| Device + System + Network + Time + Firmware + Backup | 113.3 KB |
| The same with `CONFIG_TESSERKIT_MDNS=n` (`TESSERKIT_NO_MDNS`) | 77.8 KB |
| `fw.downloads()` adds TLS, the certificate bundle and the HTTP client | about 130 KB |

- **Where it goes** (with mDNS): TesserKIT's own code 42 KB (Network 15 KB, most of it the `std::function`s behind bound values and the keyed list), ESP-IDF's `mdns` 34 KB, libc's time zone support 17 KB (`tzset()` parses with `vfscanf`), lwIP's DHCP server, DNS client and sockets 7.6 KB, the access point in the Wi-Fi driver 6 KB, OTA 2.6 KB.
- **Against the budget:** 60 KB is exceeded. Features stay, as options where they are large (mDNS, downloads); the rest is smaller only at the cost of clarity, and is left as it is until a measured application needs the space.
- **Arduino:** the Minimal sketch with every first-release service is 1.18 MB on ESP32 and ESP32-C3, within the default table's 1.25 MB slot.

**Partition table** (`partitions/tesserkit_4mb.csv`, recommended): `nvs` 20 KB, `otadata`, `ota_0` and `ota_1` 1.94 MB each, `coredump` 64 KB. No filesystem, no `phy_init` (the PHY data is in the app by default). `tesserkit_8mb.csv` has 3.94 MB slots.

**Comparison:** the same measurements are taken on ESP32-sveltekit's demo with matching features (Wi-Fi, AP, NTP, upload OTA, status), and reported in the README.

## 12. Testing

1. **Host tests** (CMake, ASan and UBSan, the same warnings as TesserAPI): every service against the fake platform. Wi-Fi state machine (join, lose, fall back to the AP, recovery), time and DST, timers (catch-up, no double runs), firmware upload checks, backup round trips, tokens (expiry, revocation, brute-force delay), the MQTT bridge's topic mapping. The fake platform is a CMake target, `tesserkit_fake` (`fake_platform.h`, `fake::Platform::install(dev)`), so applications' host tests and simulators show `/sys` too.
2. **Golden schemas:** each service's schema with its hints, checked in as `fixtures/*.json`. TesserUI renders them in its golden-model tests, so a hint change shows as a diff in both repositories.
3. **On target:** `examples/idf/minimal` (and `examples/arduino/Minimal`, with `--app /lamp`), driven over HTTP and WebSocket by `test/e2e_device.py`: status, the interface, subscriptions, a scan, SNTP, sign-in with tokens over HTTP and WebSocket, a timer running, persistence across a restart, backup and restore, refused images, an OTA upload, its confirmation and rollback, and an image that crashes at boot (`-DCRASH_AT_BOOT=1`) rolled back by the bootloader, with its core dump downloaded. Provisioning: `test/captive_probe` is a firmware that plays a phone on a second board. It joins a fresh device's access point (built with `-DNO_DEFAULT_NETWORK=1`), checks that any name resolves to the device and that Android's and Apple's connectivity probes are redirected to the interface, then saves a network through the API and watches the device join it and the access point go. *(later)* a test firmware with every service and serial scripts.
4. **CI:** host tests, ESP-IDF builds (5.1, 5.4, 5.5, latest × ESP32, ESP32-C3, ESP32-S3), Arduino-ESP32 3.x builds of the examples, the footprint budgets.

## 13. Packaging

```
TesserKIT/
  docs/DESIGN.md
  src/TesserKIT.h
  src/tesserkit/            device, services
  src/tesserkit/platform/   ESP implementation of the platform layer
  partitions/               tesserkit_4mb.csv, tesserkit_8mb.csv
  examples/arduino/         Minimal, Heliostat-like, UiHost
  examples/idf/             minimal
  test/                     host tests, fake platform, test firmware
  CMakeLists.txt, idf_component.yml, library.json, library.properties
```

- ESP-IDF component and PlatformIO/Arduino library, like TesserAPI and TesserUI. Dependencies: TesserAPI and TesserUI as public requirements; NowTP detected as TesserAPI does (`__has_include` or the build's component list).
- Versions follow semver; TesserKIT pins compatible TesserAPI and TesserUI ranges.

## 14. Follow-ups wanted in other layers

**TesserAPI** (phase 7 of its roadmap; each with its section in TesserAPI's DESIGN.md). Done as of 2026-10-11.

| Item | For |
|---|---|
| Access levels: `tesser::Access`, `Client::access`, `readAccess()`/`writeAccess()` on nodes, filtered reads, `api.authenticate(fn)` for tokens | §5.7, §7 |
| Secret values: `.secret()`, never rendered | passwords (§5.2, §5.7, §5.8) |
| Typed object arguments for actions, and typed deferred actions | `login({name, password})`, helionext's `add`, `download(url)` |
| Keyed lists: `.key("name")` | networks, users, timers, helionext's sources |
| Scalar arrays | calibration tables, spin-seq's 128 encoder offsets |
| File nodes: streamed upload and download over HTTP | firmware upload, backups, core dumps |
| Streamed replies on WebSocket and serial | large schemas without paging |
| `view=hash` | TesserUI's schema cache for directly reached nodes |
| Automatic re-advertisement when the tree changes | gateways with mounted peers |
| `Storage::erase()`, `Api::restore()` | factory reset, backups |

**NowTP:**

| Item | For |
|---|---|
| Channel following | Network changing networks (§5.2) |
| Authenticated pairing, persistent peers | trusting peers without trusting MACs (§7) |
| A shared clock (offset estimated from pings) | synchronized starts (§9) |
| Streaming of large messages | firmware updates of NowTP-only nodes (§5.4) |

**TesserUI** (done as of 2026-10-10, except the custom elements):

| Item | For |
|---|---|
| `file` widget: upload with progress, download | §5.4, §5.5 |
| Secrets: a password input that never shows the value and sends only edits | §5.2, §5.7 |
| Access-aware rendering: the schema's `access`, disabled controls, a login form for `/sys/auth/login` | §5.7 |
| `flags` widget (bitmask with labels) | §5.6 |
| `timezone` widget | §5.3 |
| `$now` as a `ui.value` | §5.3 |
| Keyed lists and arrays in tables; arrays as charts | §5.2, §10 |
| `x-keyframes`, `x-sky` custom elements (examples, not in the bundle) | §10.1 |

## 15. Roadmap

| Phase | Content | Acceptance |
|---|---|---|
| 0 | TesserAPI follow-ups of §14 | Host-tested; the HTTP and WebSocket parts checked on hardware. |
| 1 *(done)* | Device, System, Network, Time, Firmware (upload, download, rollback), Backup; the TesserUI widgets they need | A fresh classic ESP32 is provisioned from a phone through the captive portal, updated from the browser, rolled back after a crashing image, backed up and restored. Footprint within §11, compared with ESP32-sveltekit. |
| 2 *(done)* | Timers, Auth | spin-seq's timers and a locked-down device. |
| 3 | helionext on TesserKIT, in tiny-heliostat (§10.1) | Every helionext page usable, with custom elements only for the keyframes and the sky. |
| 4 | spin-seq on TesserKIT, NowTP channel following and pairing, shared clock | A master and three remotes run a synchronized sequence; fleet update through the master. |
| 5 | Mqtt, GitHub releases, Battery, Sleep, Ethernet, Log | *(later)* |

## 16. Decisions

- **A library rather than a template:** ESP32-sveltekit's forks (helionext, spin-seq) carry the framework's code and diverge from upstream. A library is updated by version.
- **Services under `/sys`, generic UI:** the alternative, a hand-written page per service, is what makes ESP32-sveltekit's interface 240 KB and every service cost a page.
- **NVS, no filesystem:** the persisted state is small (a few KB), NVS levels wear, and the flash a filesystem partition takes goes to the app slots instead.
- **The platform layer:** the services' logic (state machines, validation) is where bugs live, and it is host-testable only if ESP calls are behind an interface. ESP32-sveltekit has no tests.
- **Stateless signed tokens** rather than sessions in RAM: logins survive restarts and OTA updates, and nothing grows per client. Revocation is all-or-nothing (`logoutAll`), acceptable for devices with a handful of users.
- **Firmware updates checked on the device, not in the browser:** the browser may have no internet (a field on the device's access point), and the device is the one that knows its chip and slot.
