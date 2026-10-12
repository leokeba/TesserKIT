# TesserKIT design

Status: draft, written before any code. Everything here is the specification the code will follow; when the two disagree, fix one of them. Sections marked *(later)* are committed to but not part of the first release. TesserAPI and TesserUI references point to their own DESIGN.md: [TesserAPI](https://github.com/leokeba/TesserAPI/blob/main/docs/DESIGN.md), [TesserUI](https://github.com/leokeba/TesserUI/blob/main/docs/DESIGN.md).

## 1. Goals

TesserKIT is the device framework built on [TesserAPI](https://github.com/leokeba/TesserAPI) (the tree and its transports), [TesserUI](https://github.com/leokeba/TesserUI) (the interface rendered from the tree's schema) and, optionally, [NowTP](https://github.com/leokeba/NowTP) (ESP-NOW). It provides what every connected device needs besides its own function: network setup, time, firmware updates, backups, users, MQTT, system status. It aims at most of [ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit)'s functionality at a fraction of its footprint.

- **A library, not a template.** Applications depend on TesserKIT; they don't fork it. Upstream fixes arrive by updating a version, not by merging.
- **Every service is a subtree.** A service describes itself into the TesserAPI tree under `/sys`, with values, actions, events, lists and files. It is then reachable over every TesserAPI transport (HTTP, WebSocket, serial, NowTP), through gateways, and from MQTT, with no endpoint code of its own.
- **Zero UI code.** Services carry TesserUI hints (labels, units, widgets, confirmations, layout); the generic renderer draws their pages. No service has a hand-written page, and neither does an application unless it wants one (custom elements, TesserUI §9.8).
- **Pay for what you use.** A service that isn't instantiated isn't linked. There are no feature flags.
- **Small.** The whole interface is TesserUI's bundle (≤ 50 KB gzipped, against ESP32-sveltekit's ~240 KB). Settings live in NVS: a filesystem partition is needed only by the optional Files service (§5.12). Measured budgets in §11.
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
| Partitions | Every first-release service fits a 1.25 MB app slot, so Arduino's default 4 MB table (two OTA slots, no `min_spiffs`) works; with Mqtt an Arduino sketch needs a larger slot. TesserKIT ships a table without a filesystem for more headroom, and one with a LittleFS partition for Files (§11). |
| Dependencies | TesserAPI, TesserUI (C++ part: `WebUi`), ArduinoJson 7. Optional: NowTP. ESP-IDF components only (`esp_wifi`, `esp_netif`, `esp_eth`, `mdns`, `esp_https_ota`, `mqtt`, `esp_http_client`, `nvs_flash`, `app_update`; `joltwallet/littlefs` for Files, which Arduino-ESP32 ships): the same code on Arduino and ESP-IDF. |
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
  artnet/               ArtNet (§5.9)
  bluetooth/            Bluetooth (§5.10)
  nowtp/                NowTp (§5.11)
  files/                Files (§5.12)
/peers                  the nodes NowTp finds nearby, mounted beside the application's tree
/sequencer              Sequencer (§5.13): the application's, so at the root
```

- The root stays the application's: `/sys` is one child of it, rendered by TesserUI as a link to its own page, where each service is a card.
- Service subtrees are stable API: other devices, scripts and MQTT topics address them by path. Renaming a node is a breaking change, listed in the changelog.
- Every service describes itself through a public `describe(tesser::Object&)`, so an application that wants a different layout (no `/sys`, or a service under its own object) mounts it itself: `api.mount("wifi", net)` and `dev.begin(tesserkit::NoSys)`.

### 3.3 Platform layer

Services never call ESP-IDF directly. They call `tesserkit::Platform`, a set of narrow interfaces: `Core` (chip information, heap, restart, tasks), `Wifi` (station, access point, captive portal), `Ethernet` (the internal MAC and its PHY), `Mdns`, `Clock` (the clock, the time zone, SNTP), `Ota` (the OTA slots), `HttpClient` (downloads and release checks), `Coredump`, `Udp` (Art-Net), `MqttClient`, `Ble` (Bluetooth LE), `NowTpRadio` (ESP-NOW) and `Filesystem` (LittleFS: mounting, usage, formatting; the files themselves through stdio and `dirent`, the same on the host):

- `src/tesserkit/platform/esp/` implements them on ESP-IDF APIs, which Arduino-ESP32 3.x also ships, so one implementation serves both.
- `test/fake_platform.*` implements them in memory for host tests: a Wi-Fi radio whose scans and connections the test scripts, a clock the test advances, an OTA partition that's a byte vector.
- **Services also hear of a factory reset** (`Service::factoryReset()`), before the restart, to erase what they keep outside the device's storage: NowTp's installation key.
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
  ethernet/            with a board (below)
    status/            link, connected, ip, speed (Mbps), mac
    static/ *(p)       { enabled, ip, mask, gateway, dns } ui.form, ui.collapsed, Admin
    joined             event, ok toast: "192.168.1.60 on Ethernet"
  wifi/
    networks *(p)      keyed list (key "ssid"), ≤ 5, Admin, readAccess User:
                         ssid ≤ 32, password (secret) ≤ 64,
                         static/ { enabled, ip, mask, gateway, dns } ui.collapsed
    scan               deferred action, Admin → [{ssid, rssi, channel, secure}]
    status/            connected, ssid, ip, rssi (watched, dBm), channel, bssid
    joined             event, ok toast: "192.168.1.50 on Workshop", the address clients on the AP must follow
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

**Ethernet:** `net.ethernet(board)` declares the board's wiring: the PHY, its address on the MDIO bus, the MDC and MDIO pins, the pin powering the PHY or its oscillator (driven high before the MAC starts, whose reset needs the clock), and the RMII clock (in on GPIO 0, or out on 0, 16 or 17). Presets in `tesserkit::boards`, from WLED's table: `gledopto()` (Gledopto's controllers with Ethernet: LAN8720 at 1, MDC 23, MDIO 33, power 5, clock in), `wt32Eth01()`, `olimexEsp32Poe()`, `esp32EthernetKit()`. Without the call, the driver isn't linked; with it, Ethernet adds about 40 KB (§11). The ESP32's internal MAC only, so the classic ESP32 (and the ESP32-P4, untried); elsewhere the subtree is absent and Network runs as without it.
- **Ethernet first:** while Ethernet has an address, the station keeps away (it disconnects if it was joined): one address on the network, and the radio's time for the access point and ESP-NOW. Without a cable, or when the link goes, the station joins as usual, at once. At start, and when the link comes up, the station waits 5 s for Ethernet's address before joining (time for the link and DHCP); a link without an address after that lets the station join, and leave again when the address comes.
- **The access point** treats Ethernet's address as the station's: `fallback` brings it up 15 s after the device went offline, and down 30 s after Ethernet's address came. **Recovery** waits those 15 s too on a board with Ethernet, so a device on a cable with no saved Wi-Fi network doesn't open an access point at each boot.
- `static` replaces DHCP, applied at once: the old address goes with the old settings. A static address is there at once; back on DHCP, Ethernet has no address until a lease comes, which the station waits 5 s for as after a link. `joined` is announced on each new address.
- **PHY drivers:** ESP-IDF 5.x has drivers for LAN87xx, IP101, RTL8201, DP83848 and KSZ80xx; 6.0 moved them to the esp-eth-drivers components and keeps the generic IEEE 802.3 driver (from 5.4), which TesserKIT uses for every PHY there.
- **State and address:** the device is `online` over either interface, and `provisioning` only with no saved network and no link. `dev.address()` is Ethernet's address, else the station's, else the access point's, and `dev.addressMac()` the MAC of that interface (the station's on Wi-Fi): Art-Net's poll replies give both.

**Access point:**
- `fallback`: up when no saved network is joined within 15 s of trying, and down 30 s after the station connects (time for a phone on the AP to follow the device's new address). `always`: always up (AP+STA). `never`: never, except in recovery (below).
- **Recovery:** with no saved network, the AP comes up whatever the mode, so a fresh device is always reachable.
- **Captive portal:** while the AP is up and the station isn't connected, a DNS server answers every name with the AP's address, and the HTTP server's 404 handler redirects to `/` (`302`). OS connectivity probes (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`) thus all land on the UI, so phones open it on joining. This costs no URI handler slot.

**mDNS:** `<hostname>.local`, with `_http._tcp` and TXT records `api=/api`, `ws=/ws`, `version=<version>`, `tesser=1`, and `_tesser._tcp` with `api` and `ws`, what TesserAPI's `HttpServer::advertise()` announces and TesserUI's bridge lists for the launcher (§8), on ESP-IDF's `mdns` component (about 34 KB of flash). On by default, since it is how a provisioned device is found; `-DTESSERKIT_NO_MDNS` leaves it out, with the `mdns` setting.

**Channels and NowTP:** a station follows its router's channel, and ESP-NOW must share it (TesserAPI §14). NowTP follows by itself: a node whose station or access point fixes its channel is an anchor, and the nodes nearby follow it when it moves (§5.11). Network reports channel changes to `dev.onChannel(fn)` as well. Without a router, the AP's `channel` is the field's ESP-NOW channel. Network reports its state (provisioning, connecting) from its start, so services that wait for the network see it at once.

**Not in the first release:** Ethernet modules on SPI (W5500, *(later)*, same tree), WPA2-Enterprise, IPv6 configuration.

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
  updateFailed  event, error toast: a download's error (with fw.downloads(); an upload's is its reply)
  pending       boolean: running a new image that isn't confirmed yet
  confirm       action, Admin: mark the running image valid now
  rollback      action, Admin, ui.confirm: boot the previous image
  github/       (with fw.github("owner/repo", "name-{target}.bin"))
    repo        string
    latest      string: the latest release's tag
    status      enum: unchecked, checking, current, available, failed
    error       string (status = failed)
    newRelease  event, info toast: the tag of a newer release an automatic check found, once per release
    autoCheck   boolean, persisted, Admin
    check       deferred action → the latest release's tag
    update      action, Admin, ui.confirm: install the latest release (status = available)
```

- **Upload:** a `PUT /api/sys/firmware/image` with the raw `.bin` as body, streamed to the next OTA slot through a TesserAPI file node (§14). TesserUI's file widget shows progress. The image header is checked on the first chunk (wrong chip or not an app image is `invalid_value` before anything is written). A completed upload sets the boot slot and restarts after the reply. The announced size is erased in 64 KB blocks just ahead of the bytes written: a 1 MB image uploads in 12 to 15 s on a classic ESP32 (25 s erasing sector by sector as it arrives). Erasing the whole image at once is as fast but keeps the idle task from running for over 5 s, which trips the task watchdog (a panic on Arduino).
- **Download** (`fw.downloads()`, before `begin()`): `esp_http_client` in a task of its own (8 KB of stack), following redirects (release assets redirect to a CDN), streaming through the same checks as an upload, with progress in `progress`. HTTPS uses ESP-IDF's certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`). TLS and the client cost about 130 KB of flash, hence the opt-in: without `downloads()`, neither the action nor the client is linked. The action replies at once; completion shows in `state`.
- **Rollback:** with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a new image starts as `pending`. It is confirmed automatically once the device has been `online` (or serving, without Wi-Fi) for 60 s, or by `confirm`. A crash or reset before that boots the previous image. `rollback` makes the previous image boot next; it then starts `pending` too. ESP-IDF refuses an update while the running image is unconfirmed, so starting one confirms it: it is evidently working. Arduino-ESP32 confirms every new image at boot unless the application defines `verifyRollbackLater()`; TesserKIT defines it (returning true) whenever Firmware is linked, so the same rule applies there.
- **GitHub releases** (`fw.github(repo, asset)`, before `begin()`; implies `downloads()`): the device asks `https://api.github.com/repos/<repo>/releases/latest` itself, so the browser needs no internet, unlike ESP32-sveltekit. The check runs in a task of its own (8 KB of stack) and reads the answer through an ArduinoJson filter as it arrives, keeping only the tag and the assets' names and URLs: WLED's answer is 72 KB. The asset named after the pattern is the image, with `{target}` replaced by the chip (`esp32`, `esp32c3`...) and `{version}` by the tag without its `v`. The status is `available` when the tag is later than `version` in semver order (a leading `v` ignored). A git describe version (`v1.2.0-3-g1a2b3c4`, `-dirty`) counts as later than its tag, so a development build isn't offered the release it started from; a version that isn't semver is offered any other release. `update` installs the asset as `download` would. A URL instead of `owner/repo` names another latest-release endpoint answering as GitHub's does (Gitea, Forgejo, `test/e2e_github.py`'s server). About 10 KB of flash above `downloads()`, linked only when `github()` is called.
- **Checks:** `check`, and with `autoCheck` (on by default) once the device has been online for a minute, then daily, or an hour after a failure; never during an update. GitHub allows 60 unauthenticated requests an hour per address, far above a fleet's daily checks behind one router. Failures say why: `no release found` (404: no release yet, or a private repository), GitHub's rate limit (403, 429), a release without this chip's image (`v1.2.0 has no name-esp32c3.bin`, `latest` still shown), an unreadable or cut answer. On a classic ESP32, checking WLED's release takes 5 s and briefly takes about 50 KB of heap, mostly TLS. Releases come from CI (§13).
- **Fleets:** a gateway reaches its Wi-Fi peers' `/sys/firmware/download` like any other path (`/peers/<name>/sys/firmware/download`), so "update all" is one write per peer, without the browser fanning out (spin-seq's Remotes page). NowTP-only peers need NowTP streaming *(later, §14)*.

### 5.5 Backup

```
/sys/backup
  settings      file, Admin: download = the persisted state as JSON; upload = restore it
  restartAfter *(p) boolean (default true)
  save          action (string: a name, "" for the date), Admin: store the settings on the device   (with Files)
  stored        read-only keyed list (key "name"), newest first, Admin:                            (with Files)
    name, size (B), saved (epoch s)
    content     file: download it, as <hostname>-<name>.json
    restore     action: restore it, as an upload does
    delete      action
  keep *(p)     integer 1–50 (default 10): stored backups kept                                      (with Files)
```

- **Download:** `api.persistedState()`, secrets included (it is Admin-only), as `<hostname>-settings.json`.
- **Restore:** applied with `api.restore(json, &skipped)` (TesserAPI §12): lenient like a boot load, so a backup from an older firmware restores whatever still fits; the reply (`{"skipped": [...], "restart": bool}`) lists the skipped paths. Every record is saved at once, then the device restarts one second later if `restartAfter` was set when the restore began (the backup's own `restartAfter` applies to the next one).
- **Stored on the device** when the application has a Files service (§5.12): `save` writes the settings to `backups/<name>.json` there, replacing a backup of that name. An empty name is the local time (`2026-10-12-1430`, `-2` if taken) once the clock is valid, else `backup-001`, `backup-002`... Names are 1 to 27 characters (the file name's 32 with `.json`). Beyond `keep`, the oldest backups go, by modification time (a clock that wasn't set reads as oldest), never the one just saved. A timer can call `/sys/backup/save` daily (§5.6). `backup.save(name)` does the same from the application, and `backup.keep(n)` sets the default.
- **Undoable restores:** before any restore (an upload or a stored backup), the settings it replaces are stored as `before-restore`, overwritten by the next restore and not counted by `keep`.
- **Factory reset** removes the stored backups: they hold the settings, secrets included, which a reset must erase. Other files stay.
- **Linked with Files only:** Backup reaches the stored backups through `Files`' virtual functions, so a firmware without Files doesn't link them (about 37 KB on Arduino, mostly the file helpers and `snprintf`).
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
/sys/timerFailed    event, warn toast: "<name>: <error>" for a run that failed
```

- At the given local time on the given days, a timer performs `set <path> <value>` through the API, as an Admin local client, exactly as if a client had sent it. Its result is logged and kept in a read-only `last` (`"ok"` or the error), which a boot load and a restored backup bring back with the list.
- A write to a timer checks that `path` resolves and that `value` parses; whether the value fits the node is checked when it runs (the node may accept different values by then). Below a list, an array, a remote node or a `*` segment (every child, TesserAPI's fan-out, §14), the path can't be checked further and is accepted. The check applies once the service has started: a boot load keeps stored timers whose node the application adds after `begin()`, and a run then reports `not_found`.
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
- **Bluetooth clients** (TesserAPI's `BleTransport`) aren't trusted, since anyone in range connects: like a network client without a token, they are Admin while the device is open and public after. They sign in on their line: `login` gives the token, an `auth` envelope presents it until the connection drops (TesserAPI §10.4), as TesserUI does after its sign-in dialog.
- Serial clients stay Admin (physical access). NowTP peers are Admin when trusted (TesserAPI §13): a gateway's own access checks apply to its browser clients before it forwards.

### 5.8 Mqtt

```
/sys/mqtt               label "MQTT", Admin
  enabled *(p)          boolean (default false)
  uri *(p)              string: mqtt://, mqtts://, ws:// or wss:// ("mqtt://broker.local:1883")
  user *(p), password *(p) (secret)
  clientId *(p)         string (empty: the hostname), advanced
  prefix *(p)           string (empty: "tesser/<hostname>"), no + or #, no slash at either end, advanced
  publish *(p)          list ≤ 8 of {path, interval ms 0–86400000 (default 1000)}
  status/               connected, error, published, dropped (watched counters)
  lost                  event, warn toast: a connected session dropped by the broker or the network, with the error
```

- **Session:** the client connects once the device is online, and reconnects by itself (ESP-IDF's esp-mqtt, every 5 s, keepalive 30 s). A change to `enabled`, `uri`, the credentials, `clientId` or `prefix` restarts the session once the settings have been still for 1 s, so editing several fields reconnects once. Each session has a number, carried by its platform events, so a stopped session's late events are ignored.
- **Bridge:** while connected, each `publish` entry is a subscription of its own (TesserAPI `LocalClient`, Admin: what to publish is the admin's choice) with its `interval`. Its snapshot and every change notification are flattened into leaves, each published as JSON to `<prefix>/<path>`, retained: `/led` gives `tesser/lamp-a67c/led/on` → `true`. Arrays and keyed lists are leaves (their JSON). Events under the path go to `<prefix>/<event path>` unretained. A notification flagged `overflow` is followed by a read of the whole subtree. The subscriptions end with the connection, and a reconnection publishes everything again, so retained topics are refreshed. They count against TesserAPI's 16 subscriptions.
- **Commands:** the client subscribes (QoS 1) to `<prefix>/+/set`, `<prefix>/+/+/set` … 8 levels deep, rather than `<prefix>/#`, which would bring back every retained topic it publishes. A message on `<prefix>/<path>/set` becomes `set <path> <payload>` through a `LocalClient` with **User** access (TesserAPI `setAccess`), so the broker's clients can drive the application but can't reconfigure `/sys`. The payload is JSON; anything that doesn't parse is a string (`ON`, a name), and an empty payload is `null` (an action without argument). The reply goes to `<prefix>/<path>/result`, unretained: `{"status": "ok", "body": true}` or the error.
- **Availability:** `<prefix>/status` is `online` on each connection and the last will is `offline` (QoS 1), both retained. Stopping a session publishes the will itself before disconnecting, since a clean disconnection doesn't.
- **Platform:** `MqttClient` (§3.3) on esp-mqtt, a component of ESP-IDF 5 and `espressif/mqtt` from ESP-IDF 6 (a dependency of TesserKIT's manifest for those versions). Publishing only queues: each session's task of the platform layer (`tk_mqtt_pub`, 4 KB of stack) sends with `esp_mqtt_client_publish()`, from the session's queue of 32 messages and 16 KB, so TesserKIT's task never waits on the network. Once the session stops, that task skips what is left, publishes the will, destroys the client and ends, freeing its queue: a stopped session holds no memory. esp-mqtt's own queue (`esp_mqtt_client_enqueue`) isn't used: its task sends one queued message per pass and waits up to a second between passes. A message the queue refuses counts in `dropped`.
- **TLS:** `mqtts://` and `wss://` brokers are verified with ESP-IDF's certificate bundle when the application calls `mqtt.tls()` before `begin()`; without it the bundle isn't linked.
- **Factory defaults:** `mqtt.broker("mqtt://...", user, password).enable(true).prefix("home/lamp").publish("/led", 0)`.
- **Home Assistant discovery** *(later)*: for each published leaf, a config under `homeassistant/<component>/<hostname>/<path>/config`, the component chosen from the schema: writable boolean → `switch`, read-only boolean → `binary_sensor`, number with a range → `number`, read-only number → `sensor` (with `unit`), enum → `select`, action without argument → `button`.
- One generic bridge replaces ESP32-sveltekit's per-service `MqttEndpoint`s.

### 5.9 ArtNet

```
/sys/artnet             label "Art-Net", Admin
  enabled *(p)          boolean (default true)
  universe *(p)         integer 0–32767: the 15-bit port-address (net, sub-net, universe)
  mappings *(p)         list ≤ 8 of {path, coarse 0–512, fine 0–512, min, max, threshold 0–255}
  status/               packets, universe (the last seen), received (hidden), age (s since a frame of ours, -1 none; shown once received), error
  refused               event, warn toast: a new error in status/error
```

- **Receiving:** an Art-Net 4 node on UDP 6454 (unicast and broadcast). TesserKIT's task drains the socket without blocking every period and keeps the last ArtDmx frame of its universe; ArtDmx for other universes only counts.
- **Mappings:** each mapping turns channels of the frame into a write to `path` through the API (a `LocalClient`, Admin, as Timers do), **only when the written value changes**. `coarse` is the channel (1–512, 0: unused), `fine` the low byte's (0: 8 bits). The 16-bit value `v` (8 bits: `coarse × 257`, so 255 is the top) gives `min + v/65535·(max − min)`, written as a number, rounded for an integer node, or `true` from half up for a boolean node. A `threshold` (1–255) makes a gate: `true` while the coarse channel is at or above it. A frame too short for a mapping's channels leaves it alone. New mappings or a new universe write everything again on the next frame.
- **Errors:** a refused write is kept in `status/error` (`/led/brightness: invalid_value: out of range`) until a write to the same path succeeds.
- **Discovery:** an ArtPoll is answered with an ArtPollReply, unicast to the controller on port 6454: the device's address (station, else access point), its MAC, version, short name the label, long name the hostname, one DMX output port on the configured port-address, and a node report with the packet count.
- **Platform:** `Udp` (§3.3) on lwIP sockets. A port that can't be opened is tried again every 5 s.
- **Factory defaults:** `artnet.universe(0).map({"/heliostat/artnet/azimuth", 1, 2, 0, 360})`.
- What a value means stays the application's: tiny-heliostat maps channels onto `/heliostat/artnet/azimuth` and `elevation` and decides there whether Art-Net takes over (§10.1). It replaces helionext's `ArtNetService` and serves any lighting-controlled device.

### 5.10 Bluetooth

```
/sys/bluetooth          label "Bluetooth", ui.icon "bluetooth", Admin
  enabled *(p)          boolean (default true)
  name                  the name advertised: the hostname
  restartNeeded         hidden; restartWarning, a warning callout while it is true
  status/               connected, client (the address)
```

- **Transport:** TesserAPI's `BleTransport`, the Nordic UART Service carrying envelopes as a serial line does, for TesserUI's launcher (TesserUI §9.7): a phone reaches the device with no network in common. NimBLE on ESP-IDF (`CONFIG_BT_NIMBLE_ENABLED`), Arduino's BLE library otherwise; without either, the service logs that the build has no Bluetooth.
- **Access:** anyone in range connects, so a Bluetooth client is a network client: Admin while the device is open (§5.7), then the level of the token it presents with an `auth` envelope (TesserAPI §10.4), which TesserUI sends after signing in. The token holds until the client disconnects.
- **At runtime:** switching it off drops the client and stops the stack, giving its memory back (`BleTransport::end()`: about 60 KB on a classic ESP32 with ESP-IDF, the host and the controller); switching it on starts it again. A client may switch it off over Bluetooth: its reply is lost with its connection. On Arduino, whose BLE library leaks its service at each deinit, switching off only stops advertising. The name follows the hostname, from the next advertisement. A transport that can do neither shows the restart warning instead.
- **Platform:** `Ble` (§3.3). On ESP-IDF the transport handles requests in its own task; Arduino's in `poll()`, which the platform layer runs in a task of its own (`tk_ble`), outside TesserKIT's task, since a request takes the API lock.
- **Memory:** NimBLE releases classic Bluetooth's controller memory on ESP32. The examples configure it as what `BleTransport` uses, a peripheral with one client: no central or observer role, no GATT client, no bonding, one connection.
- **Factory default:** `bluetooth.enable(false)` keeps it off until enabled.
- **Boards:** about 200 KB of flash on ESP-IDF. On Arduino, the classic ESP32's prebuilt Bluedroid includes classic Bluetooth and fills a 1.94 MB slot with TesserKIT's services: ESP32-C3 and ESP32-S3 only (about 1.5 MB).

### 5.11 NowTp

```
/sys/nowtp              label "NowTP", ui.icon "target", Admin
  enabled *(p)          boolean (default true)
  mountPeers *(p)       boolean (default true): the TesserAPI nodes found are mounted under /peers
  channel *(p)          1–13 (default 1): when neither a Wi-Fi network nor the access point fixes it, advanced
  longRange *(p)        boolean: Espressif's Long Range mode besides 802.11b/g/n, advanced
  pairingCode *(p)      secret: what both sides of a pairing must know, advanced
  restartNeeded         hidden; restartWarning, a warning callout while it is true
  pairing/              secured (paired), state (text), pair, accept, forget (confirm, danger)
  nodes                 read-only keyed list (key "mac") of {name, mac, rssi, channel, tesser, authenticated}
  status/               started, channel, timeSynced, timeReference, sent, received, failed
```

- **Transport:** NowTP on ESP-NOW (discovery, channel following, pairing, network time) with TesserAPI's `NowTpTransport` serving the tree to the nodes nearby, and advertising it in discovery, so they mount this device too.
- **UI host:** with `mountPeers`, the TesserAPI nodes found are mounted under `/peers` (label "Nodes nearby"), each a remote node with its page in TesserUI's menu (TesserUI §6.3), forwarded over ESP-NOW. `nodes` lists every node heard, TesserAPI or not.
- **Starting:** once the network has settled, joined or on its own access point (at the latest 20 s after boot, when the fallback access point is up): while the station scans and joins, ESP-NOW can't hold a channel. Its channel is then the router's or the access point's; `channel` only applies to a device with neither.
- **Pairing:** `accept` opens this node's pairing window for 60 s, `pair` takes the installation key from a node whose window is open (NowTP's X25519 exchange, proving the pairing code). Both run in a task of their own and report in `pairing/state`. With a key, NowTP drops every message not authenticated with it.
- **Access:** nodes holding the installation key are trusted, so Admin: the transport trusts every sender while the network is secured, which only key holders can reach. Without a key, a node is a client without a token: Admin while the device is open, public after (TesserAPI §13). A gateway checks its own clients before it forwards (§9).
- **At runtime:** `enabled`, `channel` and `longRange` apply at once; `mountPeers` and `pairingCode` at the next boot (the restart warning). A factory reset also forgets the installation key, which NowTP keeps in NVS of its own (namespace `nowtp`).
- **Platform:** `NowTpRadio` (§3.3), built when the build has NowTP (TesserAPI's `TESSER_HAVE_NOWTP`, or `<NowTP.h>` on Arduino). Mounted nodes keep pointing at the transport, so stopping NowTP stops the radio and the transport's task but keeps the transport.
- **Memory:** about 25 KB of heap while running (NowTP's event buffer and task, the transport's task, ESP-NOW).
- **Factory defaults:** `nowtp.mountPeers(false).channel(6).longRange(true).txPower(15).pairingCode("...")`.
- Replaces §9's `dev.peers()`: the service is how a TesserKIT device joins a NowTP field.

### 5.12 Files

```
/sys/files              Admin, read and write
  mounted       boolean, read-only (advanced)
  total, used   integers (B), read-only: the partition, and what its files take
  dir           string: the directory listed ("/", "/logs"); a write must name one
  up            action: the parent directory
  entries       read-only keyed list (key "name"), ≤ 64, directories first, then by name:
    name, size (B), isDir (ui.icon "folder"), modified (epoch s, 0 if the clock wasn't set; advanced)
    content     file: download it; upload replaces it (ui.visibleWhen "isDir=false"; maxSize: the partition)
    open        action: list this directory (ui.visibleWhen "isDir=true")
    rename      action (string): a new name, in the same directory
    delete      action: the file, or the directory with what it holds (ui.confirm)
  truncated     boolean, read-only (advanced): the directory holds more than 64 entries
  upload        file: a new file in `dir`, named by the client (TesserAPI's ?filename=); one of that name is replaced
  mkdir         action (string): a directory in `dir`
  refresh       action: list again (after the application wrote files)
  format        action: erase every file (ui.confirm, advanced)
```

- **The partition** is a LittleFS data partition labelled `files` (TesserKIT's `*_files.csv` tables, §11), else `spiffs` (Arduino's default and `min_spiffs` tables), mounted at `/files`; `files.partition(label)` and `files.root(path)` change them. A partition that holds no filesystem (new, or SPIFFS from an older firmware) is formatted. One an application already mounted (Arduino's `LittleFS.begin()`) is used as it is, at the root it was given. Without a partition, or without LittleFS in the build, `mounted` is false and every operation is `not_allowed`.
- **Names** are 1 to 32 characters, without `/`, `\` or control characters, and don't start with a dot; UTF-8 is fine. Paths below the root are at most 63 characters. Anything else is `invalid_value`, so no request reaches outside the root.
- **Uploads** go to a hidden temporary file (`.<name>`) beside the target and are renamed over it once complete, so a cut transfer or a power cut leaves the old file or the new one, never a part. An upload announcing more than the free space is `too_large` before any byte is written. Writes run outside the API lock (TesserAPI §4.6); the listing follows within a period of the device's task.
- **One directory at a time:** `dir` is the device's, shared by its clients: a browser opening a directory moves the others' listing too. Keys can't hold `/`, so a listing of the whole tree would need another key; one directory keeps every row addressable by name.
- **The application** reads and writes its own files through stdio below `files.root()`, or with the helpers (`files.write(rel, data)`, `read`, `remove`, `rename`, `mkdir`, `list`), which other services use too. After writing through stdio, `files.changed()` updates the listing.
- **Factory reset** doesn't format: files are often data rather than settings. Only the stored backups go (§5.5).
- Of the examples, only `examples/idf/full` has Files, on `tesserkit_4mb_files.csv`.

### 5.13 Sequencer

```
/sequencer              label "Sequencer", ui.icon "play"; the application's: at the root, beside its tree
  steps *(p)            list, ≤ 32, Admin; a step is addressed by index, and optionally named:
    name                string ≤ 24, optional; can't start with a digit, + or -
    path                string: the node to set or call (as in Timers, §5.6)
    value               string: its JSON value or argument ("null" for an action without one)
    wait                enum "reply" | "time" | "none" (default reply)
    ms                  integer 0–86 400 000 (ms)
    next                string: the next step, a name, an index, or "+1", "-2"… from this one; "" ends
  boot *(p)             string: the step to play from at boot (a name or an index; "" none), Admin
  finally/ *(p)         path, value: a set when playing stops (advanced), Admin
  play                  action (string): from a step (a name or an index; "" the first), following next, User
  execute               action (string): one step, without following next (advanced), User
  stop                  action, User
  status/               read-only, watched: running, step (index, -1 stopped), name, elapsed (ms), duration (ms), error
  failed                event, warn toast: "<step>: <error>"
```

- **A step is a write.** It performs `set <path> <value>` through the API as an Admin local client, like a timer: a value, an action with its argument, or a patch of an object (whose actions run after its values, TesserAPI §6.3). So a step sets several values and starts something at once, and the sequencer drives anything of the tree, the application's nodes, `/sys` and the nodes under `/peers`, with no command language of its own. A step without a name is shown as `#<index>`.
- **Waiting:** `reply` waits for the write's reply, which a deferred action sends when what it started has finished (a move); with `ms` > 0 it waits at most that long, reports `no reply in <ms> ms` and goes on, and a later reply is ignored. `time` waits `ms` from the step's start, or until stopped when `ms` is 0 (a last step that holds). `none` goes on at once, so steps that don't wait run together, in one period.
- **Next:** after its wait, a playing step goes to `next`, by name (the first step with it), by index, or relative to itself. Empty ends the sequence. A next that names no step reports it and stops. More step starts in one period than there are steps is a loop of steps that never wait: reported, and stopped. Whether next names a step is checked when it runs, so steps refer to steps written later.
- **Starting and stopping:** `play` and `execute` take effect at the device's next period (≤ 20 ms), and a step name that resolves to nothing is `not_found` at once. A play or an execute while playing takes over at its step. A stop and a play in one period apply in that order. `boot` plays at the first period after `begin()`.
- **Finally:** when playing stops (a stop, the end of a sequence, an executed step done, an unknown next, a loop), unless another play takes over, the `finally` set runs: a motor stopped, a light turned off. So `execute` of a step that waits a time is spin-seq's "run for d ms, then stop".
- **Failures** (a step's error reply, a timeout, an unknown next, a finally that failed) are logged, kept in `status/error` and sent as the `failed` event. A step's error doesn't stop the sequence: on a fleet, one node that doesn't answer shouldn't stop the others.
- **Checks on write:** `path` must resolve (with Timers' exceptions), `value` and `finally/value` must parse, `name`, `next` and `boot` must have the syntax of a name, an index or (`next` only) an offset. A step's own result isn't stored: it would be a write to the persisted list at every step.
- **Several:** `Sequencer(dev, "lights")` mounts at `/lights`, so an application has one per subject.
- **Several nodes:** a step on the master writes to the nodes under `/peers`: `/peers/spinner-2/sequencer/play` `"intro"`, or `/peers/*/sequencer/play` for all of them through TesserAPI's fan-out (§14). With `wait: reply`, the step waits for every node's reply; an offline or silent node is a failure (`timeout: 1 of 3 failed: spinner-2: ...`), and the others go on. Each node answering later takes one of the API's pending slots (`maxPending`, 4 by default): a master raises it to at least its number of nodes (`dev.api().config().maxPending = 8`, as `examples/idf/full` does). The writes leave one after the other, a few milliseconds apart per node; a start at the same instant on every node (NowTP's network time, §9) is *(later)*.
- **Factory defaults:** `sequencer.add(step)`, `.boot("intro")`, `.finally("/motor/stop", "null")`. From code, under the API lock: `play(step)`, `execute(step)`, `stop()`, `running()`, `current()`, `find(ref)`.
- This replaces spin-seq's JSON sequencer (§10.2), which had a command language for its motor and its broadcasts.

### 5.14 Later

| Service | Content |
|---|---|
| Battery | `soc` (%), `charging`, `voltage`; fed by the application (`battery.update(...)`) or an ADC pin with a divider. |
| Sleep | `sleep` action (deep sleep), wake-up pin and level *(p)*, `dev.onSleep(fn)` for the application to park its hardware. |
| Ethernet on SPI | W5500 and similar modules, for chips without the internal MAC (§5.2). |
| Log | §5.1. |

## 6. Persistence

### 6.1 Storage

- **Records:** TesserAPI's persistence (§12 there) saves the persisted nodes of the tree as one NVS blob per top-level node, debounced 2 s after the last change: `/sys` (every service's settings) is one record, each of the application's top-level objects another. A change rewrites only its own record, and NVS spreads writes across its pages, so frequent changes don't wear one sector.
- **No filesystem for settings:** the persisted state stays in NVS, with or without Files (§5.12). Files gives applications, and the stored backups, a LittleFS partition.
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

- **Menu:** `/sys` has `ui.page`, so TesserUI's menu (TesserUI §6.3) shows it as one entry with a sub-entry, and a page, per service. Each service names its icon (`ui.icon`): Status `info`, Network `wifi`, Time `clock`, Firmware `download`, Backup `refresh`, Timers `play`, Users `lock`, MQTT `link`, Art-Net `bulb`.
- **Services' pages** are their trees with hints: labels, units, `confirm` on destructive actions, `style danger`, `advanced` on rarely used settings, `form` where several fields must change together (the access point), `visibleWhen` for conditional fields (static IP), `chart` on heap and RSSI.
- **The application's pages** are its tree, plus TesserUI views (§8 there) for dashboards and custom elements (§9.8 there) for what generic widgets can't draw.
- **Notifications:** what happens in the background, after any reply, is an event with `ui.toast` (TesserUI §7.8) and `ui.hidden`: a toast, not an events log on the service's page. Network's `joined`, Firmware's `updateFailed` and `newRelease`, `timerFailed`, Mqtt's `lost`, ArtNet's `refused` and the Sequencer's `failed`. TesserUI shows them on any page, as "<parent's label> › <label>: <payload>" ("Wi-Fi › Joined: 192.168.1.50 on Workshop"), so their labels don't repeat their parent's. A moment a reply or a widget already shows isn't one: a failed upload, a manual release check, a pairing (its state is on the page), a restore. Clients only hear events while subscribed, so what a device does at boot (a rollback, a crash) stays a value (`resetReason`, `coredumpPresent`).
- **Login:** when a request answers `unauthorized`, TesserUI's sign-in dialog calls `/sys/auth/login` with a name and password (TesserUI §7.4). Without the Auth service it never needs to: everyone is Admin.
- **Theming and text:** `dev.web().theme(...)`, `.text(...)`, `.title(...)` (TesserUI §9.4).
- **The launcher** (TesserUI §9.7): the same bundle on GitHub Pages, `https://leokeba.github.io/TesserKIT/`, published by CI with the TesserUI the example firmware is built with (§13). It reaches a device without the device serving a page:
  - **USB:** Web Serial to a `LineTransport` on the console. The examples add one (`tesser::UartTransport` on UART0 with ESP-IDF, `tesser::StreamTransport` on `Serial` with Arduino); a serial client is Admin (§5.7), so a device is set up over a cable with no network at all. The launcher skips log lines, but one written in the middle of a reply breaks it: the ESP-IDF example logs warnings only.
  - **Network:** an address or `<hostname>.local`, from an HTTPS page, so the device must allow the launcher's origin: `dev.cors("https://leokeba.github.io")`, as the examples do. Chrome asks the user for local network access, and polls the device over HTTP (no WebSocket from HTTPS).
  - **Nearby:** TesserUI's bridge, on the user's machine, lists the devices announcing `_tesser._tcp` (§5.2) and serves them live.
  - **Bluetooth:** the Bluetooth service (§5.10): `examples/idf/bluetooth`, `examples/arduino/Bluetooth` (ESP32-C3 and ESP32-S3) and `examples/idf/full`. The device's name is its hostname.

TesserUI renders everything TesserKIT's services declare: files (§5.4, §5.5), secrets, access levels and sign-in, flags (§5.6), the time zone widget and `$now` (§5.3).

## 9. Several nodes

TesserKIT devices are TesserAPI nodes, so the multi-node features are TesserAPI's and TesserUI's:

- **UI host:** a TesserKIT device with the NowTp service (§5.11) mounts the nodes nearby under `/peers` and advertises itself. The UI shows a page per node, forwarded over ESP-NOW (TesserUI §9.5).
- **NowTP-only nodes** run TesserAPI and NowTP without TesserKIT's Wi-Fi services, or with a subset: `System` and `Firmware` work over any transport, except uploads (HTTP only).
- **Fleet operations** are writes through the gateway to each peer, or to all of them with a `*` (`/peers/*/sequencer/play`, TesserAPI's fan-out, §14): update every peer's firmware (§5.4), set a value on all of them, trigger a sequence (§5.13). A view (TesserUI §8) with `each("/peers", ...)` shows a fleet dashboard.
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
| `NotificationService::pushNotification` | an event with `ui.toast` and `ui.hidden` (§8, TesserUI §7.8) |
| `AnalyticsService` | watched values with `ui.widget "chart"` |
| Svelte pages per service, menu entries | none for services; the application's tree, views and custom elements |
| `WWWData.h` (one handler per asset, `immutable` caching) | TesserUI's bundle: two handlers, ETag revalidation, so a firmware update never serves a stale UI |
| `addLoopFunction()` | the application's own `loop()` or task; `dev.onTick(fn)` runs in TesserKIT's task |

### 10.1 helionext (open-heliostat)

The heliostat is ported in its own repository, [tiny-heliostat](https://github.com/leokeba/tiny-heliostat), whose `docs/DESIGN.md` is the spec of the port. In short:

- Classic ESP32 DevKitC, ESP-IDF only. Two DC servo axes with magnetic encoders, GPS.
- Motors, encoders, axes, sequencers and the GPS come from [TesserMotion](https://github.com/leokeba/TesserMotion), a sibling library on TesserAPI, so devices without motors don't carry them. `NmeaGps` feeds `clock.external()` (§5.3) through the application's `onTick`.
- `/heliostat` holds the sun position, the modes, and the named directions: one keyed list `directions` (helionext's `targetsMap`), with `source` and `target` naming entries and a `mode` enum.
- Art-Net is received into `/heliostat/artnet` (`azimuth`, `elevation`); the heliostat decides what the values mean. TesserKIT's ArtNet service (§5.9) writes them, mapped from DMX channels.
- The calibration routines, the mount orientation, autotune, several heliostats and settings migration are dropped for now.
- TesserKIT provides `/sys`: Device, System, Network, Time, Firmware (upload), Backup, Timers, Auth; the interface uses `dev.web().asset()` for the custom elements `x-sky` and `x-keyframes`, and TesserUI's `Views`.

### 10.2 spin-seq (spin-sequencer)

spin-seq (ESP32-S3 XIAO, TMC5160 stepper, magnetic encoder, master and remotes) ports with a different multi-node architecture:

| Today | TesserKIT |
|---|---|
| `SpinSequencerService` `/rest/spin-seq` + UDP JSON router (port 9600) | `/controller`, `/sequencer`, `/io`, `/welcome`; the UDP router is replaced by TesserAPI over NowTP (and a UDP datagram transport if older installations must keep working, §14) |
| `JsonSeq`: an array of free-form JSON commands (`p` parameters, `c` motor command, `d` duration, `n` next, `t` trigger, `b` broadcast, `w` remote writes, `id` deduplication) | the Sequencer service (§5.13): steps that are writes to the tree (`/controller` values and actions), `wait` and `ms` for `d`, `next` by name, index or offset, `wait: none` for `t`, writes to `/peers` for `b` and `w`; NowTP's reliable delivery replaces `id` |
| `bootCommandIndex`, `isRunning` | `/sequencer/boot` *(p)*; a sequence isn't resumed at boot unless `boot` says so |
| A single command run for `d` ms, then the motor stopped | `execute`, with `finally` stopping the motor |
| Remotes list from mDNS, `ESPNowService`, `UdpSenderService` | the master is a UI host: NowTP discovery mounts the remotes under `/peers`; the console and stats come from NowTP's diagnostics |
| Master page fanning out over HTTP from the browser | a view with `each("/peers", "$/sequencer/status")` and actions on the master that write to every peer |
| Fleet OTA via GitHub | `/sys/firmware` on each peer, driven through the gateway (§5.4) |
| `JsonTimer` daily timers | `/sys/timers` with `path` `/sequencer/play` |
| Config export and import | `/sys/backup` |
| Front-panel button and LED | the application's own; `dev.onState()` for the LED's network states |

Commands broadcast to every remote (`b`) become a master's step writing to `/peers/*/sequencer/play`, and later a scheduled start with NowTP's shared clock (§9).

## 11. Footprint

Budgets, measured on a classic ESP32 (ESP-IDF, same method as TesserAPI §15: an example against a baseline build) and enforced in CI:

| Item | Budget |
|---|---|
| Flash: Device + System + Network + Time + Firmware + Backup, above TesserAPI + TesserUI | ≤ 60 KB (mDNS and the DNS server included; ESP-IDF's Wi-Fi stack excluded, it is in every baseline) |
| Flash: the UI | TesserUI's bundle, ≤ 50 KB |
| Flash: an application with every first-release service, Wi-Fi and HTTP | ≤ 1.25 MB, to fit Arduino's default partition table |
| RAM at rest, above Wi-Fi and `esp_http_server` | ≤ 12 KB, the task's stack included |
| Stack | TesserKIT's task: 6 KB, the HTTP server's 6 KB (Mqtt adds esp-mqtt's task, 6 KB, and its publishing task, 4 KB) |

**Measured** (2026-10-10, ESP-IDF 6.1-dev, ESP32, `-Os`): `examples/idf/minimal` against its `-DFOOTPRINT_BASELINE=1` build, the same application on TesserAPI, TesserUI and a plain Wi-Fi station.

| Build | Above the baseline |
|---|---|
| Device + System + Network + Time + Firmware + Backup | 113.3 KB |
| The same with `CONFIG_TESSERKIT_MDNS=n` (`TESSERKIT_NO_MDNS`) | 77.8 KB |
| `fw.downloads()` adds TLS, the certificate bundle and the HTTP client | about 130 KB |
| `fw.github()` adds the release feed above `downloads()` (2026-10-11, `-DWITH_GITHUB=1` against `-DWITH_DOWNLOADS=1`) | 9.6 KB |
| ArtNet (2026-10-11, ESP-IDF 6.2-dev, `-DWITH_ARTNET=1` against the example without it) | 9.2 KB |
| Mqtt (the same, `-DWITH_MQTT=1`): TesserKIT's bridge 14.5 KB, esp-mqtt 12.7 KB, TCP transports, esp-tls and the TLS crypto they link, stdio | 110 KB |
| Mqtt with `CONFIG_MQTT_TRANSPORT_SSL=n` and `CONFIG_MQTT_TRANSPORT_WEBSOCKET=n` (mqtt:// only) | 102 KB |
| Files with the stored backups (2026-10-12, `examples/idf/full` with and without `Files`): LittleFS, the VFS directory calls, the service | 40.9 KB |
| Ethernet (2026-10-12, `examples/idf/full` with and without `-DETHERNET_BOARD=gledopto`): `esp_eth`'s MAC and generic PHY drivers, its netif glue, the GPIO driver | 40.5 KB |

- **Where it goes** (with mDNS): TesserKIT's own code 42 KB (Network 15 KB, most of it the `std::function`s behind bound values and the keyed list), ESP-IDF's `mdns` 34 KB, libc's time zone support 17 KB (`tzset()` parses with `vfscanf`), lwIP's DHCP server, DNS client and sockets 7.6 KB, the access point in the Wi-Fi driver 6 KB, OTA 2.6 KB.
- **Against the budget:** 60 KB is exceeded. Features stay, as options where they are large (mDNS, downloads); the rest is smaller only at the cost of clarity, and is left as it is until a measured application needs the space.
- **Arduino:** the Minimal sketch with every first-release service is 1.18 MB on ESP32 and ESP32-C3, within the default table's 1.25 MB slot (1.21 MB on ESP32 as of 2026-10-11, with that day's TesserAPI and TesserUI; 1.24 MB with ArtNet). With Mqtt it is 1.36 MB on ESP32: Arduino's prebuilt esp-mqtt can't drop its transports, so such a sketch needs a larger app slot (`min_spiffs.csv`, or TesserKIT's tables). As of 2026-10-12 (TesserAPI 0a44fdf, TesserUI 612d892) the Minimal sketch is 1.31 MB, 99.9% of the slot; with Files it is 1.38 MB, so a sketch with Files needs `min_spiffs.csv` (whose 128 KB `spiffs` partition Files then uses) or `tesserkit_4mb_files.csv`.

**Files on hardware** (2026-10-12, DEVKIT-V3, `examples/idf/full`): uploads of 100 KB at about 40 KB/s (LittleFS writes; firmware uploads, which erase up front, go at 70 KB/s), downloads at about 110 KB/s.

**RAM with everything** (2026-10-11, `examples/idf/full` on a classic ESP32 without PSRAM, ESP-IDF 6.1-dev): 46 KB of heap free with Wi-Fi, HTTP, Bluetooth and NowTP running, 29 KB with an MQTT session too. Stacks used, measured with `vTaskList`: the HTTP server 3 KB, TesserKIT's task 2.4 KB, NowTP's transport 2.8 KB, esp-mqtt 1.5 KB. NowTP takes about 25 KB while running, an MQTT session 15 KB, and an HTTPS connection about 34 KB at its peak (a GitHub release check), its 8 KB task included.

- **HTTPS waits for memory:** a release check or an HTTPS download doesn't start below 44 KB of free heap (`not enough free memory for HTTPS now`), since a handshake would fail, or take the memory the rest of the firmware needs: such a device aborted on an allocation in TesserKIT's task. On a classic ESP32 without PSRAM, every service at once leaves no room for HTTPS while MQTT is connected; the ESP32-S3, or a board with PSRAM, has plenty.
- **What the full example gives up for it:** mbedTLS allocates its buffers as it needs them and frees the certificates once verified, six Wi-Fi receive buffers rather than ten (a little less throughput under load), and NimBLE configured as a one-client peripheral (§5.10). The HTTP server's stack is 6 KB rather than 8 (3 KB used).
- **Flash:** `examples/idf/full` is 1.53 MB on ESP32 with Files (2026-10-12), in `tesserkit_4mb_files.csv`'s 1.75 MB slot.

**Partition table** (`partitions/tesserkit_4mb.csv`, recommended): `nvs` 20 KB, `otadata`, `ota_0` and `ota_1` 1.94 MB each, `coredump` 64 KB. No filesystem, no `phy_init` (the PHY data is in the app by default). `tesserkit_8mb.csv` has 3.94 MB slots. With Files: `tesserkit_4mb_files.csv` has 1.75 MB slots and a 384 KB `files` partition (LittleFS), `tesserkit_8mb_files.csv` 3 MB slots and 1.94 MB of files. Both keep `nvs` where the other tables have it, so the settings survive a change of table (flashed over serial: an OTA update can't change the table).

**Comparison:** the same measurements are taken on ESP32-sveltekit's demo with matching features (Wi-Fi, AP, NTP, upload OTA, status), and reported in the README.

## 12. Testing

1. **Host tests** (CMake, ASan and UBSan, the same warnings as TesserAPI): every service against the fake platform. Wi-Fi state machine (join, lose, fall back to the AP, recovery), Ethernet (preferred to the station, the station's wait for it, recovery on a cable, a static address), time and DST, timers (catch-up, no double runs), firmware upload checks, release checks (versions, GitHub's answers and failures, scheduling), backup round trips, tokens (expiry, revocation, brute-force delay), the MQTT bridge (topics, commands with User access, sessions, intervals), Art-Net (scaling, gates, changes only, ArtPollReply's bytes), Bluetooth (the name following the hostname, switching off and on, the client shown), NowTp (waiting for the network, the nodes listed and read-only, pairing, the factory reset forgetting the key), HTTPS refused without the memory for it, Files (names and paths refused, uploads replaced whole and cut short, directories, space, formatting, the application's own writes), the stored backups (names, `keep`, `before-restore`, the factory reset) and the Sequencer (each wait, names, indices and offsets, finally, take-overs, loops, failures, boot). The fake platform is a CMake target, `tesserkit_fake` (`fake_platform.h`, `fake::Platform::install(dev)`), so applications' host tests and simulators show `/sys` too.
2. **Golden schemas:** each service's schema with its hints, checked in as `fixtures/*.json`. TesserUI renders them in its golden-model tests, so a hint change shows as a diff in both repositories.
3. **On target:** `examples/idf/minimal` (and `examples/arduino/Minimal`, with `--app /lamp`), driven over HTTP and WebSocket by `test/e2e_device.py`: status, the interface, subscriptions, a scan, SNTP, sign-in with tokens over HTTP and WebSocket, a timer running, persistence across a restart, backup and restore, refused images, an OTA upload, its confirmation and rollback, and an image that crashes at boot (`-DCRASH_AT_BOOT=1`) rolled back by the bootloader, with its core dump downloaded. Provisioning: `test/captive_probe` is a firmware that plays a phone on a second board. It joins a fresh device's access point (built with `-DNO_DEFAULT_NETWORK=1`), checks that any name resolves to the device and that Android's and Apple's connectivity probes are redirected to the interface, then saves a network through the API and watches the device join it and the access point go. Releases: `test/e2e_github.py` serves a release endpoint and a newer image from the test machine, and watches the device find it, install it and come back up to date; `--check-only` checks the device's own feed (GitHub's API). A real GitHub download (2026-10-11, DEVKIT-V3: v0.1.2's asset through its ~950-character signed CDN redirect, 1.5 MB in 25 s, installed and booted): the mock server's short URLs had hidden that the HTTP client's request buffer was too small for it. Bluetooth: `examples/idf/bluetooth`, driven from a Mac with `bleak` (Nordic UART): an open device is Admin, a locked one public, sign-in with a line token gives Admin, a bad token public, and a reconnection starts public again. `examples/idf/full` (2026-10-11, DEVKIT-V3 beside DEVKIT-R1 running TesserAPI's test firmware): every service up, R1 found over NowTP and mounted under `/peers`, the envelopes on the serial line among the logs, a GitHub release check over HTTPS beside Bluetooth and NowTP, and with an MQTT session too (when it ran out of memory, which the guard of §11 now refuses). Files: `test/e2e_files.py` on `examples/idf/full` (2026-10-12, DEVKIT-V3, 39 checks): directories, a 100 KB upload and download compared byte for byte, a name with a space and accents (`filename*=UTF-8''`), refused names and sizes, replace, rename and delete, and stored backups saved, downloaded, restored and deleted. Two transfers on one path can't overlap over HTTP there: ESP-IDF's server handles one request at a time, so the second waits for the first (TesserAPI's host tests cover `busy`). *(later)* a test firmware with every service and serial scripts.
4. **CI:** each push to main runs the host tests, then releases the example and publishes the launcher (§13). *(later)* ESP-IDF builds (5.1, 5.4, 5.5, latest × ESP32, ESP32-C3, ESP32-S3), Arduino-ESP32 3.x builds of the examples, the footprint budgets.

## 13. Packaging

```
TesserKIT/
  docs/DESIGN.md
  src/TesserKIT.h
  src/tesserkit/            device, services
  src/tesserkit/platform/   ESP implementation of the platform layer
  partitions/               tesserkit_4mb.csv, tesserkit_8mb.csv, and *_files.csv with a LittleFS partition
  examples/arduino/         Minimal, Bluetooth
  examples/idf/             minimal, bluetooth, full (every service and transport)
  test/                     host tests, fake platform, test firmware
  CMakeLists.txt, idf_component.yml, library.json, library.properties
```

- ESP-IDF component and PlatformIO/Arduino library, like TesserAPI and TesserUI. Dependencies: TesserAPI and TesserUI as public requirements; `espressif/mdns`, `espressif/mqtt` from ESP-IDF 6 (part of ESP-IDF before), and `joltwallet/littlefs`, linked only with Files; NowTP detected as TesserAPI does (`__has_include` or the build's component list).
- Versions follow semver; TesserKIT pins compatible TesserAPI and TesserUI ranges.
- **Releases:** `.github/workflows/firmware.yml` is a reusable workflow for ESP-IDF projects. It computes the next version, builds each target in `espressif/idf`'s container with `-DPROJECT_VER=<version>` (which becomes `/sys/firmware/version`), and publishes a release tagged `v<version>` with notes generated from the commits and, per target, `<name>-<target>.bin` (what `github()` installs) and `<name>-<target>-factory.bin` (bootloader, partition table and app merged, flashed at 0x0 on a new board). The version is the last `vX.Y.Z` tag with its patch raised, or what `version-command` prints when that is higher: a minor or major release is a commit raising that version (`library.json` here). A rerun on a released commit keeps its version. `dependencies` checks out repositories next to the project, where a developer's siblings are. Several firmwares go into one release with `projects`, a JSON list of `{project, name, build-args}`, instead of `project`, `name` and `build-args`. TesserKIT's `release.yml` calls it on each push to main, after the host tests, for `examples/idf/minimal` with `-DWITH_GITHUB=1` (`tesserkit-minimal-*`) and `examples/idf/full` (`tesserkit-full-*`, NowTP checked out beside) on ESP32, ESP32-C3 and ESP32-S3; applications call it for their own firmware (`uses: leokeba/TesserKIT/.github/workflows/firmware.yml@main`). `[skip ci]` in a commit message skips the release.
- **Launcher:** `release.yml` also publishes the launcher (§8) to GitHub Pages after the host tests: TesserUI's bundle, built from TesserUI's main as the firmware's is, with `pages/index.html`, which makes it a launcher titled TesserKIT (`window.TESSER = {launcher: true, title: "TesserKIT"}`).

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

Done 2026-10-12, for Files (TesserAPI 0a44fdf): file nodes in list elements transfer (`/sys/files/entries/<name>/content`), and an upload carries the client's file name (`?filename=`, `FileRequest::filename`).

Done 2026-10-12, for the Sequencer (§5.13), in TesserAPI 9c07af2 (its §6.5): a `*` path segment in set, matching every child of an object or element of a list or array, the set applied to each match (remote nodes forwarded in parallel, offline ones failing at once, matches without the rest of the path skipped), and a reply with each match's result, or `"<n> of <total> failed: <name>: <message>"` and each failure.

Done as of 2026-10-11 too, for Bluetooth and NowTp: an `auth` envelope presenting a token on a line, an unauthenticated line or untrusted NowTP peer taking the level of the empty token, `BleTransport` controlled at runtime (`start()`, `stop()`, `setName()`, `peerAddress()`), read-only lists.

**NowTP** (done in NowTP d39e0d4, 2026-10-11; NowTp uses the first three):

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
| File nodes in list rows (download and upload buttons), `?filename=` on uploads, a `folder` icon (TesserUI 612d892, 2026-10-12) | §5.12, §5.5 |
| Toasts from events on any page, and hidden events as toasts only, through an events-only subscription to `/` (TesserUI 0b096d9, TesserAPI 297615d `changes: false`, 2026-10-12) | §8 |

## 15. Roadmap

| Phase | Content | Acceptance |
|---|---|---|
| 0 | TesserAPI follow-ups of §14 | Host-tested; the HTTP and WebSocket parts checked on hardware. |
| 1 *(done)* | Device, System, Network, Time, Firmware (upload, download, rollback), Backup; the TesserUI widgets they need | A fresh classic ESP32 is provisioned from a phone through the captive portal, updated from the browser, rolled back after a crashing image, backed up and restored. Footprint within §11, compared with ESP32-sveltekit. |
| 2 *(done)* | Timers, Auth | spin-seq's timers and a locked-down device. |
| 3 | helionext on TesserKIT, in tiny-heliostat (§10.1) | Every helionext page usable, with custom elements only for the keyframes and the sky. |
| 4 | spin-seq on TesserKIT, NowTP channel following and pairing, shared clock | A master and three remotes run a synchronized sequence; fleet update through the master. |
| 5 | Battery, Sleep, Ethernet (RMII *(done)*), Log, Home Assistant discovery | *(later)* |

Mqtt and ArtNet were built ahead of the roadmap (2026-10-11), ArtNet for tiny-heliostat, and so were GitHub releases with the workflow that publishes them, and the Bluetooth and NowTp services with `examples/idf/full` (2026-10-11): phase 4's NowTP part, short of the synchronized sequence. Ethernet on the internal MAC came on 2026-10-12, for a Gledopto LED controller.

## 16. Decisions

- **A library rather than a template:** ESP32-sveltekit's forks (helionext, spin-seq) carry the framework's code and diverge from upstream. A library is updated by version.
- **Services under `/sys`, generic UI:** the alternative, a hand-written page per service, is what makes ESP32-sveltekit's interface 240 KB and every service cost a page.
- **NVS, no filesystem for settings:** the persisted state is small (a few KB), NVS levels wear, and the flash a filesystem partition takes goes to the app slots instead. Files (§5.12) is for what isn't settings, and it is optional, with its own partition tables.
- **LittleFS for Files:** it survives power cuts and levels wear, and has directories. SPIFFS has no directories and is no longer developed; FAT with wear levelling isn't safe against power cuts.
- **Stored backups go with a factory reset:** they hold the Wi-Fi passwords and accounts a reset is there to erase (a device given away). Recovering from an accidental reset takes a downloaded backup.
- **The platform layer:** the services' logic (state machines, validation) is where bugs live, and it is host-testable only if ESP calls are behind an interface. ESP32-sveltekit has no tests.
- **Stateless signed tokens** rather than sessions in RAM: logins survive restarts and OTA updates, and nothing grows per client. Revocation is all-or-nothing (`logoutAll`), acceptable for devices with a handful of users.
- **Versions from tags, not from a file CI commits:** a release's version is derived from the last tag, so main never moves under a developer's feet and no bot commit triggers another run; the manifests' version only marks minor and major steps.
- **Firmware updates checked on the device, not in the browser:** the browser may have no internet (a field on the device's access point), and the device is the one that knows its chip and slot.
- **MQTT commands with User access:** a broker is shared, and its clients aren't the device's admins. They drive the application; `/sys` (Wi-Fi, firmware, accounts) stays with whoever signs in to the device.
- **`/set` filters by depth, not `#`:** `<prefix>/#` would bring every retained topic the device publishes back to it. Eight filters cost the broker nothing and bound the paths a command reaches, which writable application nodes don't exceed.
- **Art-Net through the API, on change:** writes go through the same validation, access checks and notifications as a client's, so a console can't set what a browser couldn't, and a still console costs nothing. Meaning (takeover, which mode) stays in the application's node.
- **Paired NowTP nodes are trusted, not listed MACs:** ESP-NOW frames carry no proof of origin, so a list of trusted MACs is spoofable; NowTP's installation key authenticates every message, and only a node holding it can reach a secured device at all. The key proves membership of the installation, not which node spoke, which is the trust a field of nodes needs.
- **Sequencer steps are writes, not commands:** spin-seq's sequencer had its own JSON language, a verb per thing it could do (motor commands, broadcasts, writing commands, rebooting). Writes to the tree already reach all of that, validated and access-checked, on this node and on the nodes under `/peers`, so the sequencer drives any device without knowing it. A deferred action's reply stands for "until it has finished".
- **HTTPS refused below a heap floor:** a handshake that runs the heap out doesn't fail alone, it can abort whatever task allocates next. Refusing it with a message, and trying again at the next check, keeps the device up; the floor comes from a measurement (§11), not a guess.
