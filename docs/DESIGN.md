# TesserKIT design

Status: draft, written before any code. Everything here is the specification the code will follow; when the two disagree, fix one of them. Sections marked *(later)* are committed to but not part of the first release. TesserAPI and TesserUI references point to their own DESIGN.md: [TesserAPI](https://github.com/leokeba/TesserAPI/blob/main/docs/DESIGN.md), [TesserUI](https://github.com/leokeba/TesserUI/blob/main/docs/DESIGN.md).

## 1. Goals

TesserKIT is the device framework built on [TesserAPI](https://github.com/leokeba/TesserAPI) (the tree and its transports), [TesserUI](https://github.com/leokeba/TesserUI) (the interface rendered from the tree's schema) and, optionally, [NowTP](https://github.com/leokeba/NowTP) (ESP-NOW). It provides what every connected device needs besides its own function: network setup, time, firmware updates, backups, users, MQTT, system status. It aims at most of [ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit)'s functionality at a fraction of its footprint.

- **A library, not a template.** Applications depend on TesserKIT; they don't fork it. Upstream fixes arrive by updating a version, not by merging.
- **Every service is a subtree.** A service describes itself into the TesserAPI tree under `/sys`, with values, actions, events, lists and files. It is then reachable over every TesserAPI transport (HTTP, WebSocket, serial, NowTP), through gateways, and from MQTT, with no endpoint code of its own.
- **Zero UI code.** Services carry TesserUI hints (labels, units, widgets, confirmations, layout); the generic renderer draws their pages. No service has a hand-written page, and neither does an application unless it wants one (custom elements, TesserUI §9.8).
- **Pay for what you use.** A service that isn't instantiated isn't linked, nor a large option whose builder isn't called. The few parts a service would link anyway are options of TesserKIT's component (Kconfig, or `-D` on Arduino): one scheme, described in §11 (Profiles).
- **Small.** The whole interface is TesserUI's bundle (≤ 50 KB gzipped, against ESP32-sveltekit's ~240 KB). Settings live in NVS, or on the filesystem of the optional Files service (§5.12) when there is one (§6.1): no partition is needed for them. Measured budgets in §11.
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
                            └─▶ NVS, or Files' LittleFS (one record per top-level node)
```

### 3.1 Device

`tesserkit::Device` owns what every node has: the `tesser::Api`, the HTTP server with WebSocket, the TesserUI page and bundle, persistence (NVS, or the Files service's filesystem, §6.1), and TesserKIT's task. Services are separate objects, given the device:

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
- **Options** (before `begin()`): `dev.hostname("helio")`, `dev.version("1.4.0")`, `dev.ui(false)` (headless: no page or bundle), `dev.httpPort(80)`, `dev.cors("*")`, `dev.task(false)` (the application calls `dev.loop()` itself), `dev.storage(storage)` (instead of NVS or Files' filesystem).
- **Accessors:** `dev.api()`, `dev.http()`, `dev.web()` (TesserUI's `WebUi`, for its options), `dev.sys()` (the `/sys` object, for an application's own system-level nodes).

### 3.2 The `/sys` tree

```
/sys                    label "System", ui.page, ui.icon "settings", a description
  name                  System (§5.1): the device's name (§4.4)
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
  log/                  Log (§5.14)
  battery/              Battery (§5.15)
  sleep/                Sleep (§5.16)
  udp/                  UdpApi (§5.18)
/peers                  the nodes NowTp finds nearby, mounted beside the application's tree
/sequencer              Sequencer (§5.13): the application's, so at the root
```

- The root stays the application's: `/sys` is one child of it, rendered by TesserUI as a link to its own page, where each service is a card.
- Service subtrees are stable API: other devices, scripts and MQTT topics address them by path. Renaming a node is a breaking change, listed in the changelog.
- Every service describes itself through a public `describe(tesser::Object&)`, so an application that wants a different layout (no `/sys`, or a service under its own object) mounts it itself: `api.mount("wifi", net)` and `dev.begin(tesserkit::NoSys)`.

### 3.3 Platform layer

Services never call ESP-IDF directly. They call `tesserkit::Platform`, a set of narrow interfaces: `Core` (chip information, heap, restart, tasks), `Wifi` (station, access point, captive portal), `Ethernet` (the internal MAC and its PHY, or a W5500 on SPI), `IpStack` (the IP stack's one list of DNS servers, which Network refills), `Mdns`, `Clock` (the clock, the time zone, SNTP), `Ota` (the OTA slots), `HttpClient` (downloads and release checks), `Coredump`, `Udp` (Art-Net and sACN: sockets and a receiving task), `UdpGroups` (sACN's multicast groups, apart so that a build without sACN doesn't link IGMP), `MqttClient`, `Ble` (Bluetooth LE), `NowTpRadio` (ESP-NOW), `Filesystem` (LittleFS: mounting, usage, formatting; the files themselves through stdio and `dirent`, the same on the host), `LogOutput` (the log's lines and level), `Power` (deep sleep and its wake-up), `Pins` (ADC and digital inputs, pulled up, down or not), `UdpApiTransport` (TesserAPI's UDP transport) and, for applications rather than services, `Pwm` (the LEDC, §5.20):

- `src/tesserkit/platform/esp/` implements them on ESP-IDF APIs, which Arduino-ESP32 3.x also ships, so one implementation serves both.
- `test/fake_platform.*` implements them in memory for host tests: a Wi-Fi radio whose scans and connections the test scripts, a clock the test advances, an OTA partition that's a byte vector.
- **Services also hear of a factory reset** (`Service::factoryReset()`), before the restart, to erase what they keep outside the device's storage: NowTp's installation key. It runs in the device's task under the API lock, like `tick()`, whichever task asked for the reset.
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
2. Opens the settings' storage (one record per top-level node, §6.1): with a Files service whose partition mounts, it is mounted now, and the settings are files in its `/.settings`, moved there from NVS at the first such boot; else NVS (namespace `tesserkit`). It loads the persisted state (TesserAPI §12), migrated first if it was stored under older settings versions (§6.3). An NVS partition NVS can't use (`no free pages` or `new version found`, as a firmware with another partition table leaves it) is erased and initialized again: nothing in it could be read, and Wi-Fi can't start without NVS. Values that don't fit any more are skipped with a warning: a firmware update never breaks a boot.
3. Applies factory defaults (§6.2) to whatever the stored state didn't set.
4. Starts each service's platform side: Network brings up Wi-Fi, Time starts SNTP, Firmware checks the rollback state.
5. Starts the HTTP server: the API at `/api`, WebSocket at `/ws`, the UI at `/`, `/ui.js` and its widgets' chunk under `/ui/` (TesserUI §9.4), the captive portal's error handler (§5.2). The server's URI handler budget is computed from what's used, never set by hand: the API's, the UI's, plus `dev.extraHandlers(n)` (default 4) for the application's own routes and TesserUI assets.
6. Starts TesserKIT's task.

### 4.2 Task

One FreeRTOS task (`tesserkit`, 6 KB stack, priority 3; 8 KB and priority 5 with pollers) runs every 20 ms:

- `api.poll()`: change notifications, watched values, debounced persistence
- each service's `tick(nowMs)`: Wi-Fi state machine, timers, MQTT publishing
- the pollers (`dev.poller(fn)`, without the API lock): the transports that have no worker task of their own, NowTp's (§5.11, NowTP's radio included) and UdpApi's (§5.18) by default

**Transports in the device's task:** a transport's worker runs the requests that arrive by it, and each worker's stack is heap on a chip without PSRAM. NowTp and UdpApi therefore have none by default: their messages are handled by the pollers, and `dev.wake()`, called from the transport as a message is queued, runs the pollers at once rather than at the next period. The task's stack is then 8 KB, since requests run in it as in the HTTP task (up to 5 KB). Measured on DEVKIT-V3 and DEVKIT-R1 with `examples/idf/full` (2026-10-13, three runs of 40 reads each way): about 11 KB more free heap on each board (18.8 → 29.5 KB, the lowest after two minutes 10.2 → 21.1 KB), NowTP's round trip unchanged (19–22 ms), UDP's about 10 ms longer (69–84 → 82–88 ms, through a Wi-Fi access point that holds it near 100 ms anyway). `espNow.ownTask(true)` and `udp.ownTask(true)` bring the workers back.

**NowTP's own task too** (2026-10-22, NowTP e980851's polled mode): NowTP's radio events and timers (sends, acknowledgements, reassembly, discovery, time sync) run in the same pollers, before TesserAPI's transport, and NowTP's wake hook calls `dev.wake()` when there is new work; NowTP never needs a poll more than 250 ms apart, and the task polls every 20 ms. With pollers the task runs at priority 5, the priority NowTP's task and the HTTP server's have: at 3, a relay through a gateway waited behind the HTTP task feeding it, and NowTP's next frame and acknowledgements waited for the task. Measured with `test/hil.py` (`classic` profile, Gledopto C as gateway on Ethernet, DEVKIT-R1 as its peer over NowTP): about 3–4 KB more free heap on both (the task's 4 KB stack), the gateway's lowest 51 KB rather than 43 KB; a relayed firmware update in 49 s (with NowTP's task 48 s, polled at priority 3 80 s), relayed file uploads and downloads 15.7 and 33.0 KB/s (17.3 and 39.8 with the task, on 20 KB files). `espNow.ownTask(true)` brings NowTP's task back with the worker.

Platform callbacks (Wi-Fi events, SNTP sync) only post to a small queue that the task drains, so services never run in the Wi-Fi task and always hold the API lock while they touch the tree. **One time per round:** the task reads the clock once, handles the queued events, then calls every `tick(nowMs)` with that time, and a service's `event()` takes its time from the same reading (`dev.loopMs()`), never from the clock. An event is handled some milliseconds after the round began (a busy gateway's events, a slow one before it), so a time read from the clock then would be ahead of the tick that follows, and `nowMs - then` would wrap: until 2026-10-21 every Wi-Fi attempt of a Gledopto acting as a peer timed out "no address (DHCP)" as it associated, and the access point's fallback could come up at once. With `dev.task(false)` the application calls `dev.loop()` from its own loop instead.

**The event queue is bounded** (32 events), and fills when a burst of events meets a long stall of the task: an OTA chunk whose `esp_ota_end` takes 1.3–1.5 s, a sign-in's PBKDF2 (about 280 ms), while a broker sends its retained commands or peers flap. So no event is precious: MQTT's messages have at most 8 places, the oldest giving way to a newer one and to any other event when the queue is full (counted in Mqtt's `status/missed`, §5.8); `PeersChanged` and `NowTpPeers` say only "look again", so one of each waits at most; and a service that keeps state reads it back from the platform rather than trusting edges alone (Network's link and addresses, §5.2). Drops are logged by the task, with their count.

### 4.3 State

`dev.state()` and `dev.onState(fn)` give the device's overall state, for a status LED or a display: `booting`, `provisioning` (access point up, no network configured), `connecting`, `online`, `updating`, `error`. It is also a read-only value, `/sys/status/state`.

### 4.4 Identity

- **Label:** `Device("Heliostat")` is the firmware's name for the device: its name until the user gives it one, and its hostname's start.
- **Name:** `/sys/name` (§5.1), the user's name for this device ("Heliostat 3", "Lampe d'entrée"); empty, the label. It is the root's label (`api.label()`), which TesserUI shows as the node's name: the header, and the node's tab in a gateway's or the launcher's tab bar (TesserUI §6.5), where every node of one firmware would otherwise read the same. A change applies at once: the label is part of the schema's hash, which the node advertises over NowTP and UDP, so a gateway's UI reads the new name. Art-Net's short name and Home Assistant's device name are the name too. `dev.name()` reads it, `dev.nameSetting()` the setting. Its `ui.nodeName` hint has TesserUI read the root's label again after writing it (the hash already new when the setter returns) and leave it out of apply to all (TesserUI §6.6), which would otherwise give every node one name and one hostname.
- **Hostname:** `/sys/network/hostname` when set; else the name the user gave, made a hostname (lowercased, Latin letters' accents dropped, other characters as single hyphens, at most 32: "Lampe d'entrée" → `lampe-d-entree`); else `dev.hostname("helio")`, the application's; else the label made one (at most 26), plus the last two MAC bytes (`heliostat-a67c`), so devices of one firmware differ until named. It is the DHCP hostname, the mDNS name, the NowTP and UDP discovery name (their nodes' names under a gateway's `/peers`; a new one from the next restart), the Bluetooth name and the files' download names. A name with neither letters nor digits ("★") leaves the hostname to the next rule. Two devices given the same name get the same hostname: give one a hostname of its own.
- **Version:** `dev.version("1.4.0")`, else the ESP-IDF app description's version. Shown in `/sys/firmware/version`, advertised over mDNS.

## 5. Services

Each service lists its tree, its behaviour and its access levels (§7). "Admin" means the node's `writeAccess(Admin)`; reads are public unless stated. Persisted nodes are marked *(p)*. Settings are checked with TesserAPI validators (`.validate()`), before anything is applied, so a refused patch, form or list write changes nothing, and a boot load or a restore skips what no longer passes.

### 5.1 System

```
/sys/name *(p)      string ≤ 32, Admin, ui.placeholder the label, ui.nodeName, no control characters: the device's name (§4.4); empty, the label
/sys/status
  state        enum: booting, provisioning, connecting, online, updating, error
  uptime       integer, s, watched
  heap         integer, B, watched, ui.chart        minHeap  integer, B: internal RAM, PSRAM apart (it is what runs out)
  maxBlock     integer, B, advanced: the largest block one allocation can get (fragmentation)
  settings     string, warning callout while not empty: the settings kept in NVS although the application has Files, and why (§6.1); a settings record too large for NVS (§11)
  psram        integer, B (only with PSRAM)        minPsram  integer, B, advanced
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
- `factoryReset` answers first too: `Device::factoryReset()` only asks for it (thread-safe), and the device's task, at its next loop, erases the storage, calls each service's `factoryReset()`, stops saving (the running state would be written back), and restarts 500 ms later. So the services' erasures (NowTP's key, Files' backups) never run in a transport's task beside NowTP's own or the API's saves. An erase the storage refuses is logged; the reply can no longer say so.
- The firmware's log lines are the Log service's (§5.14).

### 5.2 Network

```
/sys/network
  hostname *(p)        string ≤ 32, Admin, ui.placeholder "from the name": empty, from the device's name (§4.4)
  host                 string, read-only, watched, advanced: the hostname in use, the setting or the one made from the name (<host>.local)
  ethernet/            with a board (below)
    enabled *(p)       boolean (default true), Admin: off stops the MAC and the PHY
    status/            state (in words), link, connected, ip, speed (Mbps), mac
    static/ *(p)       { enabled, ip, mask, gateway, dns } ui.form, ui.collapsed, Admin
    joined             event, ok toast: "192.168.1.60 on Ethernet"
  wifi/
    mode *(p)          enum, Admin: the station always, never, or as a fallback to Ethernet (with a board,
                         the default); without a board always (default) or never
    powerSave *(p)     enum none, min, max (default min, ESP-IDF's; none with NowTp), Admin, advanced
    networks *(p)      keyed list (key "ssid"), ≤ 5, Admin, readAccess User:
                         ssid ≤ 32, password (secret) ≤ 64, bssid (advanced: this access point only; empty: any),
                         static/ { enabled, ip, mask, gateway, dns } ui.collapsed;
                         elements are ui.form (saved whole, with Apply); ui.suggest "scan" (a scan's networks
                         are offered as new elements)
    scan               deferred action, Admin → [{ssid, rssi, channel, secure, accessPoints: [{bssid, rssi, channel}]}]
    status/            state (in words), connected, ssid, ip, rssi (watched, dBm), channel, bssid
    joined             event, ok toast: "192.168.1.50 on Workshop", the address clients on the AP must follow
    failed             event, warn toast: "Couldn't join "Workshop": wrong password?", once per new reason
  ap/
    mode *(p)          enum: always, fallback, never (default fallback), Admin
    ssid *(p)          string ≤ 32 (default: the hostname), Admin
    password *(p)      secret, empty or ≥ 8 characters, Admin: empty, open, or in secure mode securePassword
    securePassword *(p) read-only, Admin-readable, advanced: the access point's password in secure mode (§7) while password is empty: 12 random characters made once per device, printed on the console when the access point comes up
    channel *(p)       integer 1–13, Admin
    hidden *(p)        boolean, Admin
    status/            active, ip, clients
  mdns *(p)            boolean (default true), Admin
```

**Station:**
- At start and after a disconnect, it scans and joins the strongest saved network in range, then the next ones; when none is in range, it tries every saved network in order (hidden networks don't show in scans). An attempt has 10 s to associate, then 20 s more for its address: DHCP with lwIP's address conflict detection (ESP-IDF's default) takes 5 to 10 s after association. Measured on a Freebox that answers the third request (2026-10-13, a Gledopto GL-C-611WL in ballet-photophonique): associated 2.9 s after the attempt's start, the address 7.3 to 9.8 s later, which the former single 10 s limit cut off every time. An attempt that runs out of either is abandoned ("no answer", "no address (DHCP)"). A round that fails is retried with backoff (1, 2, 5, 10, 30 s); a scan finds networks that came back.
- A write to `networks` reconnects if the current network changed or was removed.
- **Access points:** several access points can share a network's name (a router and its repeater, a mesh), and the station joins the strongest. A network's `bssid` makes it join that one only, when the scan before the attempt heard it, or when the scan heard nothing (a hidden network); otherwise any of them, so a device whose access point is gone stays online. Devices that talk to each other directly should share one: on the home network of 2026-10-13 (a Freebox and a repeater on one name and channel), two devkits on different access points lost every unicast datagram between them, both ways, while broadcasts, the Mac and a wired node still reached both (found by tesserapi-3d over 8 restarts). `scan` lists each network once, strongest first, with its access points (`accessPoints`, strongest first), where a BSSID is read from. Writing the BSSID of the access point the station is on doesn't reconnect. Checked on DEVKIT-V3 and DEVKIT-R1: the scan lists the Freebox and its repeater under one name, a pin moved R1 to the repeater within 12 s and back, and both devkits are pinned to the Freebox since. The loss itself didn't come back that time, with the devkits on different access points: it doesn't happen every time.
- `static.enabled` replaces DHCP for that network (below, "Static addresses").
- **Says why:** `status/state` tells what the station does or why it doesn't: "off", "no network saved", "waiting for Ethernet", "not needed: Ethernet has a DHCP address", "looking for networks", "joining "Home"", "getting an address from "Home"", "connected", or the last failure, `"Home": not found; retrying` (from ESP-IDF's reasons: not found, wrong password?, refused the connection, no answer). A round of attempts that fails for a reason not told yet emits `failed`; joining, or a write to `networks`, makes the next failure news again. Passwords can't be read back, so this is how a wrong one shows. The platform translates its disconnection codes into a `StaReason` (left, not found, wrong password, refused, other), so Network knows none of ESP-IDF's; another reason shows with the platform's code (`failed (reason 3)`).
- `scan` is deferred (TesserAPI §4.2): the reply comes when the scan completes, within TesserAPI's `deferTimeoutMs`. The radio can't scan while it associates, so a scan requested then waits for the association or its failure (at most 10 s), and one requested during the join's own scan shares it.
- `net.txPower(dBm)` caps the transmit power, for boards whose supply browns out at the default 20 dBm.
- **Power save** (`powerSave`): `min`, ESP-IDF's default, sleeps the modem between the access point's beacons, so a packet to the device waits for its next wake-up: measured in ballet-photophonique (2026-10-13), reads forwarded over UDP from an Ethernet node to two devkits on Wi-Fi took 54 and 101 ms longer than direct reads (median). `none` keeps the radio awake, for live control over Wi-Fi and ESP-NOW latency, at the cost of the receiver's current (roughly 100 mA on a classic ESP32); `max` sleeps through several beacons. Applied at once; `net.powerSave(PowerSave::None)` sets the factory default. Beside NowTp the default is `none` (a service that needs the station awake says so, `Service::needsStationAwake()`), since ESP-NOW loses the frames that arrive while the modem sleeps: until NowTP 014c4d5 NowTP forced `WIFI_PS_NONE` over this setting while it ran, and now leaves it to Network, so the user's `min` or `max` holds there too, at that cost. With Bluetooth running, ESP-IDF accepts `none` but Wi-Fi still sleeps outside its coexistence time slices.

**Ethernet:** `net.ethernet(board)` declares the board's wiring: the PHY, its address on the MDIO bus, the MDC and MDIO pins, the pin powering the PHY or its oscillator (driven high before the MAC starts, whose reset needs the clock), and the RMII clock (in on GPIO 0, or out on 0, 16 or 17). Presets in `tesserkit::boards`, from WLED's table: `gledopto()` (Gledopto's controllers with Ethernet: LAN8720 at 1, MDC 23, MDIO 33, power 5, clock in), `wt32Eth01()`, `olimexEsp32Poe()`, `esp32EthernetKit()`; `boards::byName("gledopto", board)` finds one by name (`gledopto`, `wt32-eth01`, `olimex-esp32-poe`, `esp32-ethernet-kit`) for an application reading its board from a file. An application that keeps its board in its own settings, which `Device::begin()` loads, gives `net.ethernet(choose)` instead (2026-10-23, pixled-controller): `choose(EthernetBoard&)` runs when Network starts, after the settings are loaded and before Ethernet starts, and fills the board or returns false for none. The subtree exists either way; without a board its `status/state` is "no board" and Network runs as without Ethernet. The `wifi/mode` choices are Ethernet's (with fallback), which then behaves as always. Without the call, the driver isn't linked; with it, Ethernet adds about 40 KB (§11). On chips without the internal MAC (ESP32-S2, S3, C3, C6...), a WIZnet W5500 module on SPI: `boards::w5500(sck, miso, mosi, cs, irq, reset)` (GPIOs; without `irq` the module is polled every 10 ms, without `reset` it isn't reset by a pin), on SPI2 at 20 MHz, sharing a bus the application started already. The module has no MAC address of its own: it takes the chip's Ethernet address (the base + 3 where the chip has four). Same tree and behaviour: the platform hides which MAC it is. Its driver is ESP-IDF's until 6.0 (`CONFIG_ETH_SPI_ETHERNET_W5500=y`, which Arduino-ESP32 3.x has on), then the `espressif/w5500` component, which TesserKIT's manifest adds on targets without the internal MAC: so on the classic ESP32 and the ESP32-P4 with ESP-IDF 6, a W5500 isn't available, and an RMII board doesn't link it. Where neither applies the subtree is absent and Network runs as without it; a module that doesn't answer leaves `status/state` at "didn't start", with the driver's reason in the log.
- **W5500 on hardware** (2026-10-16, a Seeed XIAO ESP32-S3 wired as spin-sequencer's `seeed_xiao_esp32s3`: SCK 7, MISO 8, MOSI 9, CS 44, INT 43, `examples/idf/full -DETHERNET_W5500="7,8,9,44,43"`): the driver starts and checks the chip, but the module answered every read with zeros (version 0 for 4), as it did for spin-sequencer's own firmware (Arduino-ESP32 3.3.2): it drives MISO when selected and its INT pull-up is powered, so the module is on CS 44 but doesn't see the clock or the data. A wiring fault on that board, not tried further; the W5500 has not carried traffic under TesserKIT yet.
- **The station's mode** (`wifi/mode`): `fallback`, the default with a board, keeps the station away while Ethernet has a DHCP lease (it disconnects if it was joined): one address on the network, and the radio's time for the access point and ESP-NOW. Only a lease counts: a static or link-local address can be a cable to a single computer, which says nothing of the network beyond, so the station joins beside it. Without a cable, or when the link or the lease goes, the station joins as usual, at once. At start, and when the link comes up, it waits 5 s for a lease (time for the link and DHCP; not with a static address); a link without a lease after that lets it join, and leave again when the lease comes. `always`: the station joins whatever Ethernet does (a show network on the cable, and a phone over Wi-Fi). `never`: no station; the access point and ESP-NOW still have the radio.
- **`enabled`** off stops the MAC and the PHY: no link, and the station doesn't wait for one.
- **Link-local:** with no DHCP server on the cable (a cable straight to a computer), lwIP's AutoIP takes an address in 169.254/16 after two unanswered DHCP requests, while DHCP keeps asking; a lease that comes later replaces it (a router that boots after the device). The computer takes one in the same range, and finds the device at `<hostname>.local`. Network finds the address by polling (AutoIP posts no event; below, "Lost events"). It needs `CONFIG_LWIP_AUTOIP=y` in the application's sdkconfig (`examples/idf/full` sets it); Arduino-ESP32's prebuilt lwIP has no AutoIP, so there a cable without DHCP needs a static address.
- **`status/state`** in words: "off", "no cable", "waiting for DHCP", "applying the static address", "connected", "connected, static address", "connected, link-local address (no DHCP server)", "didn't start".
- **The access point** treats Ethernet's address as the station's: `fallback` brings it up 15 s after the device went offline, and down 30 s after Ethernet's address came. **Recovery** waits those 15 s too on a board with Ethernet, so a device on a cable with no saved Wi-Fi network doesn't open an access point at each boot.
- `static` replaces DHCP (below, "Static addresses"), applied at once: the old address goes with the old settings. A static address is there at once; back on DHCP, Ethernet has no address until a lease (or a link-local address) comes, which the station waits 5 s for as after a link. `joined` is announced on each new address, "(link-local)" after a link-local one.
- **Names (DNS):** each interface's from its lease, or its static block's `dns`. An empty `dns` is the block's `gateway` (a router usually answers names), or no server of its own when `gateway` is empty too. lwIP keeps one list of servers for all interfaces, which esp_netif empties whenever an interface starts its DHCP client (a station associating, a cable's link) or takes a static address, and a lease's servers come in its ACK, seconds before its address (lwIP's address check). So a DHCP client is started by esp_netif on an association or a link, not by an attempt to join, and Network fills an empty list again from the interface the default route uses first (Ethernet while it has an address, then the station): its static server, or its lease renewed (the ACK brings the servers back), at most every 5 s per interface while a DHCP server is silent. A static address without a server of its own, or a link-local one, gives none: the next interface does. The policy is Network's and host-tested against a fake list that empties as esp_netif's does; the platform's `IpStack` only reads and writes the list, renews a lease (in lwIP's task) and posts `DnsEmpty` from lwIP's interface callback, and Network also refills on an association, a link, an address, and each second's poll, which catches a lost event (2026-10-21; before, the platform's callback refilled the list itself and renewed leases from inside lwIP's callback). Until v0.1.17, a station trying to join while Ethernet's lease was being checked left the device without names (no SNTP, no GitHub updates) until the lease's renewal.
- **PHY drivers:** ESP-IDF 5.x has drivers for LAN87xx, IP101, RTL8201, DP83848 and KSZ80xx; 6.0 moved them to the esp-eth-drivers components and keeps the generic IEEE 802.3 driver (from 5.4), which TesserKIT uses for every PHY there.
- **State and address:** the device is `online` over either interface, and `provisioning` only with no saved network and no link. `dev.address()` is Ethernet's address, else the station's, else the access point's, and `dev.addressMac()` the MAC of that interface (the station's on Wi-Fi): Art-Net's poll replies give both.
- **Lost events:** the platform keeps each interface's state (the cable's link, whether the station is joined, the addresses, the access point's clients), and Network reads it back once a second besides following the events, which only make a change known sooner. An event the device's queue lost (§4.2) is thus caught up within a second: a cable pulled while its `EthLink` was lost no longer keeps the station away for good in `fallback`, nor a lost `StaDisconnected` the station `connected`. A station's lost address (`StaGotIp`) is found once the attempt has associated; a lost failure of an attempt is still its 10 s or 20 s timeout. A station joined while Network counts on none (a late disconnection of an earlier link made it give up the attempt) is taken as connected when it has an address, or left when the station is idle (kept away by Ethernet, off, nothing saved): found under chaos (§12), where a station stayed joined beside Ethernet's lease. Ethernet's address going while the link stays (a lease that expired) counts as the lease lost.
- **Settings apply after the reply:** a change to the network's settings (networks, the station's mode, the access point, Ethernet's address or switch, the hostname, mDNS, power saving) is applied once they have been still for 150 ms. TesserAPI sends a reply after releasing the API lock, so a new address applied at once could come before the reply to the write that set it, which the client then lost (seen on a Gledopto, 2026-10-15); and a few quick patches make one reconnection.

**Static addresses** (each saved network's `static`, and Ethernet's): with `enabled`, the interface takes `ip` and `mask` instead of asking DHCP.
- `ip` is a host's address (not 0.0.0.0, loopback, multicast or broadcast) and `mask` a netmask (ones then zeros, /1 to /32): anything else is refused, so a refused patch changes nothing.
- `gateway` may be empty: a network without a router (a show network, a cable to one computer). The interface then has no gateway, and with the station joined too, traffic beyond the cable's subnet goes out over Wi-Fi (below, "Two interfaces at once").
- `enabled` with no address yet stays on DHCP, with a warning in the log. TesserAPI validates a patch's fields one at a time, so this combination isn't refused: it needs a validator on the whole object *(later, TesserAPI)*.

**Access point:**
- `fallback`: up when no saved network is joined within 15 s of trying, and down 30 s after the station connects (time for a phone on the AP to follow the device's new address). `always`: always up (AP+STA). `never`: never, except in recovery (below).
- **Recovery:** with no saved network, the AP comes up whatever the mode, so a fresh device is always reachable.
- **Captive portal:** while the AP is up and the station isn't connected, a DNS server answers every name with the AP's address, and the HTTP server's 404 handler redirects to `/` (`302`). OS connectivity probes (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`) thus all land on the UI, so phones open it on joining. This costs no URI handler slot.

**mDNS:** `<hostname>.local`, with `_http._tcp` and TXT records `api=/api`, `ws=/ws`, `version=<version>`, `tesser=1`, and `_tesser._tcp` with `api` and `ws`, what TesserAPI's `HttpServer::advertise()` announces and TesserUI's bridge lists for the launcher (§8), on ESP-IDF's `mdns` component (about 34 KB of flash). On by default, since it is how a provisioned device is found; `-DTESSERKIT_NO_MDNS` leaves it out, with the `mdns` setting.

**Channels and NowTP:** a station follows its router's channel, and ESP-NOW must share it (TesserAPI §14). NowTP follows by itself: a node whose station or access point fixes its channel is an anchor, and the nodes nearby follow it when it moves (§5.11). Network reports channel changes to `dev.onChannel(fn)` as well. Without a router, the AP's `channel` is the field's ESP-NOW channel. Network reports its state (provisioning, connecting) from its start, so services that wait for the network see it at once.

**Two interfaces at once** (`always`, or a static or link-local address on the cable): `dev.address()` gives Ethernet's address, mDNS answers on each interface with its own, the default route is Ethernet's while it has a gateway (its lease's router, or a static one), else the station's (Leo, 2026-10-21), and the DNS servers come from the default route's interface first. Network chooses it (`IpStack::setDefault()`, esp_netif's manual default) on every change it hears of, and at its next poll for one whose event was lost: a cable to a computer (link-local) or a static one without a gateway leaves the internet to the station, where with the cable as the default every connection beyond its subnet failed (board B, 2026-10-21: "server unreachable" on each HTTPS request, SNTP never synced). esp_netif keeps a manual default until Network sets another, rather than choosing again by route priority on each address event, so after a pulled cable the route moves when Network sees the link go (at once from the event, within a second from the poll). Ethernet's route priority (128, above the station's 100) only decides before Network's first choice. Time asks SNTP again when the route moves while the clock isn't set (`Device::routeGeneration()`). Two addresses in one subnet confuse routing: give the cable its own subnet, or leave the station to `fallback`.

**Not in the first release:** Ethernet modules on SPI other than the W5500 (DM9051, KSZ8851SNL, W6100, *(later)*), WPA2-Enterprise, IPv6 configuration.

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

- **Upload:** a `PUT /api/sys/firmware/image` with the raw `.bin` as body, streamed to the next OTA slot through a TesserAPI file node (§14). TesserUI's file widget shows progress. Over serial, Bluetooth, WebSocket and NowTP the same node takes the image in base64 chunks of `upload` envelopes (TesserAPI §10.1.1), the client sending one at a time: a 1.66 MB image takes 216 s over a serial line at 115200 baud (7.5 KB/s), and the chunk that completes the image gets the reply before the restart. The image header is checked on the first chunk (wrong chip or not an app image is `invalid_value` before anything is written). A completed upload sets the boot slot and restarts after the reply. The announced size is erased in 64 KB blocks just ahead of the bytes written: a 1 MB image uploads in 12 to 15 s on a classic ESP32 (25 s erasing sector by sector as it arrives). Erasing the whole image at once is as fast but keeps the idle task from running for over 5 s, which trips the task watchdog (a panic on Arduino).
- **Download** (`fw.downloads()`, before `begin()`): `esp_http_client` in a task of its own (8 KB of stack), following redirects (release assets redirect to a CDN), streaming through the same checks as an upload, with progress in `progress`. HTTPS uses ESP-IDF's certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`). TLS and the client cost about 130 KB of flash, hence the opt-in: without `downloads()`, neither the action nor the client is linked. The action replies at once; completion shows in `state`.
- **Rollback:** with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a new image starts as `pending`. It is confirmed automatically once the device has been within clients' reach for 60 s: `online` (always, without a Network service), or with a station joined to its access point, so a device out of its router's reach and used through its access point keeps an update made there; or by `confirm`. A device nobody can reach (an image that breaks the station and has nobody on its access point) stays pending, and a crash or reset before the confirmation boots the previous image. A confirmation that fails is tried again every 10 s. `rollback` makes the previous image boot next; it then starts `pending` too. ESP-IDF refuses an update while the running image is unconfirmed, so starting one confirms it: it is evidently working. Arduino-ESP32 confirms every new image at boot unless the application defines `verifyRollbackLater()`; TesserKIT defines it (returning true) whenever Firmware is linked, so the same rule applies there.
- **GitHub releases** (`fw.github(repo, asset)`, before `begin()`; implies `downloads()`): the device asks `https://api.github.com/repos/<repo>/releases/latest` itself, so the browser needs no internet, unlike ESP32-sveltekit. The check runs in a task of its own (8 KB of stack) and reads the answer through an ArduinoJson filter as it arrives, keeping only the tag and the assets' names and URLs: WLED's answer is 72 KB. The asset named after the pattern is the image, with `{target}` replaced by the chip (`esp32`, `esp32c3`...) and `{version}` by the tag without its `v`. The status is `available` when the tag is later than `version` in semver order (a leading `v` ignored). A git describe version (`v1.2.0-3-g1a2b3c4`, `-dirty`) counts as later than its tag, so a development build isn't offered the release it started from; a version that isn't semver is offered any other release. `update` installs the asset as `download` would. A URL instead of `owner/repo` names another latest-release endpoint answering as GitHub's does (Gitea, Forgejo, `test/e2e_github.py`'s server). About 10 KB of flash above `downloads()`, linked only when `github()` is called.
- **Checks:** `check`, and with `autoCheck` (on by default) once the device has been online for a minute, then daily, or an hour after a failure; never during an update. GitHub allows 60 unauthenticated requests an hour per address, far above a fleet's daily checks behind one router. Failures say why: `no release found` (404: no release yet, or a private repository), GitHub's rate limit (403, 429), a release without this chip's image (`v1.2.0 has no name-esp32c3.bin`, `latest` still shown), an unreadable or cut answer. On a classic ESP32, checking WLED's release takes 5 s and briefly takes about 50 KB of heap, mostly TLS. Releases come from CI (§13).
- **Fleets:** a gateway reaches its Wi-Fi peers' `/sys/firmware/download` like any other path (`/peers/<name>/sys/firmware/download`), so "update all" is one write per peer, without the browser fanning out (spin-seq's Remotes page). A NowTP-only peer's image goes through the gateway: an upload to `/peers/<name>/sys/firmware/image` is relayed to it in its chunks (TesserAPI §14, f204291), one transfer per peer at a time, from any client. TesserUI sends it in WebSocket envelopes, since a relayed HTTP upload holds the gateway's HTTP task for the whole transfer (about 30 s per MB). Measured (2026-10-13, DEVKIT-V3 relaying to DEVKIT-R1, both on `examples/idf/full`): a 1.66 MB image in 90 s (18 KB/s), the gateway's free heap about 9 KB lower meanwhile. The peer must let the gateway write: paired, or open (§7). The gateway waits 3 s for each chunk's reply and sends it again twice at most, so an erase on a block boundary and the image's check at the last chunk must each answer within 3 s: the check, the slowest, took 1.3 to 1.5 s. A last chunk sent again after its reply was lost fails though the update went through: the peer's `version` tells.

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

- **Download:** `api.persistedState()`, secrets included (it is Admin-only), as `<hostname>-settings.json`. Values that belong to this one device stay out (TesserAPI's `.backup(false)`, 2026-10-22): `/sys/auth/key`, which would let the backup's holder forge tokens for it, the boot count, the access point's `securePassword` and the Peers passwords; a restore leaves them as they are, also from an older backup that has them. It holds every persisted value, defaults included, and the settings version (`@version`, §6.3), so a restore is exact: it gives the backed-up settings whatever changed since (TesserAPI b611da6). The device's storage, unlike a backup, holds only what differs from the defaults (TesserAPI §12), so a later firmware's new defaults reach what the user never changed.
- **Restore:** applied with `api.restore(json, &skipped)` (TesserAPI §12): lenient like a boot load, so a backup from an older firmware restores whatever still fits; the reply (`{"skipped": [...], "restart": bool}`) lists the skipped paths. Every record is saved at once, then the device restarts one second later if `restartAfter` was set when the restore began (the backup's own `restartAfter` applies to the next one).
- **Stored on the device** when the application has a Files service (§5.12): `save` writes the settings to `backups/<name>.json` there, replacing a backup of that name. An empty name is the local time (`2026-10-12-1430`, `-2` if taken) once the clock is valid, else `backup-001`, `backup-002`... Names are 1 to 27 characters (the file name's 32 with `.json`). Beyond `keep`, the oldest backups go, by modification time (a clock that wasn't set reads as oldest), never the one just saved. A timer can call `/sys/backup/save` daily (§5.6). `backup.save(name)` does the same from the application, and `backup.keep(n)` sets the default.
- **Undoable restores:** before any restore (an upload or a stored backup), the settings it replaces are stored as `before-restore`, overwritten by the next restore and not counted by `keep`.
- **A restore whose save waits:** the settings are applied at once; if saving them waits for memory (settings on the filesystem on a tight heap), the restore still succeeds, its reply says `"saved": false`, `/sys/status/settings` says the save is waiting, and the device doesn't restart (which would lose them) until a later one. TesserAPI saves them as soon as it can. Seen on DEVKIT-V3 over its serial line (2026-10-15), where it had answered `internal`.
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
- **Tokens** are stateless: `<name>.<expiry>.<HMAC-SHA256 of both, truncated to 16 bytes, hex>`, signed with a random key kept in the persisted state (`/sys/auth/key`, a hidden secret). They survive restarts; `logoutAll` revokes them all, and removing an account revokes its. The expiry is the clock's time plus `sessionHours`. A token issued while the clock isn't set (a show network without SNTP) says `b<boot>:<seconds of uptime>` instead: it is valid in that boot only, until `sessionHours` of uptime (`/sys/auth/boots`, a hidden persisted count, grows at each start). Until 2026-10-22 such a token said 0 and never expired; one of those is refused now, so its client signs in again. The device registers a token check with TesserAPI (`api.authenticate()`), so HTTP and WebSocket clients present it as `Authorization: Bearer` or `?token=` (TesserAPI §13).
- **First boot:** until an admin user exists, every client is Admin and `/sys/auth/open` is `true` (hidden, for conditions); `openWarning`, a read-only text with `ui.style "warning"` and `visibleWhen "open=true"`, is TesserUI's warning callout. A factory default account (`auth.defaultAdmin("admin", "secret")`, before `begin()`) avoids the open window for devices shipped to others.
- **Off the API lock:** `login` defers its reply and hands the key derivation to TesserAPI's background task (`tesser::background()`, TesserAPI §9), so the 280 ms don't stall every transport. Under the lock it checks the lockout and copies the account's stored hash; the job hashes without it, then, under the lock briefly, counts the failure or issues the token, and refuses a sign-in whose account changed meanwhile; the reply is sent after the lock. A full background queue (8 jobs) answers `busy`.
- **Writing a password** (`users`, in any patch) hashes it on the background task too. A list patch can't be deferred (a setter runs in the patch's second pass, after validation), so the write is applied at once and only the hash comes later: the setter draws the salt and queues the derivation; until it lands (about 280 ms), the new password is the account's (`login` compares it, after a dummy hash for even timing) while the persisted state, backups and reads keep the previous hash, so plain text is never stored. The validator still runs first, so a refused patch changes nothing. So a backup taken, or a restart, within that time keeps the previous password (none for a new account, which then can't sign in until it is set again); a password written again meanwhile drops the older one's hash. With the background queue full, the password is hashed at once under the lock, as before.
- **Revocations:** an account added, removed or changed (password, role), `logoutAll`, and the device closing (its first admin account) call TesserAPI's `tokensChanged()`: open WebSockets, streams and lines that hold a level from a token, or from the open device, are checked again and lose what they no longer may do (a WebSocket losing subscriptions is closed, and TesserUI reconnects).
- **Brute force:** after 5 failed logins from one client (its transport and address: an IP, a MAC), `login` answers that client `busy` for 30 s; the others still sign in, so a stranger can't keep the admin out. The failures of 8 clients are counted, the oldest unlocked one making room; with all 8 locked or being checked, a new client is answered `busy` too. Sign-ins still being checked count as failures to come, so with 5 failures and checks together, another answers `busy` at once ("sign-ins being checked"): no more than 5 are tried before the lockout. An unknown name costs a hash like a wrong password (against a fixed dummy hash), so timing doesn't tell which names exist.
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
  homeAssistant *(p)    boolean (default false): Home Assistant discovery
  discoveryPrefix *(p)  string (default "homeassistant"), not empty, no + or #, no slash at either end, advanced
  status/               connected, error, published, dropped, missed (watched counters), entities (announced to Home Assistant)
  lost                  event, warn toast: a connected session dropped by the broker or the network, with the error
```

- **The status on a short heap:** it is rebuilt from the heap at each change and every second, reserved first through TesserAPI's byte budget. Without room the previous status stands until the next period (routes and sign-ins go on), rather than aborting in the device's task, as DEVKIT-V3 did as a gateway during a file download (2026-10-15).

- **Session:** the client connects once the device is online, and reconnects by itself (ESP-IDF's esp-mqtt, every 5 s, keepalive 30 s). A change to `enabled`, `uri`, the credentials, `clientId` or `prefix` restarts the session once the settings have been still for 1 s, so editing several fields reconnects once. Each session has a number, carried by its platform events, so a stopped session's late events are ignored. The session's state is also read back once a second (`MqttClient::connectedNow()`), as Network reads its interfaces: a `MqttConnected` the device's queue lost (§4.2) left the bridge down for the whole session.
- **Bridge:** while connected, each `publish` entry is a subscription of its own (TesserAPI `LocalClient`, Admin: what to publish is the admin's choice) with its `interval`. Its snapshot and every change notification are flattened into leaves, each published as JSON to `<prefix>/<path>`, retained: `/led` gives `tesser/lamp-a67c/led/on` → `true`. Arrays and keyed lists are leaves (their JSON). Events under the path go to `<prefix>/<event path>` unretained. A notification flagged `overflow` is followed by a read of the whole subtree. The subscriptions end with the connection, and a reconnection publishes everything again, so retained topics are refreshed. They count against TesserAPI's 16 subscriptions.
- **Commands:** the client subscribes (QoS 1) to `<prefix>/+/set`, `<prefix>/+/+/set` … 8 levels deep, rather than `<prefix>/#`, which would bring back every retained topic it publishes. A message on `<prefix>/<path>/set` becomes `set <path> <payload>` through a `LocalClient` with **User** access (TesserAPI `setAccess`), so the broker's clients can drive the application but can't reconfigure `/sys`. The payload is JSON; anything that doesn't parse is a string (`ON`, a name), and an empty payload is `null` (an action without argument). The reply goes to `<prefix>/<path>/result`, unretained: `{"status": "ok", "body": true}` or the error. Commands come through the device's event queue, where at most 8 wait (§4.2): a burst larger than that while the task is busy (a broker's retained commands at connection, during an OTA chunk) keeps the latest 8, and the others count in `status/missed`.
- **Availability:** `<prefix>/status` is `online` on each connection and the last will is `offline` (QoS 1), both retained. Stopping a session publishes the will itself before disconnecting, since a clean disconnection doesn't.
- **Platform:** `MqttClient` (§3.3) on esp-mqtt, a component of ESP-IDF 5 and `espressif/mqtt` from ESP-IDF 6 (a dependency of TesserKIT's manifest for those versions). Publishing only queues: each session's task of the platform layer (`tk_mqtt_pub`, 4 KB of stack) sends with `esp_mqtt_client_publish()`, from the session's queue of 32 messages and 16 KB, so TesserKIT's task never waits on the network. Once the session stops, that task skips what is left, publishes the will, destroys the client and ends, freeing its queue: a stopped session holds no memory. esp-mqtt's own queue (`esp_mqtt_client_enqueue`) isn't used: its task sends one queued message per pass and waits up to a second between passes. A message the queue refuses counts in `dropped`.
- **TLS:** `mqtts://` and `wss://` brokers are verified with ESP-IDF's certificate bundle when the application calls `mqtt.tls()` before `begin()`; without it the bundle isn't linked. A TLS session reserves its memory first (§11).
- **Factory defaults:** `mqtt.broker("mqtt://...", user, password).enable(true).prefix("home/lamp").publish("/led", 0).homeAssistant(true)`.
- **Home Assistant discovery:** with `homeAssistant` on, each value and action under the published subtrees is announced to Home Assistant's MQTT discovery: a retained JSON config at `<discoveryPrefix>/<component>/<hostname>/<object id>/config`, the object id being the path with `_` for `/` (`led_on`). The component comes from the schema, and "writable" means writable by the broker's clients (User access, as commands are):

  | Node | Component |
  |---|---|
  | boolean | `switch` writable, else `binary_sensor` |
  | integer, number | `number` writable (`min`, `max` from the range, a slider; without one a box from -10⁹ to 10⁹; `step` from the node's, else 1 for an integer), else `sensor` (`state_class: measurement`) |
  | string with options (an enum) | `select` writable, else `sensor` with `device_class: enum` and its options |
  | other string | `text` writable, else `sensor` |
  | action without argument | `button` (`payload_press: null`) |

  Secrets, `ui.hidden` nodes (and objects), events, lists, arrays, files, remote nodes and actions with an argument aren't announced; at most 64 entities. Each config has the node's label, prefixed by its parent's below the root ("LED on"), a `unique_id` (`<hostname>_<object id>`), the leaf's state topic and its `/set` command topic, `<prefix>/status` as availability, the unit and the display precision (`ui.precision`), `entity_category` `diagnostic` (read-only) or `config` (writable) under `/sys`, and the device (identifiers the hostname, name the device's name (§4.4), manufacturer TesserKIT, model the chip, the firmware's version, a link to its page). Payloads are JSON both ways: `payload_on: "true"`, `value_template: "{{ value_json }}"` for strings, `command_template: "{{ value | tojson }}"` for strings and `"{{ value | int }}"` for integers, since Home Assistant formats numbers as `100.0`.
- **Announcing:** on each connection, and when `homeAssistant`, `discoveryPrefix` or the publish list change (no reconnection), the configs are queued after the states and sent four a period while the client's queue takes them: discovery never pushes states out, and a full queue only delays it (it isn't `dropped`). Configs announced earlier in this boot that no longer apply (a path removed from the list, discovery switched off, another prefix) are emptied, which removes them from Home Assistant. Those announced before a restart can't be known: switching discovery off before changing what is published removes them cleanly. Disabling MQTT leaves the configs, and Home Assistant shows the entities unavailable (the will). `CONFIG_TESSERKIT_HOME_ASSISTANT=n` (`-DTESSERKIT_NO_HOME_ASSISTANT` outside ESP-IDF's build) leaves discovery out, its settings and `status/entities` with it: 7.9 KB on the ESP32-C3.
- One generic bridge replaces ESP32-sveltekit's per-service `MqttEndpoint`s.

### 5.9 ArtNet

```
/sys/artnet             label "Art-Net", Admin
  enabled *(p)          boolean (default true)
  universe *(p)         integer 0–32767: the mappings' 15-bit port-address (net, sub-net, universe)
  mappings *(p)         list ≤ 8 of {path, coarse 0–512, fine 0–512, min, max, threshold 0–255}
  status/               packets, universe (the last seen), received (hidden), age (s since a frame of the mappings' universe, -1 none; shown once received), sequenceErrors (advanced), error; the counters as of the device's last period
  refused               event, warn toast: a new error in status/error
```

- **Receiving:** an Art-Net 4 node on UDP 6454 (unicast and broadcast). Without a sink, TesserKIT's task drains the socket without blocking every period (up to 64 datagrams) and keeps the last ArtDmx frame of the mappings' universe; ArtDmx for other universes only counts.
- **Mappings:** each mapping turns channels of the frame into a write to `path` through the API (a `LocalClient`, Admin, as Timers do), **only when the written value changes**. `coarse` is the channel (1–512, 0: unused), `fine` the low byte's (0: 8 bits). The 16-bit value `v` (8 bits: `coarse × 257`, so 255 is the top) gives `min + v/65535·(max − min)`, written as a number, rounded for an integer node, or `true` from half up for a boolean node. A `threshold` (1–255) makes a gate: `true` while the coarse channel is at or above it. A frame too short for a mapping's channels leaves it alone. New mappings or a new universe write everything again on the next frame.
- **The application's sink** (2026-10-23, pixled-controller's gap report: an LED controller receiving 16–24 universes at 40–44 frames a second, about 1,000 datagrams a second): pixels never go through the tree. `sink(DmxSink*, DmxTask)` (before `begin()`) and `universes(list)` (any time, from any task, up to 64; sorted, those out of range left out) hand those universes' frames to the application raw: `DmxFrame` {protocol, universe, sequence, priority, syncAddress, data, length, source address, arrival time in µs on `Core::uptimeMs()`'s clock}. With a sink the socket gets a receiving task of its own (`UdpSocket::listen()`: `DmxTask` priority 12 by default, below lwIP's 18 and above the device's and the HTTP server's 5, either core, 3 KB of stack, the rest of which is the sink's), and `frame()` is called in it as each datagram arrives, not under the API lock: the sink copies what it needs (≤ 512 bytes, under its own lock) and returns, and must not call the service back. If the task can't be created, frames come from the device's task every period. The mappings keep working beside the sink, fed by the same datagrams; a universe can be both. ArtSync reaches the sink's `sync()` with its source, so the application can show a frame on sync (closing it on the sender's ArtSync is the application's: Art-Net says to follow only the ArtSync of the controller sending the ArtDmx).
- **Load** (2026-10-23, `test/e2e_dmx.py` on Gledopto D: classic ESP32, `examples/idf/full -DPROFILE=classic -DETHERNET_BOARD=gledopto -DDMX_PROBE=24`, every classic service, cabled at 100 Mbit to a Mac, which sends 24 universes at 44 frames a second, each frame's universes back to back then a sync: 1,100 datagrams a second, with a whole-tree `?sub` stream open and `/sys/status` read every second). ESP-IDF's defaults lose most of a burst: ten 512-byte DMA buffers hold five frames, and the socket's mailbox six. The share of frames that reached the sink, in runs of 10 to 30 s:

  | Settings | Art-Net | sACN multicast | Lowest heap |
  |---|---|---|---|
  | defaults (160 MHz, mailbox 6, 10 × 512 B DMA) | 33% | 36% | 13.8 KB |
  | mailbox 32 | 41% | 53% | 32.6 KB |
  | + 30 × 512 B DMA | 82% | 95% | 22.9 KB |
  | + 240 MHz | 91% | 98% | 21.4 KB |
  | 28 × 768 B DMA, mailbox 32, 240 MHz, Wi-Fi joined too | 100% | out of memory (abort) | under 13.7 KB |
  | the same, the station off (`wifi/mode` never) | **100%** (5 × 30 s, 158,400 frames and 6,600 syncs, sACN unicast too) | **100%** | 14.4 KB (49 KB at rest) |

  The committed code on the same settings, 3 × 30 s: Art-Net 100%, sACN unicast and multicast 99.96% and 99.98% (12 and 7 frames, at most two of a universe), lowest heap 14.5 KB. Whether those few were lost by the device or by the sender (Python, through a USB adapter and a hub) isn't known: a console's own measurement will tell.
  A receiving task above lwIP's (priority 19) with a mailbox of 6 or 12 kept the backlog out of the heap but lost 0.02–0.2%: lwIP's task, running a burst through uninterrupted into a deep mailbox, is what keeps up. Over Wi-Fi (an access point shared with other traffic) a third of the datagrams were lost whatever the rate, about 1% when the datagrams were spread through the period: bursts are lost on the air. So an application receiving many universes sets, in its sdkconfig: `CONFIG_ETH_DMA_RX_BUFFER_NUM=28` and `CONFIG_ETH_DMA_BUFFER_SIZE=768` (a burst of 24 frames and their sync in one buffer each: 21.5 KB of static RAM instead of 5 KB), `CONFIG_LWIP_UDP_RECVMBOX_SIZE=32` (frames waiting there are heap: up to about 24 KB at a burst's peak, for every UDP socket), `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`, and keeps the station off while Ethernet works (`wifi/mode` never, or fallback with a DHCP lease). TesserKIT's profiles keep ESP-IDF's defaults, which suit a device mapping a universe or two.
- **Sequence numbers:** per universe of the sink or the mappings, a frame whose sequence doesn't follow the previous one (1–255, then 1 again; 0: not numbered) counts in `status/sequenceErrors`: lost or reordered on the way. Frames are delivered all the same. The sink gets each frame's sequence, so counting per universe is the application's.
- **Errors:** a refused write is kept in `status/error` (`/led/brightness: invalid_value: out of range`) until a write to the same path succeeds.
- **Discovery:** an ArtPoll is answered (from the device's task, up to four controllers a period) with ArtPollReplies, unicast to the controller on port 6454: the device's address (station, else access point), its MAC, version, short name the device's name (§4.4), long name the hostname, a node report with the packet count, and DMX output ports: the sink's universes, and the mappings' universe when there are mappings or no sink universes. Art-Net 4 puts four ports in a reply, sharing a net and a sub-net: one reply per group of up to four, with bind indexes 1 (the root), 2, 3…, all giving the root's address. A port's GoodOutput says a frame came in the last 3 s.
- **Platform:** `Udp` (§3.3) on lwIP sockets: `receive()` without blocking for the device's task, `listen()` for a task blocked in `recvfrom` (100 ms timeouts to notice the socket closing, which waits for the task). Sending a reply from the device's task while the receiving task waits works on lwIP (`LWIP_NETCONN_FULLDUPLEX`). A port that can't be opened is tried again every 5 s.
- **Factory defaults:** `artnet.universe(0).map({"/heliostat/artnet/azimuth", 1, 2, 0, 360})`.
- What a value means stays the application's: tiny-heliostat maps channels onto `/heliostat/artnet/azimuth` and `elevation` and decides there whether Art-Net takes over (§10.1). It replaces helionext's `ArtNetService` and serves any lighting-controlled device.
- ArtNet and Sacn (§5.22) share their code (`DmxService`, `tesserkit/dmx.h`): the tree, the mappings, the sink and its task, the counters. An application gives both the same sink and tells them apart by `DmxFrame::protocol`.

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
- **Boards:** about 200 KB of flash on ESP-IDF. `examples/idf/full` leaves it out on the ESP32-C3, whose larger code wouldn't fit the files table's slot with it (§11), and in its `classic` profile for the classic ESP32, where it takes about 48 KB of heap (§11, Profiles); `examples/idf/bluetooth` has it on the C3. On Arduino, the classic ESP32's prebuilt Bluedroid includes classic Bluetooth and fills a 1.94 MB slot with TesserKIT's services: ESP32-C3 and ESP32-S3 only (about 1.5 MB).

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
  nodes                 read-only keyed list (key "mac") of {name, mac, rssi, channel, otherChannel, tesser, authenticated}; otherChannel: held on another channel by its access point, seen but out of reach (a warning)
  time/                 label "Network time"
    reference *(p)      "auto" (default), "this", or a node's MAC (either case), ui.perNode (one reference, not every node)
    interval *(p)       1–600 s (default 5): between synchronization rounds once synchronized, advanced
    synced, isReference, source (the reference's MAC)
    offset (µs), roundTrip (µs), drift (ppm), rounds, age (ms)   advanced
  status/               started, channel, sent, received, failed
```

- **Transport:** NowTP on ESP-NOW (discovery, channel following, pairing, network time) with TesserAPI's `NowTpTransport` serving the tree to the nodes nearby, and advertising it in discovery, so they mount this device too.
- **UI host:** with `mountPeers`, the TesserAPI nodes found are mounted under `/peers` (label "Nodes nearby"), each a remote node with its page in TesserUI's menu (TesserUI §6.3), forwarded over ESP-NOW. `nodes` lists every node heard, TesserAPI or not.
- **Starting:** once the network has settled, joined or on its own access point (at the latest 20 s after boot, when the fallback access point is up): while the station scans and joins, ESP-NOW can't hold a channel. Its channel is then the router's or the access point's; `channel` only applies to a device with neither. A `channel` written while the station or the access point fixes the channel is kept for later and changes nothing then (NowTP refuses the move). When the station leaves (Ethernet taking over, §5.2), ESP-NOW stays on the router's channel, where the nodes nearby followed it, rather than going back to `channel` by itself: it moves only when `channel` is written again (NowTP's `moveToChannel()` would bring the nodes along) or to follow an anchor.
- **Pairing:** `accept` opens this node's pairing window for 60 s, `pair` takes the installation key from a node whose window is open (NowTP's X25519 exchange, proving the pairing code). Both run in a task of their own and report in `pairing/state`. With a key, NowTP drops every message not authenticated with it.
- **Network time:** NowTP's `TimeSync` (its DESIGN, port 252): every node estimates the offset between its clock and the reference node's from NTP-style exchanges, so an instant named in network time ("strobe at T", "start at T") happens at once on every synchronized node, each on its own clock. `reference` sets this node's role (NowTP d2b515e's `TimeRole`): `auto` is a candidate, `this` the reference, a MAC a follower of that node, which never takes over. Candidates elect a reference with no setting, so a fleet of identical devices synchronizes as it is: a node set to `this` first, else an acting candidate; when none has been heard for 5 s, the candidates take over in turn, synchronized ones first, then the lowest MAC, and the reference stays until it is lost (a lower MAC booting later doesn't take over). A node that takes over keeps serving the network time it had (its estimate frozen), so the clock doesn't jump; after a reboot, the old reference comes back as a follower. The first reference appears about 5 s after start, a new one about 12 s after a loss; followers run on their drift estimate meanwhile. For the application: `espNow.networkTimeUs()`, `espNow.localTimeUs(networkUs)` (the `esp_timer_get_time()` value at that instant: schedule there, from an `esp_timer` with `ESP_TIMER_ISR` dispatch or a hardware timer, since a task-dispatched callback adds tens of µs of jitter), from any task, and `espNow.timeSynced()`. The status values change with each round (announced then); the offset, drift and age are read when asked. `reference` and `interval` apply at once. Accuracy (nowtp-9f, 2026-10-19, two classic ESP32s 1 m apart, both timestamping the same access-point beacons): in steady state the error between them has a median of 1–40 µs, 95 % within about 95–120 µs, the worst about 350 µs; it ramps between rounds (the drift estimate) and steps at each. Within 100 µs holds for the median, not for every instant; enough for Fluante (346 µs is 0.24° at 116 rpm, inside a 500 µs flash, and its phase lock filters the offset), so NowTP's tighter outlier rejection and the radio's receive timestamps are *(later)*, when an application needs them. Not yet measured edge to edge on GPIOs.
- **Access:** nodes holding the installation key are trusted, so Admin: the transport trusts every sender while the network is secured, which only key holders can reach. Without a key, a node is a client without a token: Admin while the device is open, public after (TesserAPI §13). A gateway checks its own clients before it forwards (§9).
- **At runtime:** `enabled`, `channel` and `longRange` apply at once; `mountPeers` and `pairingCode` at the next boot (the restart warning). A factory reset also forgets the installation key, which NowTP keeps in NVS of its own (namespace `nowtp`).
- **Platform:** `NowTpRadio` (§3.3), built when the build has NowTP (TesserAPI's `TESSER_HAVE_NOWTP`, or `<NowTP.h>` on Arduino). Mounted nodes keep pointing at the transport, so stopping NowTP stops the radio and the transport's task but keeps the transport.
- **Memory:** about 18 KB of heap while running (NowTP's event buffer, ESP-NOW; NowTP's task only with `ownTask(true)`, §4.2), 24 KB with the transport's own worker (`espNow.ownTask(true)`): by default its requests run in the device's task (§4.2).
- **NowTP 0.6** (82e7eba, nowtp-5c, Roadmap Phase 4): the ESP-NOW interface follows Wi-Fi's mode by itself (the station's while it runs, else the access point's; the node's MAC changes with it, so peers see a new node and forget the old one after 7 s); a station that loses its access point tells its followers at once on the old channel, and announces the new one on joining; receivers advertise their free room in acknowledgements, so senders hold a message a full receiver can't take; callbacks run outside NowTP's lock. *(later)* Network calling NowTP's `announceChannelChange()` before joining a network on a known channel, so followers move at once.
- **Factory defaults:** `espNow.mountPeers(false).channel(6).longRange(true).txPower(15).pairingCode("...")`; `espNow.timeReference(ref)` and `espNow.timeInterval(seconds)` for the network time; `espNow.maxCalls(n)` as UdpApi's (§5.19).
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
  format        action: erase every file (ui.confirm, advanced); the settings stay
```

- **The partition** is a LittleFS data partition labelled `files` (TesserKIT's `*_files.csv` tables, §11), else `spiffs` (Arduino's default and `min_spiffs` tables), mounted at `/files`; `files.partition(label)` and `files.root(path)` change them. A partition that holds no filesystem (new, or SPIFFS from an older firmware) is formatted. One an application already mounted (Arduino's `LittleFS.begin()`) is used as it is, at the root it was given. Without a partition, or without LittleFS in the build, `mounted` is false and every operation is `not_allowed`. The device mounts it in `begin()`, before loading the settings, which it keeps there (§6.1), through `Files::settingsStorage()`, virtual so that a firmware without Files doesn't link the filesystem storage or the move; `files.mount()` is idempotent, so the service's start doesn't mount it again.
- **Names** are 1 to 32 characters, without `/`, `\` or control characters, and don't start with a dot; UTF-8 is fine. Paths below the root are at most 63 characters. Anything else is `invalid_value`, so no request reaches outside the root, nor the hidden entries: the uploads' temporary files and the settings' directory, `/.settings`, which no listing shows and no request or helper can read, write, rename or delete.
- **Uploads** go to a hidden temporary file (`.<name>.part`) beside the target and are renamed over it once complete, so a cut transfer or a power cut leaves the old file or the new one, never a part. The suffix keeps a file named `settings` from having the settings' directory as its temporary file. An upload announcing more than the free space is `too_large` before any byte is written. Writes run outside the API lock (TesserAPI §4.6); the listing follows within a period of the device's task.
- **Room for the settings:** while the settings live on the filesystem (§6.1), an upload or a helper's write that would leave less than `files.settingsReserve(bytes)` free (`Files::kSettingsReserve`, 64 KB) is `too_large` ("not enough space: the rest is kept for the settings"), before any byte is written if the upload announced its size, else as it writes. So a full filesystem never blocks a settings save. 64 KB is the worst case of TesserKIT's records (§11) in LittleFS's 4 KB blocks: `sys` (16.7 KB: 5 blocks) and `sequencer` (10.3 KB: 3), the temporary copy of `sys` while it is rewritten (5), the directory's metadata pair (2), and a block for LittleFS to move metadata to. It doesn't count the space the settings take already, so it errs on the safe side; an application with large records of its own raises it.
- **Format** keeps the settings while they live there: it erases every file and directory but `/.settings`, rather than formatting the partition. A format would leave the settings only in memory until written again: a power cut then, or a heap too short for the writes, would lose them all, the Wi-Fi networks included, and the device would come back on its defaults, on its access point. Refusing the format would be as safe but would leave no way to clear the files. A filesystem LittleFS can't mount is formatted when mounted, as before.
- **Write errors say why:** a write the filesystem refuses for want of space is `too_large` ("the filesystem is full"); one LittleFS can't do for want of heap (its caches, allocated as it writes) is `busy` ("not enough free memory to write the file now"), to try again; any other is `internal`. Seen on DEVKIT-V3 as a gateway (2026-10-14): a 20 KB upload over the serial line, with 24 KB of 384 KB used, was refused as "full" when it was memory.
- **One directory at a time:** `dir` is the device's, shared by its clients: a browser opening a directory moves the others' listing too. Keys can't hold `/`, so a listing of the whole tree would need another key; one directory keeps every row addressable by name.
- **The application** reads and writes its own files through stdio below `files.root()`, or with the helpers (`files.write(rel, data)`, `read`, `remove`, `rename`, `mkdir`, `list`), which other services use too. After writing through stdio, `files.changed()` updates the listing.
- **Factory reset** doesn't format: files are often data rather than settings. Only the stored backups go (§5.5), and the settings in `/.settings` (§6.1).
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

### 5.14 Log

```
/sys/log                label "Log", ui.icon "menu", read Admin
  level *(p)            enum none, error, warn, info, debug, verbose: what the firmware logs, its console included
  line                  event: a log line, "W (1234) wifi: ..."
  recent                file, download only (text/plain, log.txt): the last lines
  dropped               integer: lines lost (advanced)
```
- **Recent lines without allocating:** they are kept in a ring of `history` bytes (default 4096) allocated once at start, so keeping a line never allocates: on a short heap, a log line (often about that very shortage) mustn't be what aborts the device, as it did on a Gledopto with every service and Ethernet during a file upload (2026-10-15, `Log::keep`'s copy). The oldest lines go whole to make room; a line longer than the ring is cut. The download takes a copy reserved from the byte budget, answered `busy` without room.

- **Capture:** the platform's `LogOutput` (§3.3) hooks ESP-IDF's log output (`esp_log_set_vprintf`), which TesserAPI's warnings and errors go through too. Each line still goes to the console; a copy, without its colour codes or newline and cut at 160 characters, waits in a 2 KB ring for TesserKIT's task, which emits up to 16 lines a period as `line` events and keeps the last 4 KB (`log.history(bytes)`) for `recent`. Lines that find the ring full are counted in `dropped`. Lines are assembled from what each call prints, so ESP-IDF's log v2, which prints a line in pieces, works too.
- **No feedback:** while the service emits, the lines its own task logs aren't captured, so a transport that warns per event can't feed itself.
- **Level:** `esp_log_level_set("*", level)`, so it applies to the console as well, and to every tag without a level of its own. Lines above the build's `CONFIG_LOG_MAXIMUM_LEVEL` aren't compiled in: a level above it is refused (`invalid_value`). The examples log warnings (their console's UART also carries the API, §8), so their maximum is `warn`. The factory default is the level the firmware runs at when the service starts, or `log.level(tesserkit::LogLevel::Info)`.
- **Access:** logs carry addresses, network names and errors: Admin only.
- **In the interface:** `line` has TesserUI's `log` widget (`ui.lines` 200): the lines as they come while the page is open, monospace, errors and warnings coloured; `recent` downloads the history. Arduino's own `log_x()` macros print without esp_log unless the core is built with `CONFIG_ARDUHAL_ESP_LOG`, so only ESP-IDF's and TesserKIT's lines come through there.

### 5.15 Battery

```
/sys/battery            label "Battery", ui.icon "battery", write Admin
  soc                   integer 0–100 %, read-only, ui.widget gauge
  voltage               number, V, read-only, ui.widget chart
  charging              boolean, read-only: with a charging pin, or fed by the application
  low *(p)              integer 0–50 % (default 15): below it, lowBattery (advanced)
  lowBattery            event, warn toast: the charge, once each time it falls under `low`
```

- **Sources:** an ADC pin through a divider, `battery.adc(pin, ratio)` (the battery's voltage is the pin's times `ratio`: 2 for two equal resistors), sampled each second, 16 readings averaged and smoothed over the last few samples; or the application's own measurement (a fuel gauge, a power management chip), `battery.update(volts, soc, charging)` from any task, where a charge or a charging state below 0 means unknown. A charger's status pin, `battery.chargingPin(pin, activeLow)`, is read with each sample (TP4056's CHRG: open drain, low while charging, so pulled up).
- **Charge:** from the voltage on a lithium cell's resting curve (3.27 V empty, 3.84 V half, 4.2 V full), or a straight line, `battery.linear(emptyV, fullV)`, for other chemistries and several cells. A voltage under load or while charging reads high or low: the charge is an estimate, which a fuel gauge's `soc` replaces.
- **Notifications:** the voltage is marked changed when it moves by 10 mV, the charge when it changes by a percent. `lowBattery` is emitted when the charge falls to `low` or under while not charging, then again only after charging or rising 5 % above it. Putting the device to sleep at that point is the application's (or a timer's) choice: Sleep (§5.16) is an action like any other.
- **Platform:** `Pins` (§3.3): ESP-IDF's ADC oneshot driver with its calibration (curve fitting, or line fitting on the ESP32) at 12 dB, ADC1 only: the ESP32's ADC2 belongs to Wi-Fi. On Arduino, `analogRead()` claims ADC1 itself: a sketch reading its own analog pins feeds the service with `update()` instead.
- **Factory default:** `battery.lowAt(20)`.

### 5.16 Sleep

```
/sys/sleep              label "Sleep", ui.icon "moon", write Admin
  sleep                 action (integer 0–31 536 000 s): deep sleep for that long, 0 until the wake pin; ui.confirm
  wakePin *(p)          integer -1–63: the GPIO that wakes the device (-1 none), checked against the chip
  wakeLevel *(p)        enum low, high (default low)
  wakePull *(p)         boolean (default true): the chip's pull resistor holds the pin at the other level (advanced)
  wakeCause             enum none, timer, pin, other, read-only: how this boot woke (none: not from deep sleep)
```

- **Sleeping:** the action answers, then half a second later TesserKIT's task calls the application's `dev.onSleep(fn)` callbacks (to park its hardware, hold its pins), saves what the persistence's debounce still holds, and enters deep sleep. A sleep with neither a time nor a wake pin is refused, and so is one during a firmware update (`busy`). The device wakes as from a reset, with `/sys/status/resetReason` `deep-sleep`.
- **Wake pin:** ext1 on chips that have it (ESP32, ESP32-S3: RTC GPIOs only; on the ESP32 a single pin's "all low" is that pin low), the GPIO wake-up of the others (ESP32-C3: GPIO 0–5). The pull resistor stays on through sleep (the ESP32's RTC peripherals are kept powered for it). A devkit's BOOT button (GPIO 0, pulled up) wakes it at `low`: `sleep.wakePin(0)`.
- **Composition:** being an action, sleep is what a timer (§5.6) or a sequence step (§5.13) can call: "every night at 23:00, sleep 8 hours".
- **Platform:** `Power` (§3.3).
- **Factory default:** `sleep.wakePin(pin, high, pull)`.

### 5.18 UdpApi

```
/sys/udp                label "UDP", ui.icon "bolt", write Admin
  enabled *(p)          boolean (default true)
  port *(p)             integer 1024–65535 (default 8484), advanced
  interfaces *(p)       flags Ethernet, Wi-Fi, Access point (default all), advanced: where UDP serves and reaches nodes
  peerAccess *(p)       enum public, user, admin, auto (default auto: user while the device is open, public in secure mode, §7): the level of a node that presents no token
  mountPeers *(p)       boolean (default true): the nodes found on the networks under /peers (at the next restart)
  restartNeeded         hidden; restartWarning, a warning callout while it is true
  send                  action {path, value, to}: a write to every node of the networks at once, or to one
  status/               running; sent, received, retransmits, failed (advanced)
```

- **Transport:** TesserAPI's `UdpTransport` (its §10.7) on the local networks the device is on, Ethernet, the station and the access point: the API's envelopes in datagrams, delivered reliably, so remote nodes, forwarding, subscriptions, file transfers and the `*` fan-out work as over NowTP (§5.11). It starts once the device has an address (Network's), or 20 s after boot without one, advertises the hostname, and answers queries, so the nodes on the network find each other within a second.
- **Nodes:** the TesserAPI nodes advertising on the network are mounted under `/peers` beside NowTP's, each with its page. A device both transports reach is mounted once, by the first to announce it (TesserAPI's §14.1: NowTP knows it by its MAC, UDP by the station MAC it advertises).
- **Send:** `send` with an empty `to` is one broadcast set (`value` is JSON, empty for an action without argument), sent twice by the transport and applied once by every node of every local network at the level it gives this device; unanswered, and not applied here, so a sequence step that should also act here writes here as well. With an address (`192.168.1.97`, `192.168.1.97:8484`) it is a reliable set to that node, and the action replies with its reply. This is spin-seq's broadcast to its nodes (§10.2) without a format of its own: `send {"path": "/sequencer/play", "value": "\"intro\""}` starts every node's sequence at once.
- **Timing and loss:** on Ethernet the nodes get a broadcast within about a millisecond of each other. Through a Wi-Fi access point they get it together, but at its next DTIM beacon (about 240 ms on the test network), and access points send group frames once, without retries: the two copies make a loss rare, not impossible (one of 72 broadcast writes between two devkits through the home access point, §12). A write that must arrive goes to each node with `to`, or through `/peers/*/...`.
- **Access:** datagrams prove nothing about their sender. A node is at `peerAccess`, or at its token's level if it sends one (an `auth` envelope); while the device is open (no admin account, §5.7) every node is Admin, as every other client is. `send` itself is Admin.
- **Interfaces:** `interfaces` keeps UDP to some of the device's networks (TesserAPI 1cf73f9, its §10.7): nothing is served, advertised, broadcast or reached on the others. Ethernet alone keeps a show network's traffic off Wi-Fi entirely. With one interface the socket is bound to it, which needs the interface up: the transport starts again when the device's address changes (the interface came up, or got another address).
- **Runtime:** `enabled`, `port` and `interfaces` apply at once (the transport says goodbye, closes its socket and opens the new one), `peerAccess` too; `mountPeers` at the next restart.
- **Platform:** `UdpApiTransport` (§3.3), which owns TesserAPI's transport: its receive task (4 KB of stack) reads the socket, and the requests run in the device's task (§4.2; TesserAPI's `Tasks::Receive`, 6336093), or in a worker of their own (6 KB) with `udp.ownTask(true)`. The protocol is TesserAPI's platform-independent `UdpLink`, which the host tests run over an in-memory network.
- **Factory defaults:** `udp.port(8484).peerAccess(tesser::Access::User).mountPeers(true).interfaces(tesserkit::UdpApi::kEthernet)` (`peerAccess` an explicit level instead of auto); `udp.enable(false)` leaves it off until enabled; `udp.maxCalls(n)`, the calls in flight to other nodes (default 8, §5.19).

### 5.19 Peers

```
/sys/peers              label "Nodes", ui.icon "target", write Admin
  order *(p)            string: the routes tried for each node, in order ("udp/ethernet, nowtp"); empty: the order found
  account *(p)          { name, password (secret) } ui.form: signed in with on every node (empty name: none)
  nodes *(p)            keyed list (key "id"), ≤ 16, advanced: settings per device
                          id       the device's station MAC, 12 hex digits (colons allowed), as /peers shows it
                          name     its node's name when declared by address (empty: node-<last 6 digits>)
                          order    its own order; empty: the default
                          address  "192.168.1.97" or "…:8484": reached over UDP without being discovered
                          user, password (secret)  this device's account on that node; empty: account
  status                read-only keyed list (key "id"): id, name, online, route (the route in use: udp/ethernet, nowtp…),
                          routes (every route that reaches it, in the order tried, "(offline)" after a lost one), rtt (ms), heard (s ago; -1 never),
                          access (the level the node grants this device's token), signIn (how signing in went)
  lost                  event, warn toast: "lamp-f37c", when a node goes offline on every route; "lamp-f37c: wrong pairing key" when NowTP can't authenticate it (another key, or none), even while another route reaches it
  reached               event, info toast: "lamp-f37c over nowtp", when a node is back or goes another way
```

- **Routes:** UdpApi (§5.18) and NowTp (§5.11) mount their nodes under the same `/peers`, and a device both reach is one node there with a route per transport (TesserAPI §14.3, 02c3bf4): requests take the first online route in the order, the next one when a node is lost on it (UDP: a message unacknowledged for 3 s, a goodbye, a restart; NowTP: discovery losing it), and come back to the preferred one as soon as it is heard again. `order` is TesserAPI's default route order, a node's `order` its own; both apply at once, to nodes already mounted and to devices that appear later.
- **Route names** (TesserAPI 1cf73f9): `udp/ethernet`, `udp/wifi` (the station) and `udp/ap` are UDP by one interface, the one whose subnet holds the node's address; `udp` is UDP by any; `nowtp`. A device heard on two interfaces has a UDP route by each. Routes the order doesn't name come after the named ones, so `udp/ethernet, nowtp` tries Ethernet, then NowTP, then UDP over Wi-Fi; to never use UDP over Wi-Fi, keep UDP off it (`/sys/udp/interfaces`, §5.18). Checked on the devkits (Wi-Fi only): their routes read `nowtp, udp/wifi`; `udp/wifi, nowtp` took UDP and `udp/ethernet, nowtp` NowTP, UDP over Wi-Fi coming after; with V3's UDP kept to Ethernet (which it hasn't), R1 showed `udp/wifi (offline)` and went over NowTP, and came back to UDP when V3's interfaces were all again.
- **By device:** settings and status are keyed by the device's id, the station MAC that both transports know it by (`"id"` in `/peers`' schema), so a setting follows a device whatever its name, and can be written before the device is seen. The name is beside it.
- **Declared devices:** an `address` mounts the device over UDP at that address (a node on another subnet, whose advertisements don't arrive), under `name`; TesserAPI probes it every 5 s while it is offline. A device already found gets a UDP route at that address instead (when it has none). Nodes are never removed: removing a setting takes effect at the next restart.
- **Status** is refreshed every second, and at once when a node is mounted, gets a route, or changes state or route (TesserAPI's `RemoteNode::onStateChange()`, which the service owns). `rtt` is the smoothed round trip of the calls on the route in use.
- **Signing in:** a node with an admin account gives a node that presents no token its `peerAccess` (§5.18), Public by default in that secure mode (§7). With an account (`account`, or a device's own `user` and `password`), this device signs in to each node's `/sys/auth/login` (§5.7) once the node is online, and presents the token it gets on every route (TesserAPI §13.3, 05190c8): the node then grants it that account's level. The token is presented again whenever the node may have forgotten it (a restart, a failover); when the node refuses it (expired after its `sessionHours`, revoked by `logoutAll`), this device signs in again, at most every 30 s. A wrong name or password isn't tried again until the account changes, since a node locks sign-ins out after five failures; a node without the Auth service ("the node has no accounts") needs none: it is open. Other failures (no answer, locked out) are retried after 30 s. `access` is the level the node granted the token, `public` until it has answered (the token goes before the next request to the node). `signIn` tells how signing in went: "signed in as show", "wrong name or password", "token refused", "couldn't sign in: …". Changing or removing the account drops the token. Tokens and sign-ins travel in clear, as UDP's requests do (§5.18): a LAN you trust. The account's level is this device's, not its clients': a request a client below Admin makes through `/peers` carries that client's level as a ceiling, and the node grants the lower of the two (TesserAPI §13.1, 390dd57), so an anonymous visitor of a gateway that signs in as an admin reaches the nodes as public, and a user as a user. A node on a TesserAPI before 390dd57 can't apply a ceiling and refuses such a request: only this device's admins reach it through the gateway.
- **A device gone** (`peers.onDeviceLost(fn(mac, reason))`): a node driven by a master undoes what the master did when the master is gone from every transport, not when it fails over from one to the other (TesserAPI §14.4), or when it restarted, even though it is back at once: what it held is gone (TesserAPI 1993c78's `PeerLoss`). `reason` says which: `failed` (also a NowTP message its retries couldn't deliver, TesserAPI 66f0000), `goodbye`, `restarted`, `silent`, `stopped`, `lost` or `auth_failed` (NowTP can't authenticate it: another pairing key; the `lost` toast says so) (`tesser::toString()`). UdpApi and NowTp hand their endpoints' `onPeerLost()` to the service when the endpoint is created; the function runs in the device's task, under the API lock. Who writes is `api.currentClient()->device` during the write.
- **Streaming to nodes:** `RemoteNode::byDevice(peers.nodes(), mac)` finds a device's node (`peers.nodes()` is `api.object("peers")`), and `node->stream(path, json)` writes to it in order without waiting (TesserAPI §14.3). A set in flight takes one of the endpoint's calls: `udp.maxCalls(n)` and `espNow.maxCalls(n)` (default 8, fixed when the transport starts) should be at least the number of nodes streamed to at once.
- **On hardware** (2026-10-13, DEVKIT-V3 and DEVKIT-R1 on `examples/idf/full`, both on the home Wi-Fi, ESP-NOW on channel 6): each devkit mounts the other once, with routes "nowtp, udp", and the Gledopto on Ethernet over UDP only. Writing `order` switched the route at once; round trips 21–22 ms over NowTP, 70–100 ms over UDP through the access point. R1's UDP turned off: V3 went over NowTP within 2 s, and back to UDP when R1's came back, stable after. Signing in: V3's admin write to R1 (which had an admin account) went from `unauthorized` to done, and `access` read `admin` after it. After both boards were flashed, V3 received almost no UDP until it was restarted, as TesserAPI saw with this pair: routes then flapped to NowTP every 15 s (UDP's silence timeout).
- **Factory defaults:** `peers.order("udp, nowtp").account("show", "...")`.

### 5.20 PWM outputs

Not a service: a class for applications, `tesserkit::PwmOutput` (`tesserkit/pwm.h`), on the LEDC, for outputs whose frequency must be exact and finely adjustable: ballet-photophonique's outputs and Fluante's strobe, which both had a copy (2026-10-19).

- **Plan:** the LEDC divides a source clock by a divider with 8 fractional bits (1 to 1023.996), then counts 2^bits steps per period: clock / (divider · 2^bits) = frequency. `pwm::plan(chip, hz, bits)` takes the fastest clock whose divider fits, so the finest frequency steps (0.0007 Hz at 60 Hz and 12 bits); when none fits, the nearest resolution that can, fewer bits for a frequency too high, more for one too low; past what any resolution reaches, the closest end. The classic ESP32 (`pwm::kEsp32`): APB (80 MHz) or REF_TICK (1 MHz) per timer, 20 bits, 0.001 Hz to 40 MHz. The S3, C3 and other chips with APB (`pwm::kApb14`): one clock for every timer, APB, 14 bits, 4.8 Hz to 40 MHz.
- **An output:** `PwmOutput out(espPwm(), pin)`; `setFrequency(hz)`, `setDuty(percent)`, `setResolution(bits)`, `setOn(on)`, stored before `begin()` and applied after, each only when it changes something. What runs: `timing()` (clock, divider, resolution, the frequency the divider gives, a step's length), `counts()`, `appliedDuty()`, `pulseUs()`. No tree: each application mounts its own values with its own labels. Not thread-safe: one task, or the tree's setters under the API lock.
- **The LEDC** (`Pwm`, `platform/esp/pwm_esp.cpp`): each output takes a timer and a channel of its own, from the bottom up, on the classic ESP32 the high-speed group's four first, then the low-speed group's (8 outputs; 4 elsewhere). TesserMotion takes the low-speed group's from the top down; Arduino's `ledcAttach()` and `analogWrite()` take them from the bottom up too, so they don't mix with these. `ledc_timer_config()` only sets a timer up (APB, 1 kHz); the plan is written to its registers through the low-level layer. A new divider or clock alone keeps the counter running, with no glitch, so a phase lock can steer the period; a new resolution resets the timer (a counter past the new top would run on to the wrap: minutes on a slow clock).
- **Duties** apply at the end of a period, and `ledc_update_duty()` spins in a critical section until the previous one was applied, longer than the interrupt watchdog allows at low frequencies. So a duty is written only once the channel's previous one was applied; until then the latest waits, and `Pwm::poll()` writes it: call `espPwm().poll()` often, under the same lock as the setters: `dev.onTick([&](uint32_t) { tesser::Lock lock(dev.api()); tesserkit::espPwm().poll(); })`.
- **Phase lock controls** (2026-10-21, for Fluante's strobes locked across nodes on network time, §5.11): the lock itself (filtering, the controller) is the application's; TesserKIT gives what touches the LEDC. Crystals differ by up to ±20 ppm, so a lock trims the period continuously (the divider's steps are about 12 ppm at 60 Hz and 12 bits).
  - `sample(count, localUs)`: the output's counter (0 to 2^bits − 1) and `esp_timer_get_time()` read in one critical section. On the ESP32, S2, S3, C3, C6 and H2 (the timer's value register); false elsewhere.
  - `setTrim(steps)`: the divider moved by whole steps (1/256) from the planned one, clamped to its range, without restarting the counter (a new divider alone keeps it running, above). `timing()` is what runs, trimmed; `planned()` the timing for the frequency asked, the same on every node, the lock's target; `trim()` the steps applied. A new frequency or resolution clears it.
  - `restartAt(localUs)`: the counter reset at an instant of the local clock, a new period beginning with its high part, for the first lock. A one-shot alarm of a GPTimer (taken at the first restart, 1 MHz) whose interrupt resets the LEDC timer: a few µs late, more while the flash is written (the handler isn't in IRAM). One restart waits at a time, a new one replacing it; false for an instant past or under 200 µs away (`Pwm::kMinLeadUs`). A restart landing in the high part makes one longer pulse.
- *(later)* A phase through `hpoint`.
- `end()` (2026-10-21): the channel idles low, its timer pauses and is free for another output, and the pin goes back to a plain GPIO output, low, so an application can give it to a `PulseOutput` (§5.21) and back. Settings are kept for the next `begin()`. The phase lock's restart alarm runs its handler from IRAM, which a build with `CONFIG_GPTIMER_ISR_CACHE_SAFE` (needed by §5.21) requires.

### 5.21 Pulse trains

Not a service: a class for applications, `tesserkit::PulseOutput` (`tesserkit/pulses.h`), for several pulse trains on one pin, the pin high whenever any of them is (their union: an on/off driver isn't brighter for overlaps). Fluante's layers (2026-10-21): several strobes on one LED driver, each with its own frequency, phase and pulse length. The GPIO matrix gives a pin one peripheral signal, so LEDC channels can't share it, and MCPWM operators each follow one timer: the edges are scheduled by a timer's alarm instead.

- **Trains:** up to 8, each `{on, periodUs, phaseUs, pulseUs}` in the time base: pulses begin at n · period + phase. A period under 16 µs, a pulse of 0 or a value that isn't finite leaves its train off; a pulse at least as long as its period keeps it high.
- **Time base:** the application's clock, `base = offsetUs + rate · local` (`setTimebase()`), local being `esp_timer_get_time()`. With network time (§5.11) as the base, nodes asked the same trains pulse together, with no restart and no divider trim; the filter that sets the base (once a second, say) stays the application's.
- **Planning and the interrupt:** the task plans in doubles (`pulses::plan()`): each train's next pulse found with `fmod()` (exact; the base's rounding only, 0.25 µs at Unix-time magnitudes, far less from a boot), then each train as an anchor and a period on the local clock in integers of 2^-20 µs. The alarm's interrupt (`pulses::Engine`) only adds and compares integers, so it never touches floating point. It steps every train to the alarm's instant and 4 µs beyond, sets the pin to the union, and sets the alarm to the next edge. `poll()` (the device's tick) plans again every second, so the integer steps never drift from the instants: 40 days of uptime and a base offset of 3.7·10¹¹ µs keep each pulse within 1 µs of its instant (host test).
- **Changes** (trains, time base, cap; from the setters and the tick, under the application's lock) are installed under the interrupt's spinlock and apply from the next edge. A pulse in progress keeps its end. Each train's next pulse is the new plan's nearest to the one it was waiting for, so a plan that moves the pulses by less than half a period neither repeats nor drops one. A new train starts with the first pulse at most 50 µs late. A time base step of more than half a period may add or drop one pulse; it never makes one longer.
- **Safety:** an end is never skipped. A late interrupt ends every pulse whose end has passed at once, starts a pulse in progress late (it still ends on time), and skips pulses wholly past. Edges within 4 µs are taken together, so a shorter gap merges two pulses and a shorter pulse isn't emitted. `setMaxPulseUs()` bounds the union, whichever trains keep it high (`Stats::capped`). An alarm set behind the counter would wait for its 54-bit wrap, and the pin with it: after setting one, the interrupt reads the counter again and steps again while it is behind.
- **The ESP side** (`Pulses`, `platform/esp/pulses_esp.cpp`): a GPTimer at 1 MHz, started once, its alarm moved to each next edge, the pin written by register. Its interrupt runs from IRAM while the flash is written, which needs `CONFIG_GPTIMER_ISR_CACHE_SAFE` (5.1: `CONFIG_GPTIMER_ISR_IRAM_SAFE`) and `CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM`: without them `begin()` refuses, since a flash write would hold a pulse on for tens of milliseconds. The interrupt is allocated on core 1 at level 3 (the highest a C handler may take; from ESP-IDF 5.2): on core 0, beside Wi-Fi, it was held back up to 600 µs at idle, so a 500 µs pulse lasted 1.1 ms. One output per chip. `end()` stops it and leaves the pin low, for a `PwmOutput` to take (§5.20).
- **Diagnostics** (`stats()`): interrupts, the pin's edges, interrupts more than 20 µs late and the worst lateness, pulses the cap ended; `resetStats()`.
- **Measured** (2026-10-21, Gledopto D, classic ESP32 at 160 MHz, ESP-IDF 6.1-dev, Wi-Fi joined; `test/pulses_probe`, whose MCPWM capture on the same pin times its edges at 80 MHz): one train at 60 Hz, 500 µs. Idle, 60 s: each pulse's start within 3.7 µs of a period after the previous one (mean 0.46 µs), lengths within 4.2 µs, no interrupt more than 20 µs late. HTTP reads as fast as the device answers, 60 s: mean 0.92 µs, 26 interrupts late, the worst 186 µs. Settings saved (NVS) three times a second: mean 0.51 µs, the worst 97 µs. A firmware upload (flash erased and written): mean 1.7 µs, the worst 66 µs. Eight trains near 1 kHz (16,000 edges a second) beside HTTP reads: every edge, 1,108 interrupts late of about 124,000, the worst 191 µs. What holds core 1 back under HTTP load isn't known yet.
- **In an application** (2026-10-21, fluante-firmware-85 on NoiseFluante: classic ESP32 rev 1.0 at 160 MHz, its LED driver on GPIO 19, TesserMotion's motor control task on core 1 at 50 Hz over a UART, the time base steered from NowTP's offset; the engine's lateness, no capture): 1–2 trains at 60 and 30 Hz with Wi-Fi, HTTP polling and settings saved to LittleFS, 11 interrupts late of about 37,000 edges, the worst 77 µs; 8 trains at 1 kHz (15,800 edges a second) with light HTTP, 56 late, the worst 388 µs, likely the motor task's UART spinlocks on core 1. Locked to network time within 3–31 µs across nodes. The pin handed between a `PwmOutput` and the trains both ways, and the strobe's `restartAt()` works in the same CACHE_SAFE build.
- Host-tested (`test/pwm_cases.h`, the plan's cases from ballet-photophonique); built for the ESP32, S3 and C3. The phase lock's controls ran on a Fluante (2026-10-21: classic ESP32, 60.01 Hz at 12 bits, a high-speed timer, Fluante's lock against another Fluante over NowTP): the first restart landed 99 µs off at the first sample after it, the lock held within −74 to +68 µs over two minutes, its trim between −6 and +11 ppm following NowTP's drift estimate, with no jump on a trim change.

### 5.22 Sacn

```
/sys/sacn               label "sACN", Admin
  enabled *(p)          boolean (default true)
  universe *(p)         integer 1–63999 (default 1): the mappings'
  mappings *(p)         as ArtNet's (§5.9)
  status/               as ArtNet's
  refused               event, warn toast
```

ANSI E1.31 (streaming ACN) beside Art-Net (2026-10-23, pixled-controller's gap report; Leo: it belongs here, for every lighting-controlled device). The same tree, mappings, sink (`DmxFrame::protocol` is `Sacn`) and receiving task as ArtNet (§5.9). About 4 KB of flash with the IGMP it links, and ArtNet's shared code 4 KB more than ArtNet alone was (2026-10-23, ESP32): `examples/idf/full` has it in the classic and psram profiles (`CONFIG_FULL_SACN`), not in the full set, whose slot has little room left.

- **Receiving:** UDP 5568, unicast and multicast. The socket joins the groups 239.255.<high byte>.<low byte> of the sink's universes, the mappings' universe and up to four synchronization addresses that the winning sources' frames name (so their sync packets arrive), on every interface with IGMP (Ethernet, the station, the access point), through the platform's `UdpGroups` (§3.3), from lwIP's task (`igmp_joingroup_netif`: lwIP's socket option allows only as many memberships as there are sockets in all, and ESP-IDF allocates IGMP groups from the heap, so 64 universes fit). It asks again whenever the list or the device's address changes and every 5 s, joining interfaces that came up since; groups no longer listed are left, and all of them when the socket closes. While a socket has groups, Ethernet passes every multicast frame (`ETH_CMD_S_ALL_MULTICAST`, promiscuous before ESP-IDF 5.5): the ESP32 MAC filters seven addresses, which the all-systems, mDNS and IPv6 groups mostly take, and lwIP still drops groups it hasn't joined. *(later)* Only above a few groups.
- **Packets:** data packets (root vector 4, framing vector 2, DMP 0x02/0xA1) for universes 1–63999 count in `status/packets`. Preview data (option bit 7) is ignored. Packets with a start code other than 0 (per-channel priorities 0xDD...) aren't levels: they keep their source alive and in sequence, and aren't delivered. *(later)* Per-channel priorities, universe discovery (universe 64214), sources' names in the status.
- **Sources and priority:** per universe of the sink or the mappings, up to three sources, told apart by their CID. The highest priority (0–200) wins; among equals the one already winning keeps it, else the first heard. Only the winner's frames go to the sink and the mappings. A source silent for 2.5 s (E1.31's network data loss timeout; checked as each packet of the universe arrives) or sending Stream_Terminated (option bit 6) is dropped and the next takes over at once. A fourth source is ignored while three are live.
- **Sequence numbers** are per source: a packet 1 to 20 behind its source's last is out of order and dropped (E1.31 6.7.2); any other that doesn't follow the last counts in `status/sequenceErrors` and is taken.
- **Synchronization:** a frame's synchronization address reaches the sink (`DmxFrame::syncAddress`, 0: show it now), and every sync packet (root vector 8, framing vector 1) reaches `sync()` with its address and source. Holding frames until their sync and the 2.5 s fallback are the application's.

### 5.23 Buttons

Not a service: a class for applications, `tesserkit::Button` (`tesserkit/button.h`), as `PwmOutput` is (2026-10-23, pixled-controller's function button; Leo: it belongs here). A push button on a GPIO, sampled every period of the device's task (20 ms) and debounced (a level holds 30 ms to count).

- **Presses:** `Short` on release, before the long time (1 s by default); `Long` once when held that long, while still held; `VeryLong` once more when held `veryLongMs` (0, none, by default; 10 s is WLED's factory reset), whose action is the application's. The release of a long press is nothing. `heldMs()` says how long the press in progress has lasted, for a light that tells the user what letting go now does.
- **On a pin:** `attach(dev, pin, activeLow, fn)` before `Device::begin()`: active low against the chip's pull-up (a Gledopto's button on GPIO 17, a devkit's BOOT button on GPIO 0), active high against its pull-down, through the platform's `Pins` (`inputBegin(pin, Pull)`); `fn` runs in the device's task. `update(down, nowMs)` is the logic alone, platform-free, for a level the application reads itself.
- Host-tested (`test/button_cases.h`, from pixled-controller's); not run on hardware.

### 5.17 Later

| Service | Content |
|---|---|
| Ethernet on SPI | Modules other than the W5500 (DM9051, KSZ8851SNL, W6100), for chips without the internal MAC (§5.2). |

## 6. Persistence

### 6.1 Storage

- **Records:** TesserAPI's persistence (§12 there) saves the persisted nodes of the tree as one record per top-level node, debounced 2 s after the last change: `/sys` (every service's settings) is one record, each of the application's top-level objects another, and the settings version (`@version`, §6.3) one more. A change rewrites only its own record.
- **On the filesystem when there is one** (Leo, 2026-10-15): NVS is 20 KB in every table, and can't rewrite a `sys` record over about 6.7 KB, against a worst case of 16.7 KB plus 10.3 KB for the Sequencer (§11, "Settings in NVS"). With a Files service (§5.12) whose partition mounts, `begin()` mounts it before loading and keeps the settings there instead: TesserAPI's `FileStorage`, one file per record (`/.settings/sys.json`), each written to a temporary file and renamed into place, so a power cut leaves the old record or the new one. LittleFS levels wear across the partition (384 KB in `tesserkit_4mb_files.csv`). No partition table changes, so no flashing over serial: an OTA update to a firmware with Files moves the settings.
- **The move from NVS,** at the first boot with the filesystem: the records TesserAPI keeps in NVS (namespace `tesserkit`, which holds nothing else: the tree's top-level names and `@version`; NVS can't list its keys) are copied to `/.settings`, each read back and compared; then a marker record (`@moved`) is written. NVS keeps its copy of them, no longer written (below). Nothing else in NVS is touched either: the PHY's calibration, NowTP's key (namespace `nowtp`), Wi-Fi's own data, a record of a node the tree no longer has. Each copy reserves the write's buffers and the copy read back from TesserAPI's byte budget, so a short heap stops the move rather than aborts it. The move is logged once ("settings moved from NVS to the filesystem (3 records; NVS keeps its copy)").
- **Cut short:** without the marker, the move is redone from an empty `/.settings` at the next boot, NVS still holding every record; with it, the filesystem's copy is the one in use. If a step fails, the copies are removed, the settings stay in NVS for this boot (logged, and in `/sys/status/settings`: `in NVS: moving them to the filesystem failed ("sys" couldn't be written)`), and the next boot tries again.
- **NVS otherwise:** without Files, with an application's own storage (`dev.storage()`), or when the partition doesn't mount (`/sys/status/settings`: `in NVS: the filesystem didn't mount`). A record too large for NVS to rewrite isn't written there (§11). NVS spreads writes across its pages, so frequent changes don't wear one sector.
- **Saving to the filesystem** reserves `kFileWriteBytes` (1.5 KB, one block) from TesserAPI's byte budget first, for what the write allocates beside the record: LittleFS's cache for an open file (`CONFIG_LITTLEFS_CACHE_SIZE`, 512 bytes by default; its read and program caches are allocated at mount) and the VFS's and newlib's file structures. TesserAPI's `FileStorage` writes and reads unbuffered: a stdio buffer would add the filesystem's block (4 KB), more than a tight board can give above the heap floor. On a Gledopto with every service and Ethernet (12.8 KB free, the floor at 8 KB) every save failed with it, and a change was lost at the next restart (2026-10-15). The record's text isn't counted again: TesserAPI reserved and allocated it before handing it over. Refused, the save fails, TesserAPI tries it again, and `/sys/status/settings` says so (`"sys" waiting to be saved: not enough free memory`) until it is saved: a change not saved yet is lost at a restart. It never aborts. The NVS guard doesn't apply there: the filesystem's capacity is Files' reserve (§5.12).
- **NVS's copy, a fallback:** once moved, the settings are written to the filesystem only, but NVS keeps the copy it had. A boot whose filesystem doesn't mount (or a later firmware without Files) starts from that copy, the network's settings among them, rather than from the defaults, so the device stays reachable; it may be older than the filesystem's. What such a boot saves goes to NVS, and the next boot that mounts the filesystem uses the filesystem's copy again (the marker is there).
- **Factory reset:** `Storage::erase()` removes every record (with the filesystem, the records and the marker in `/.settings`, and whatever NVS held of them), then the device restarts. The application's persisted values return to their defaults too.
- **Backups** (§5.5) go through `api.persistedState()` and `api.restore()`, the same on either storage.

### 6.2 Factory defaults

Defaults are the values the C++ variables are initialized with. Settings that vary per product are set before `begin()`:

```cpp
net.defaultNetwork("Workshop", WORKSHOP_PASSWORD);   // from a gitignored header, never committed
net.apMode(tesserkit::ApMode::Always);
clock.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
```

They apply only when the stored state doesn't set the value, so a user's change survives updates, and a factory reset returns to them. Storage holds only values that differ from them (TesserAPI §12), so a firmware that changes a default reaches every device whose user never changed it. ESP32-sveltekit's `factory_settings.ini` placeholders (`#{unique_id}`) become ordinary code (`dev.hostname()` already has the MAC).

### 6.3 Versions and migration

Storage holds only what differs from the defaults (§6.2), so a changed default reaches every device whose user never changed it, except where an older firmware stored the old default itself: TesserKIT before settings version 1 (v0.1.19 and earlier, on TesserAPI before 1a8c28e) froze every default into storage at first boot, and a backup holds whatever its firmware stored. Versions tell those values apart from a user's choice:

- **TesserKIT's version** (`Device::kSettingsVersion`, 2) covers `/sys`. `begin()` gives TesserAPI the stored version and a migration (`api.persistVersion()`, TesserAPI §12) before loading. State stored under an older version goes through each service's `migrate(from, settings)`, with its own object as stored (`/sys/<name>`, or `/<name>` for a service at the root, the Sequencer), which drops a value equal to a default that has since changed, or renames a key. Every record is then saved again, sparse, under the new version, which drops the values equal to today's defaults.
- **Version 1** made the records sparse. No persisted `/sys` default changed from TesserKIT's first release to it (v0.1.0 to v0.1.19, checked in the history on 2026-10-14), so its migration drops nothing itself: the values frozen before it equal today's defaults, and the sparse rewrite drops them. A later default change raises `kSettingsVersion`, and the service's `migrate()` drops its old value under the version before.
- **Version 2** (2026-10-21) made `/sys/network/hostname` empty for the default, so that the hostname follows the device's name (§4.4). Before, it read the hostname itself, its default the label's (or the application's `dev.hostname()`); Network's migration drops a stored one equal to that default.
- **The application's version:** `dev.settingsVersion(version, migrate)`, before `begin()`, for its own records. Its `migrate(record, version, value)` sees every stored record, `"sys"` included after TesserKIT's migration of it (an application that changes a factory default it sets, such as `defaultTz()`, drops the old one there), with the application's version the state was stored under (0 before it set one), and changes it in place; returning false drops the record. It runs only when that version differs.
- **Stored:** one number, `(application << 16) | TesserKIT's`, TesserAPI's `@version` record, also in backups (`"@version"`). A backup without it (from before versions) is migrated from 0 as it is restored (§5.5).

## 7. Security

- **Access levels** come from TesserAPI (§13 there): every client has one (`public`, `user`, `admin`), every node may require one to read and to write, inherited by its subtree. Nodes a client can't read are left out of its reads, schemas and notifications; writing one it can't write is `unauthorized`.
- **Defaults of the services:** status is public; settings read as public (secrets never readable) and write as Admin; network lists and auth read as User and Admin respectively.
- **Applications** mark their own nodes: `heliostat.writeAccess(tesser::Access::User)`.
- **Who has which level:** §5.7. Without the Auth service, everyone is Admin.

**Two modes** (Leo, 2026-10-22: security is opt-in, so deployment stays simple on secure or low-stakes networks; one switch turns on a coherent secure mode, and what costs nothing in deployment is done anyway).

- **Open mode,** while no admin account exists (`/sys/auth/open`): every client on every transport is Admin, as a new device must be to be set up. The access point is open while no password is set (provisioning); UDP nodes are User. Nothing on the network is trusted to be who it says, and nothing needs to be: anyone who reaches the device controls it. For a bench, a lab, a show network that is itself closed.
- **Secure mode,** once an admin account exists (`Device::secure()`, set by Auth; also from the launcher's claiming step, TesserUI): clients are Public until they sign in, and these follow at once, at run time, both ways:
  - the access point takes a password: its own if set, else `securePassword`, made for the device (§5.2), so the fallback network a router reboot brings up is no longer open;
  - UDP nodes without a token are Public (`peerAccess` auto, §5.18): a datagram proves nothing about its sender, so a spoofed one can't run User actions; nodes signed in with an account (Peers, §5.19) keep their level. A show that drives every node by broadcast (`/sys/udp/send`) sets `peerAccess` user, and accepts that anyone on that network can;
  - tokens made without a clock end with the boot (§5.7); a client failing to sign in is locked out alone (§5.7);
  - the HTTP server's strict mode (TesserAPI's `HttpServer::strict()`): sets and actions take JSON bodies only, so another site's page can't post to the device without a preflight; a request or WebSocket handshake with an `Origin` must come from the device's own page or its CORS origin (the launcher); and the `Host` must be the device's name, `<name>.local` or an IP address, against DNS rebinding (421 otherwise). It follows the hostname. The captive portal pays for it: in secure mode a phone joining the access point gets no portal page by itself (its check asks another name, refused), and the device is at 192.168.4.1;
  - sessions are checked again when tokens change (§5.7).
- **What secure mode protects against:** a stranger on the same network (Wi-Fi, Ethernet, the access point) reading settings or changing anything without an account; joining the fallback access point; spoofing a UDP node's level; keeping the admin out by failing sign-ins; reusing a token issued without a clock in a later boot.
- **What it doesn't** (*(later)* where marked; held for now as costing memory or performance, Leo 2026-10-22):
  - **Eavesdropping.** HTTP, WebSocket and UDP are plain: tokens and sign-in passwords cross the local network in clear, as with ESP32-sveltekit. A device that needs more uses a TLS-terminating gateway, or Web Serial. NowTP authenticates its frames once paired (an installation key) but doesn't encrypt them *(later: AEAD)*.
  - **Spoofed addresses with a token.** A token is bound to nothing: one seen on the network works from anywhere until it expires or `logoutAll`. UDP and NowTP clients are recognised by address, which an attacker on the segment can forge *(later: a fleet key authenticating datagrams)*.
  - **NowTP's discovery** without a key: goodbyes, move notices and announcements are unauthenticated, so a stranger in radio range can make nodes lose each other or leave their channel *(later)*.
  - **Cross-site requests in open mode:** any page a user visits on the same network can POST to an open device; secure mode refuses them.
  - **Backups** carry the accounts' password hashes, which an attacker holding one can test offline (the token key and this device's own secrets stay out).
  - **Physical access:** the serial console is Admin (the launcher's USB way), and flash isn't encrypted.

## 8. User interface

The UI is TesserUI's generic renderer, self-hosted by the device (TesserUI §9.4): `/`, `/ui.js`, and `/ui/widgets-<hash>.js`, the widgets few pages use (tables and lists, files, charts, gauges, knobs, the log, JSON, time zones, custom elements), loaded the first time a page needs one. About 67 KB of flash gzipped (TesserUI dde641f). Nothing is TesserKIT-specific in the bundle:

- **Menu:** `/sys` has `ui.page`, so TesserUI's menu (TesserUI §6.3) shows it as one entry with a sub-entry, and a page, per service. Each service names its icon (`ui.icon`): Status `info`, Network `wifi`, Time `clock`, Firmware `download`, Backup `refresh`, Timers `play`, Users `lock`, MQTT `link`, Art-Net `bulb`, Log `menu`, Battery `battery`, Sleep `moon`, UDP `bolt`, Nodes `target`.
- **This node's own:** `ui.perNode` marks what TesserUI's apply to all (§6.6 there) leaves out: the hostname, the access point's name, the static addresses (Ethernet's, and each saved network's), MQTT's client ID and topic prefix (a broker drops a client whose ID another takes). `/sys/name` has it through `ui.nodeName`. Art-Net's universe and mappings don't: a show's nodes may share them.
- **Services' pages** are their trees with hints: labels, units, `confirm` on destructive actions, `style danger`, `advanced` on rarely used settings, `form` where several fields must change together (the access point), `visibleWhen` for conditional fields (static IP), `chart` on heap and RSSI.
- **The application's pages** are its tree, plus TesserUI views (§8 there) for dashboards and custom elements (§9.8 there) for what generic widgets can't draw.
- **Notifications:** what happens in the background, after any reply, is an event with `ui.toast` (TesserUI §7.8) and `ui.hidden`: a toast, not an events log on the service's page. Network's `joined`, Firmware's `updateFailed` and `newRelease`, `timerFailed`, Mqtt's `lost`, ArtNet's `refused`, the Sequencer's `failed`, Battery's `lowBattery`, and Peers' `lost` and `reached`. TesserUI shows them on any page, as "<parent's label> › <label>: <payload>" ("Wi-Fi › Joined: 192.168.1.50 on Workshop"), so their labels don't repeat their parent's. A moment a reply or a widget already shows isn't one: a failed upload, a manual release check, a pairing (its state is on the page), a restore. Clients only hear events while subscribed, so what a device does at boot (a rollback, a crash) stays a value (`resetReason`, `coredumpPresent`).
- **Login:** when a request answers `unauthorized`, TesserUI's sign-in dialog calls `/sys/auth/login` with a name and password (TesserUI §7.4). Without the Auth service it never needs to: everyone is Admin.
- **Theming and text:** `dev.web().theme(...)`, `.text(...)`, `.title(...)` (TesserUI §9.4).
- **Favicon:** TesserKIT's, TesserUI's tesseract on a chip (a pin on each side, the same blue and strokes), inlined in the generated page as an SVG `data:` URL (231 bytes, no request), and on the launcher's page. An application names its own with `dev.web().icon("/icon.svg")` (an asset, or a `data:` URL).
- **The launcher** (TesserUI §9.7): the same bundle on GitHub Pages, `https://leokeba.github.io/TesserKIT/`, published by CI with the TesserUI the example firmware is built with (§13). It reaches a device without the device serving a page:
  - **USB:** Web Serial to a `LineTransport` on the console. The examples add one (`tesser::UartTransport` on UART0 with ESP-IDF, `tesser::StreamTransport` on `Serial` with Arduino); a serial client is Admin (§5.7), so a device is set up over a cable with no network at all. The launcher skips log lines, but one written in the middle of a reply breaks it: the ESP-IDF example logs warnings only, and warnings still come: a station retrying a saved network that doesn't answer broke about one long (3 KB) reply in ten on Fluante's board (2026-10-19).
  - **Network:** an address or `<hostname>.local`, from an HTTPS page, so the device must allow the launcher's origin: `dev.cors("https://leokeba.github.io")`, as the examples do. Chrome asks the user for local network access. There is no WebSocket from HTTPS, so the launcher polls values over HTTP and follows events (toasts, §8) through TesserAPI's streamed subscription (`GET …?sub`, two streams per device by default, each holding one of the server's sockets).
  - **Nearby:** TesserUI's bridge, on the user's machine, lists the devices announcing `_tesser._tcp` (§5.2) and serves them live. Each release carries it as a macOS executable, `tesserkit-bridge-macos-arm64.zip` and `-x64.zip` (TesserUI §9.7, built by its reusable `bridge.yml` from `pages/index.html`), which the launcher links to (`bridge` in the page's `window.TESSER`). It needs no Node, allows the launcher's origin, and serves the launcher itself on localhost with the flasher built in and the latest three releases' images fetched from GitHub, so *Install firmware* works there too.
  - **Installing:** *Install firmware* writes a release's factory image over USB (TesserUI §9.7.1), which resets the board's settings (the image covers NVS). Then, over the same line, the launcher scans with `/sys/network/wifi/scan`, puts the chosen network first in `networks` and follows `status` until the device has an address, and opens the device over the network or stays on USB.
  - **Bluetooth:** the Bluetooth service (§5.10): `examples/idf/bluetooth`, `examples/arduino/Bluetooth` (ESP32-C3 and ESP32-S3) and `examples/idf/full`. The device's name is its hostname.

TesserUI renders everything TesserKIT's services declare: files (§5.4, §5.5), secrets, access levels and sign-in, flags (§5.6), the time zone widget and `$now` (§5.3).

## 9. Several nodes

TesserKIT devices are TesserAPI nodes, so the multi-node features are TesserAPI's and TesserUI's:

- **UI host:** a TesserKIT device with the NowTp service (§5.11) or UdpApi (§5.18) mounts the nodes nearby under `/peers` and advertises itself. The UI shows a page per node, forwarded over ESP-NOW or UDP (TesserUI §9.5); a device both reach is one node with two routes, in the order Peers sets (§5.19).
- **NowTP-only nodes** run TesserAPI and NowTP without TesserKIT's Wi-Fi services, or with a subset: `System` and `Firmware` work over any transport, uploads and the core dump's download included (in chunks, TesserAPI §10.1.1), and through a gateway (§5.4).
- **Memory on a gateway without PSRAM:** a gateway there runs the `classic` profile (§11, Profiles), without Bluetooth: about 48 KB more free. With every service, a classic ESP32 running `examples/idf/full` has about 27–31 KB free, its largest block 22–24 KB. The device's peers' trees and replies pass through that, so a request too large for it is answered `busy` ("not enough memory for the response") rather than crashing the gateway (TesserAPI §15), and TesserUI tries again later. Measured on DEVKIT-V3 as the gateway to DEVKIT-R1 (2026-10-13), from the Mac: two streamed subscriptions to R1's whole tree held open, then 40 whole-tree reads (4.8 KB) and 40 full schema reads (30 KB) through V3, three times: every tree read answered, every schema read and both subscriptions `busy`, no restart on either board; V3's lowest free heap since boot 2.9 KB, R1's 12 KB. Before the TesserAPI and NowTP fixes of that day, the same load aborted V3 or R1 within a minute (`new` failing with no exceptions: a failed allocation aborts on ESP-IDF, `new (std::nothrow)` included).
- **Fleet operations** are writes through the gateway to each peer, or to all of them with a `*` (`/peers/*/sequencer/play`, TesserAPI's fan-out, §14): update every peer's firmware (§5.4), set a value on all of them, trigger a sequence (§5.13). A view (TesserUI §8) with `each("/peers", ...)` shows a fleet dashboard.
- **Synchronized actions** (spin-seq's "start together") need a shared clock: NowTP's network time (§5.11) lets a gateway say "start at T" to every peer, instead of re-broadcasting with retries; the application schedules T with `espNow.localTimeUs(T)`.

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
| `SpinSequencerService` `/rest/spin-seq` + UDP JSON router (port 9600) | `/controller`, `/sequencer`, `/io`, `/welcome`; the UDP router is replaced by TesserAPI over NowTP and over UDP (UdpApi, §5.18), without compatibility with spin-seq's format |
| `JsonSeq`: an array of free-form JSON commands (`p` parameters, `c` motor command, `d` duration, `n` next, `t` trigger, `b` broadcast, `w` remote writes, `id` deduplication) | the Sequencer service (§5.13): steps that are writes to the tree (`/controller` values and actions), `wait` and `ms` for `d`, `next` by name, index or offset, `wait: none` for `t`, writes to `/peers` for `b` and `w`; NowTP's reliable delivery replaces `id` |
| `bootCommandIndex`, `isRunning` | `/sequencer/boot` *(p)*; a sequence isn't resumed at boot unless `boot` says so |
| A single command run for `d` ms, then the motor stopped | `execute`, with `finally` stopping the motor |
| Remotes list from mDNS, `ESPNowService`, `UdpSenderService` | the master is a UI host: NowTP's and UDP's discovery mount the remotes under `/peers`; a step's broadcast (`b`, `bu`) is `/sys/udp/send` with an empty `to` (§5.18); the console and stats come from NowTP's diagnostics |
| Master page fanning out over HTTP from the browser | a view with `each("/peers", "$/sequencer/status")` and actions on the master that write to every peer |
| Fleet OTA via GitHub | `/sys/firmware` on each peer, driven through the gateway (§5.4) |
| `JsonTimer` daily timers | `/sys/timers` with `path` `/sequencer/play` |
| Config export and import | `/sys/backup` |
| Front-panel button and LED | the application's own; `dev.onState()` for the LED's network states |

Commands broadcast to every remote (`b`) become a master's step writing to `/peers/*/sequencer/play`, and later a scheduled start with NowTP's shared clock (§9).

## 11. Footprint

Budgets, measured on a classic ESP32 (ESP-IDF, same method as TesserAPI §15: an example against a baseline build). CI enforces the room left in the app partition (§13: every release's image leaves at least 16 KB, with a warning under 5%); the budgets below are measured by hand *(a CI check of them is later)*:

| Item | Budget |
|---|---|
| Flash: Device + System + Network + Time + Firmware + Backup, above TesserAPI + TesserUI | ≤ 60 KB (mDNS and the DNS server included; ESP-IDF's Wi-Fi stack excluded, it is in every baseline) |
| Flash: the UI | TesserUI's `ui.js`, ≤ 65 KB gzipped (Leo raised it from 60 KB on 2026-10-14; the launcher's `launcher.js` isn't served by devices), with its widgets' chunk ≤ 70 KB (Leo, 2026-10-22; TesserUI dde641f: 54 KB and 67 KB) |
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
| Ethernet on a W5500 (2026-10-16, ESP32-S3, `examples/idf/full` with and without `-DETHERNET_W5500`, ESP-IDF 6.1-dev): `esp_eth`, the SPI master driver, `espressif/w5500` and `wiznet_common`, its netif glue | 54.0 KB |
| Log, Sleep and Home Assistant discovery together (2026-10-13, `examples/idf/full` before and after) | 28.7 KB |
| Battery on an ADC pin (2026-10-13, `examples/idf/full` with and without `-DBATTERY_GPIO=34`): the service, the ADC oneshot driver and its calibration | 12.1 KB |
| UdpApi with TesserAPI's UDP transport (2026-10-13, `examples/idf/full` before and after; ESP32) | 18.1 KB; heap: 11 KB while running (its two tasks' stacks and buffers), measured on the Gledopto |
| Home Assistant discovery (2026-10-13, ESP32-C3, `CONFIG_TESSERKIT_HOME_ASSISTANT=n` against on; `-DTESSERKIT_NO_HOME_ASSISTANT` elsewhere) | 7.9 KB |

- **Where it goes** (with mDNS): TesserKIT's own code 42 KB (Network 15 KB, most of it the `std::function`s behind bound values and the keyed list), ESP-IDF's `mdns` 34 KB, libc's time zone support 17 KB (`tzset()` parses with `vfscanf`), lwIP's DHCP server, DNS client and sockets 7.6 KB, the access point in the Wi-Fi driver 6 KB, OTA 2.6 KB.
- **The ESP32-C3's full example:** RISC-V code is larger, and `examples/idf/full` reached 99.7% of the files table's 1.75 MB slot on the C3 before Log, Sleep and discovery (4.5, 14.8 and 7.9 KB there), which took it over with ESP-IDF 5.5 and 6.1 alike. It leaves Bluetooth out on the C3 (`sdkconfig.defaults.esp32c3`): 1.63 MB, 11% free.
- **Against the budget:** 60 KB is exceeded. Features stay, as options where they are large (mDNS, downloads); the rest is smaller only at the cost of clarity, and is left as it is until a measured application needs the space.
- **Flash nothing uses** (2026-10-22, ESP32; NowTP 0.6 had just taken `full` under CI's 16 KB gate): the full example turns off the soft-AP's WPA3 (SAE) and Enhanced Open (OWE) support (`CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=n`, `CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_SOFTAP=n`), 7.6 KB: Network's access point is open or WPA2, and the station keeps WPA3; and NimBLE logs its errors only (`CONFIG_BT_NIMBLE_LOG_LEVEL_ERROR`), 8.9 KB: its info lines were compiled in whatever the console's level. Applications can do the same.
- **The full example's room:** with every service it outgrew its 1.75 MB slot's 16 KB margin on the classic ESP32 and the ESP32-S3 (CI, 2026-10-15: 14.9 and 14.0 KB free). Its sdkconfig makes assertions silent (`CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT`), which saves about 59 KB: a failed assert still panics with a backtrace and a core dump, whose address gives the file and line, but the console no longer prints them. Applications choose for themselves.
- **Arduino:** the Minimal sketch with every first-release service is 1.18 MB on ESP32 and ESP32-C3, within the default table's 1.25 MB slot (1.21 MB on ESP32 as of 2026-10-11, with that day's TesserAPI and TesserUI; 1.24 MB with ArtNet). With Mqtt it is 1.36 MB on ESP32: Arduino's prebuilt esp-mqtt can't drop its transports, so such a sketch needs a larger app slot (`min_spiffs.csv`, or TesserKIT's tables). As of 2026-10-12 (TesserAPI 0a44fdf, TesserUI 612d892) the Minimal sketch is 1.31 MB, 99.9% of the slot; with Files it is 1.38 MB, so a sketch with Files needs `min_spiffs.csv` (whose 128 KB `spiffs` partition Files then uses) or `tesserkit_4mb_files.csv`.

**Ethernet on hardware** (2026-10-13, a Gledopto GL-C-611WL on a router, `examples/idf/full -DETHERNET_BOARD=gledopto`, ESP-IDF 6.1-dev): a firmware upload of 1.7 MB at 170 KB/s (Wi-Fi: 70 KB/s), a file upload of 300 KB at 129 KB/s. Heap at rest 30 to 32 KB free (45 KB without Ethernet, so Ethernet takes about 18 KB: its DMA buffers, 10 + 10 of 512 B, its receive task and lwIP's interface), 13.9 KB at the lowest during the file upload. The receive task (`emac_rx`) used 1 KB of its stack through that upload, an Art-Net flood (165 frames a second on four universes) and 27 API reads a second, so TesserKIT gives it 2.5 KB rather than ESP-IDF's 4 KB (1.5 KB of headroom for its error logs): the same load then left 15.5 KB at the lowest.

**Files on hardware** (2026-10-12, DEVKIT-V3, `examples/idf/full`): uploads of 100 KB at about 40 KB/s (LittleFS writes; firmware uploads, which erase up front, go at 70 KB/s), downloads at about 110 KB/s.

**RAM with everything** (2026-10-11, `examples/idf/full` on a classic ESP32 without PSRAM, ESP-IDF 6.1-dev): 46 KB of heap free with Wi-Fi, HTTP, Bluetooth and NowTP running, 29 KB with an MQTT session too. Stacks used, measured with `vTaskList`: the HTTP server 3 KB, TesserKIT's task 2.4 KB, NowTP's transport 2.8 KB, esp-mqtt 1.5 KB. NowTP takes about 25 KB while running, an MQTT session 15 KB, and an HTTPS connection about 34 KB at its peak (a GitHub release check), its 8 KB task included.

- **HTTPS waits for memory:** a release check, an HTTPS download or an MQTT session over TLS (`mqtts://`, `wss://`) reserves `kTlsBytes` (36 KB) from TesserAPI's byte budget before it starts (`tesserkit::reserveTls()`, `tesser::reserveBytes()`, TesserAPI §15), and is refused `busy` (`not enough free memory for HTTPS now`; MQTT's `error` says it for TLS and tries again 10 s later) if the budget can't give them. The budget keeps its heap floor (8 KB on target) above them, so a session starts with about 44 KB free, as with the free-heap check before; and the library's own buffers, reserving from the same accountant, can't count on the same bytes at the same moment. The reservation isn't one block: TLS allocates in pieces. It is settled once the server answers (the heap then shows the session's buffers) and held while the session lasts, then given back. A handshake that runs the heap out would fail, or take the memory the rest of the firmware needs: such a device aborted on an allocation in TesserKIT's task. On a classic ESP32 without PSRAM, every service at once leaves no room for HTTPS while MQTT is connected; the ESP32-S3, or a board with PSRAM, has plenty.
- **PSRAM:** on a chip with it, TesserAPI's budget counts internal RAM and PSRAM apart (`tesser::Pool`, TesserAPI 2ceda41), and each reservation names the pool its buffer comes from: HTTPS from PSRAM when mbedTLS allocates there (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, the `psram` profile), a restore's parsed document from PSRAM (TesserAPI parses backups there), and every `std::string` or `malloc` buffer (the settings' text, a file read, the log's copy, a settings write) where malloc puts its size (`tesser::mallocPool()`). Without PSRAM every pool is internal RAM, as before.
- **Settings in memory:** Backup's download and stored backups reserve the persisted state's text from the same budget (one block of twice the last state's size, 4 KB before the first), a restore its parsed document (twice the text, in pieces), and a stored backup's restore its file's text (`Files::read()`, one block), each held while the buffer lives; refused, they answer `busy` ("not enough free memory ..."), and nothing changes.
- **Plain downloads wait for memory too:** a plain HTTP download reserves 16 KB the same way (`not enough free memory for a download now`, about 24 KB free with the floor): its task and the client's buffers took a Gledopto with every service and Ethernet (18 KB free) down to 268 bytes, one allocation from an abort (2026-10-14).
- **What the full example gives up for it:** mbedTLS allocates its buffers as it needs them and frees the certificates once verified, six Wi-Fi receive buffers rather than ten (a little less throughput under load), and NimBLE configured as a one-client peripheral (§5.10). The HTTP server's stack is 6 KB rather than 8 (3 KB used).
- **Flash:** `examples/idf/full` is 1.58 MB on ESP32 with Files (2026-10-13; 1.53 MB on 2026-10-12), 90% of `tesserkit_4mb_files.csv`'s 1.75 MB slot. TesserAPI's file transfers over every transport and through gateways, and its streamed HTTP subscriptions, took 26 KB of the growth.

**Profiles** (2026-10-21, Leo's choice; the `psram` profile and each profile's memory settings 2026-10-22). A classic ESP32 without PSRAM can't hold every service with the headroom a gateway, HTTPS and backups need, and the measurements put nearly all of the difference on Bluetooth: a Gledopto with Ethernet and every service had 12 KB free with Bluetooth and 60 KB without; as a gateway with Bluetooth it fell to 92 bytes and refused every transfer, and without it its lowest was 58 KB and every suite of `test/hil.py` passed (2026-10-16). So `examples/idf/full` comes in three profiles, chosen with `-DPROFILE=<name>`, which reads `profiles/<name>.defaults` (and `profiles/<name>.defaults.<target>`) after `sdkconfig.defaults`:

| Profile | Services | Memory settings | Released for | Image (local ESP-IDF 6.2, 2026-10-22) |
|---|---|---|---|---|
| `full` (the default) | every service and transport | silent assertions, six Wi-Fi receive buffers, TLS buffers allocated as needed (below) | ESP32, ESP32-C3 (without Bluetooth, which doesn't fit its slot there), ESP32-S3: `tesserkit-full-<target>` | ESP32: 1,784,272 bytes, 50 KB free in the files table's 1.75 MB slot |
| `classic` | every service but Bluetooth: a node or a gateway, on Wi-Fi or Ethernet | assertions with their file and line (about 59 KB of flash), ESP-IDF's ten Wi-Fi receive buffers (about 6.4 KB of heap) | ESP32: `tesserkit-classic-esp32`, and `tesserkit-classic-gledopto-esp32` with `-DETHERNET_BOARD=gledopto` | 1,675,936 bytes, 155 KB free (Bluetooth's controller and NimBLE: 162 KB of flash, about 48 KB of heap); with the Gledopto's Ethernet 1,714,976 bytes, 117 KB free |
| `psram` | every service and transport | allocations of 4 KB and more, Wi-Fi's and lwIP's buffers, TLS and NimBLE in PSRAM; assertions with their strings, ten receive buffers; `tesserkit_8mb_files.csv` (3 MB slots) | ESP32-S3 with 8 MB of flash and octal PSRAM (N8R8 modules): `tesserkit-psram-esp32s3` | 1,845,808 bytes, 1.24 MB free in the 3 MB slot |

- **The full set targets the ESP32-S3 or a board with PSRAM:** the `psram` profile. On a classic ESP32 `full` still runs, with about 12–30 KB free: enough for a node, not for a gateway under load, nor for HTTPS beside MQTT. `/sys/status/heap` and `minHeap` count internal RAM only (§5.1), so `test/hil.py`'s floors mean the same on every profile. *(later)* The `psram` profile on hardware (the XIAO ESP32-S3 has 8 MB of octal PSRAM). `CONFIG_SPIRAM_IGNORE_NOTFOUND` lets an image boot on a board without the PSRAM.
- **What the example runs is in Kconfig** (its `main/Kconfig.projbuild`, "TesserKIT full example" in `idf.py menuconfig`), so a profile is a configuration file: an option per service (Timers, Auth, Mqtt, ArtNet, Bluetooth, which needs NimBLE, NowTp, UdpApi, Peers, Sequencer, Log, Sleep, and Battery's pin), the Ethernet board (a choice, with a W5500's pins), the LED's pin and the profile's name (`CONFIG_FULL_PROFILE`, in the release asset's name). System, Network, Time, Firmware, Files and Backup are always in. The board options still come as `-DETHERNET_BOARD=`, `-DETHERNET_W5500=`, `-DLED_GPIO=` and `-DBATTERY_GPIO=`, which `cmake/example.cmake` writes into `<build>/args.defaults`, read after the profile. Defaults apply to a new sdkconfig only, so an existing sdkconfig that says otherwise stops the build (`… has "CONFIG_FULL_ETHERNET_GLEDOPTO=y", not CONFIG_FULL_ETHERNET_WT32_ETH01=y`) rather than build another variant than asked: one build directory per variant. `examples/idf/minimal` does the same with its options (`CONFIG_MINIMAL_DOWNLOADS`, `_GITHUB`, `_ARTNET`, `_MQTT`, or `-DWITH_<X>=1`); its test builds (`-DCRASH_AT_BOOT`, `-DNO_DEFAULT_NETWORK`, `-DFOOTPRINT_BASELINE`) stay compile definitions.
- **Selecting features, in general** (§1): an application chooses in its code. A service it doesn't instantiate isn't linked, nor a large option whose builder it doesn't call (`fw.downloads()`, `fw.github()`, `net.ethernet()`). What a service would link anyway is an option of TesserKIT's component, in Kconfig on ESP-IDF (`CONFIG_TESSERKIT_MDNS`, `CONFIG_TESSERKIT_HOME_ASSISTANT`) and as `-DTESSERKIT_NO_MDNS` and `-DTESSERKIT_NO_HOME_ASSISTANT` on Arduino and the host. ESP-IDF's own options (`CONFIG_BT_ENABLED`, `CONFIG_SPIRAM`) are read where they decide what can be built. The examples map their Kconfig onto that code.
- **Partition tables per profile:** `psram` uses `tesserkit_8mb_files.csv`; `full` and `classic` keep `tesserkit_4mb_files.csv`, since Files holds the settings there (§6.1) and the classic ESP32 boards have 4 MB. A table is only written over serial, so a profile's table applies to boards flashed with its factory image.
- **The asset a build updates from** (`firmware.github()`) is named after its profile and its Ethernet board: `tesserkit-<profile>[-<board>]-<target>.bin` (`-w5500` for a W5500 build, which no release has), so a board never installs an image without the Ethernet it was built for. Before profiles an Ethernet build updated from `tesserkit-full-*`, which has none.
- **Checked:** each released image under the release's 16 KB `min-free` gate (§13), the Ethernet variant included, which no release built before; `test/hil.py` takes a heap floor per build (`heapFloors`; `test/hil_boards.example.json` gives 32 KB to the classic and psram builds and keeps 8 KB for `full`), and a target per build (`targets`). On the boards (2026-10-22, `classic-gledopto` on a Gledopto as gateway and Ethernet board, on DHCP, and `classic` on DEVKIT-R1 as its peer, pinned siblings): device 37/37, files 39/39, ethernet 16/16, transfers 17/17, udp 24/24. With the profile's memory settings the gateway had 88–92 KB free at rest and 42.9 KB at its lowest (through relayed transfers), the peer 67 KB at its lowest; before them, 101 KB and 54.6 KB. The ten receive buffers cost about 10 KB at rest on both. The gateway keeps the default message limits (NowTP's and UDP's 16 KB): the floor holds with them.

**Partition table** (`partitions/tesserkit_4mb.csv`, recommended): `nvs` 20 KB, `otadata`, `ota_0` and `ota_1` 1.94 MB each, `coredump` 64 KB. No filesystem, no `phy_init` (the PHY data is in the app by default). `tesserkit_8mb.csv` has 3.94 MB slots. With Files: `tesserkit_4mb_files.csv` has 1.75 MB slots and a 384 KB `files` partition (LittleFS), `tesserkit_8mb_files.csv` 3 MB slots and 1.94 MB of files. Both keep `nvs` where the other tables have it, so the settings survive a change of table (flashed over serial: an OTA update can't change the table).

**Settings in NVS** (2026-10-15). The persisted state is one NVS blob per record (§6): `sys` (every service's settings), `sequencer`, and the application's. NVS writes a record's new copy before erasing the old one, and keeps one page free for garbage collection, so the largest record it can keep rewriting is far below the partition's size.

- **Bounded strings:** every persisted string has a length limit, so the worst case is known. TesserAPI bounds only `char[N]` values; a `std::string` took whatever a request carried (4 KB). The limits (`tesserkit/limits.h`), refused like any validator's: names and user names 32 characters (`limits::kName`: Peers' account and nodes' names and users, the access point's SSID); SNTP servers 64 (`kHost`); passwords and pairing codes 64 (`kSecret`: Peers', NowTP's pairing code, MQTT's user name, client ID and topic prefix); MQTT's password 128 (`kLongSecret`) and broker URI 128 (`kUri`); tree paths 96 (`kPath`: timers, the Sequencer's steps and `finally`, MQTT's published paths, Art-Net's mappings); JSON values 96 (`kValue`: timers, steps, `finally`); the Sequencer's `next` and `boot` 24; `/sys/auth/key` 64; MQTT's discovery prefix 32. A timer's last result (`last`, persisted) is cut to 48 bytes (`kResult`) on a character's boundary. Loading is lenient (TesserAPI §12): a stored value over its limit falls back to its default.
- **The worst case** (`test/record_cases.h`: every service at its limits, every string at its longest): `sys` 16,709 bytes (Network 1.8 KB with 5 networks, static addresses and BSSIDs; Timers 4.8 KB for 16; Auth 1.6 KB for 8 accounts; MQTT 1.5 KB; Art-Net 1.7 KB; Peers 4.6 KB for 16 nodes' settings), `sequencer` 10,288 bytes (32 steps). The test fails if a change makes either larger.
- **What NVS holds,** measured with ESP-IDF's own `nvs_flash` on the host (the linux target, 6.1-dev; the PHY's calibration, 1,904 bytes, and NowTP's key beside the records; 200 to 2,000 rewrites): the largest `sys` that can be rewritten without `ESP_ERR_NVS_NOT_ENOUGH_SPACE` is 6,720 bytes in TesserKIT's 20 KB partition with no other record, 5,632 beside a 2 KB `sequencer`, 4,640 beside 4 KB; 8,704 / 7,616 / 6,656 in 24 KB; 12,704 / 11,584 / 10,688 in 32 KB. Each 4 KB page adds about 2 KB. A record up to 13.5 KB is written once in 20 KB, and its next rewrite fails. On that ESP-IDF build a failed rewrite could also erase the stored copy (a cleanup erasing the wrong chunks, rewritten in ESP-IDF 5.5 and 6.0).
- **So the worst case doesn't fit:** it needs about 64 KB of NVS (`SettingsStore::fits()`), against 20 KB in every table. A typical device's `sys` is 1 to 2 KB. TesserKIT therefore refuses, rather than lets NVS fail: `SettingsStore`, in front of the default storage, writes a record only if it fits beside the others (`2 × record + others + 2 KB ≤ 95% of 4,000 bytes × (pages − 1)`, under every measurement above). A refused record isn't written, so the stored one stays and the change is lost at the next boot; `/sys/status/settings` says which and how large (`"sys" not saved: 16.3 KB, NVS can rewrite 6.4 KB beside the other settings`), and the error is logged once. It is tried again when the record next changes, not every 50 ms as TesserAPI retries a failed save (a gap noted for TesserAPI: back off after a failed save). The partition's size is read from the `nvs` partition with the default storage; `dev.settingsCapacity(bytes)` sets it, 0 for none.
- **On the filesystem** (§6.1), with Files: the worst case is saved (`record_worst_case_on_the_filesystem`), the guard doesn't apply, and Files keeps 64 KB free for the records (§5.12). This covers the full example's boards (`tesserkit_4mb_files.csv`) without a new partition table.
- *(later)* **A larger NVS partition for new boards without Files:** 64 KB would hold the worst case. The tables keep `nvs` at 0x9000 with `otadata` right after it and `ota_0` at 0x10000, so a larger `nvs` moves the app slots (or takes the room from `files`), and changes every table. A partition table is only written over serial (an OTA update can't change it), and a new table at another offset loses the stored settings, so it would apply to new boards and to boards flashed with a factory image. Leo's decision; meanwhile the limits and the refusal keep a full device safe.

**Comparison:** the same measurements are taken on ESP32-sveltekit's demo with matching features (Wi-Fi, AP, NTP, upload OTA, status), and reported in the README.

## 12. Testing

1. **Host tests** (CMake, ASan and UBSan, the same warnings as TesserAPI): every service against the fake platform. Wi-Fi state machine (join, lose, fall back to the AP, recovery), Ethernet (preferred to the station, the station's wait for it, recovery on a cable, a static address), time and DST, timers (catch-up, no double runs), firmware upload checks, release checks (versions, GitHub's answers and failures, scheduling), backup round trips, tokens (expiry, revocation, brute-force delay), the MQTT bridge (topics, commands with User access, sessions, intervals), Art-Net (scaling, gates, changes only, ArtPollReply's bytes), Bluetooth (the name following the hostname, switching off and on, the client shown), NowTp (waiting for the network, the nodes listed and read-only, pairing, the factory reset forgetting the key), HTTPS refused without the memory for it, Files (names and paths refused, uploads replaced whole and cut short, directories, space, formatting, the application's own writes), the stored backups (names, `keep`, `before-restore`, the factory reset), where the settings live (on the filesystem, the move from NVS and one failing midway, NVS without a mount, `/.settings` out of reach of `/sys/files`, kept by a format, the reserve, a factory reset, a save waiting for memory; a fake NVS, `fake.nvs`) and the Sequencer (each wait, names, indices and offsets, finally, take-overs, loops, failures, boot). The fake platform is a CMake target, `tesserkit_fake` (`fake_platform.h`, `fake::Platform::install(dev)`), so applications' host tests and simulators show `/sys` too.
   - **Chaos:** `fake::Chaos` (`fake.chaos.start(seed)`) makes the fake platform's events reach the device late (up to 3 s), out of order between sources (each source keeps its own order, as one task posts them), or not at all (10% lost, 30% late by default), under a seeded generator; the rigs' links carry their datagrams through it too (`carry()`). `test/chaos_cases.h` plays a random scenario per seed (the router dropping the station and going away, phones on the access point, the cable and its DHCP server coming and going, the broker accepting and dropping the session, links to a node going silent while it is read and written), calms the chaos, and checks that each service's state converged on the platform's: Network (Wi-Fi alone, and beside Ethernet), Mqtt, and Peers over UDP and NowTP with a sign-in. 40 seeds per case (10 for Peers) by default; `CHAOS_SEEDS`, `CHAOS_DROP` and `CHAOS_DELAY` raise them (300 seeds at 40% lost and 50% late pass).
   - **A short heap:** the test binary replaces the global `operator new` and `delete` (`test/alloc_cap.*`). While an `alloccap::Heap` of N bytes lives, what they hand out above what was live counts against N, TesserAPI's accountant sees what is left (its free-heap and largest-block probes, and an 8 KB floor), and an allocation past N, which aborts a classic ESP32, is recorded with its size and call stack for the case to fail on (`ALLOCCAP_ABORT=1` aborts instead). A fake's flash (the OTA slot) isn't counted (`fake::NotHeap`); ArduinoJson's documents, allocated with `malloc`, aren't either. `test/heap_cases.h` runs the client-reachable paths of Device, Backup, Files, Firmware and Auth at 96, 32 and 12 KB (`ALLOCCAP_BYTES` for another size), with the largest settings there are (§11): whole-tree reads and schemas, saves, backups downloaded, restored and stored, a 100 KB file up and down and 40 more listed, a GitHub release with 20 KB of notes and a firmware upload, eight accounts and sign-ins. Nothing went past the heap (2026-10-15): each large buffer is reserved, and refused `busy` when short. With Backup's reservation taken out, the 28 KB state's text goes past a 32 KB heap and the case fails.
2. **Golden schemas:** each service's schema with its hints, checked in as `fixtures/*.json`. TesserUI renders them in its golden-model tests, so a hint change shows as a diff in both repositories.
3. **On target:** `examples/idf/minimal` (and `examples/arduino/Minimal`, with `--app /lamp`), driven over HTTP and WebSocket by `test/e2e_device.py`: status, the interface, subscriptions, a scan, SNTP, sign-in with tokens over HTTP and WebSocket, a timer running, persistence across a restart, backup and restore, refused images, an OTA upload, its confirmation and rollback, and an image that crashes at boot (`-DCRASH_AT_BOOT=1`) rolled back by the bootloader, with its core dump downloaded. Provisioning: `test/captive_probe` is a firmware that plays a phone on a second board. It joins a fresh device's access point (built with `-DNO_DEFAULT_NETWORK=1`), checks that any name resolves to the device and that Android's and Apple's connectivity probes are redirected to the interface, then saves a network through the API and watches the device join it and the access point go. Releases: `test/e2e_github.py` serves a release endpoint and a newer image from the test machine, and watches the device find it, install it and come back up to date; `--check-only` checks the device's own feed (GitHub's API). A real GitHub download (2026-10-11, DEVKIT-V3: v0.1.2's asset through its ~950-character signed CDN redirect, 1.5 MB in 25 s, installed and booted): the mock server's short URLs had hidden that the HTTP client's request buffer was too small for it. Bluetooth: `examples/idf/bluetooth`, driven from a Mac with `bleak` (Nordic UART): an open device is Admin, a locked one public, sign-in with a line token gives Admin, a bad token public, and a reconnection starts public again. `examples/idf/full` (2026-10-11, DEVKIT-V3 beside DEVKIT-R1 running TesserAPI's test firmware): every service up, R1 found over NowTP and mounted under `/peers`, the envelopes on the serial line among the logs, a GitHub release check over HTTPS beside Bluetooth and NowTP, and with an MQTT session too (when it ran out of memory, which the guard of §11 now refuses). Files: `test/e2e_files.py` on `examples/idf/full` (2026-10-12, DEVKIT-V3, 39 checks): directories, a 100 KB upload and download compared byte for byte, a name with a space and accents (`filename*=UTF-8''`), refused names and sizes, replace, rename and delete, and stored backups saved, downloaded, restored and deleted. Two transfers on one path can't overlap over HTTP there: ESP-IDF's server handles one request at a time, so the second waits for the first (TesserAPI's host tests cover `busy`). Ethernet: `test/e2e_ethernet.py` on `examples/idf/full -DETHERNET_BOARD=gledopto` (2026-10-13, a Gledopto GL-C-611WL cabled to the router, 17 checks): the generic PHY driver with the LAN8720, link at 100 Mbit/s, a DHCP lease, the station kept away, `<hostname>.local` on Ethernet's address, a restart back over the cable in 13 s, a static address reached within 1 s and DHCP's lease back within 10 s. A script beside it (18 checks) switched the station to `always` (joined beside Ethernet, the device reached on both addresses), Ethernet off (the link down within 0.5 s, the device online over Wi-Fi), the station to `fallback` without Ethernet (it stays), Ethernet on (the lease back in 11 s, the station leaving at once) and the station off; every `state` read as specified. An ArtPoll over the cable got Ethernet's address and MAC; OTA updates installed and confirmed over the cable. A cable pulled by hand: the device unreachable for 21 s, then on Wi-Fi (the station's join, the Freebox's slow DHCP), Ethernet's state "no cable"; plugged back, "waiting for DHCP" while still on Wi-Fi, then the lease 9 s after the link and the station leaving at once. Not tried on hardware: a link-local address (needs a cable without DHCP). Found there: NowTP polled the station's connection every 10 ms, and ESP-IDF logs a warning for each poll while the station isn't connected, so a board on Ethernet with its station idle flooded its console (100 lines a second), which drowned the serial line's long replies; fixed in NowTP 65e8176 (§14). Transfers: `test/e2e_transfers.py` on `examples/idf/full` (2026-10-13, DEVKIT-V3 and DEVKIT-R1, 18 checks) moves the services' file nodes in envelopes: over the serial line, a file to Files and back, the settings downloaded and restored, the core dump downloaded, and a firmware update (the serial task writing the flash), the device coming back on the other slot from a software restart; through the gateway's WebSocket to the peer under `/peers`, a file and a firmware update, each chunk's reply timed against the relay's 3 s. A GitHub release check (§5.4) that falls during a relay takes the gateway's heap down to about 20 KB for a few seconds. Log, Sleep, Battery and Home Assistant discovery: `test/e2e_power.py` on `examples/idf/full` (2026-10-13, DEVKIT-V3, and DEVKIT-R1 built with `-DBATTERY_GPIO=34`, 40 checks): the recent lines downloaded without colours, a level above the build's refused, log lines streamed as events (`GET ?sub`) while an unresolvable broker fails; discovery against a broker on the test machine (amqtt), 13 configs (switch, number with the LED's step, sensors with units and an enum, `diagnostic` under `/sys`), commands as Home Assistant sends them, nothing dropped, every config emptied when switched off; a deep sleep of 8 s (back on the network 25 s after the request, `deep-sleep`, woken by `timer`, the LED parked by `onSleep`), and woken by the pin (GPIO 0 at `high`, which a devkit's pull-up holds); a battery voltage read and sampled each second (a floating GPIO 34: 0.28 V). UDP: `test/e2e_udp.py` on `examples/idf/full` (2026-10-13, 35 checks, three devices: DEVKIT-V3 and DEVKIT-R1 on the home Wi-Fi, the Gledopto GL-C-611WL on Ethernet with its station off): each shows the others under `/peers` (the devkits once each, though NowTP reaches them too), reads through `/peers` to either kind in about 105 ms, broadcasts from each device switching the others' LEDs (seen over HTTP within 155–360 ms) and not its own, a write to one node with its reply and its error, and a sequence played and stopped on every other node by broadcast. Three runs: 104 of 105 checks; one broadcast from DEVKIT-R1 didn't reach DEVKIT-V3 through the access point (both copies lost). The Gledopto's heap there: 21 KB free with UDP off, 10 KB with it on, 6 KB at the lowest, beside Ethernet, Bluetooth, NowTP and every other service. *(later)* a test firmware with every service and serial scripts.
4. **CI:** each push to main runs the host tests, then releases the example and publishes the launcher (§13). Beside them, the Arduino examples are built with Arduino-ESP32 3.3.7 against the pinned siblings (`arduino` in release.yml, 2026-10-22): Minimal on ESP32, ESP32-C3 and ESP32-S3, Bluetooth on the C3 and the S3, with `min_spiffs` (Minimal is 1.47 MB on ESP32, Bluetooth 1.64 MB on the C3, measured with PlatformIO's pioarduino 55.03.37). A failure there doesn't stop the release. *(later)* ESP-IDF builds on more versions (5.1, 5.4, latest), the footprint budgets.
4. **Hardware in the loop** (`test/hil.py`, boards in a JSON file like `test/hil_boards.example.json`): builds `examples/idf/full` in each board's build (a profile, or its Ethernet variant) against the sibling commits `deps.lock` pins, flashes each board after reading its MAC, then runs e2e_device, e2e_ethernet, e2e_transfers and e2e_udp, and after each suite reads every board's lowest free heap since boot and its reset reason. Its `secure` suite (`test/e2e_secure.py`) turns secure mode on with an admin account, checks the strict HTTP mode's refusals (415, 403, 421) and what it still accepts, the access point's `securePassword`, UDP's `auto` level and a revoked token, then removes the account (first run 2026-10-22 on DEVKIT-R1: 20/20). It fails on a failed suite, on a restart by panic, watchdog or brownout, or on a lowest heap under the build's floor (8 KB unless the boards file says otherwise, `heapFloors` by build): out-of-memory aborts are usually one allocation away from there. Run by hand for now; a nightly self-hosted runner is the plan (docs/review-2026-10-14-open.md, Roadmap › Phase 1). First run (2026-10-15, DEVKIT-V3, DEVKIT-R1 and a Gledopto): device 37/37, ethernet 16/16, transfers 15/18 (a serial firmware chunk arriving as bad base64, TesserAPI's), udp 31/34; V3's lowest heap 6.5 KB during the transfers.

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
- Versions follow semver, and every push to main that passes CI is a release (below). The manifests (`library.json`, `library.properties`, `idf_component.yml`) aren't edited for releases: the release stamps its version into them (`scripts/version.py`, which also checks they agree) in the commit its tag points at, so a tag's files say its version; main's keep the last series raised by hand (`0.2.0`), a floor. TesserAPI and TesserUI aren't listed in `idf_component.yml`: the component manager would fetch them beside the copies a project adds through `EXTRA_COMPONENT_DIRS`, and build both. **Siblings are pinned to commits:** `deps.lock` lists the TesserAPI, TesserUI and NowTP commits TesserKIT builds against (`owner/repo@sha`), and everything follows it: the host build fetches that TesserAPI when there is no checkout beside it, `library.json`'s dependencies carry the same commits (`…TesserAPI.git#<sha>`), and CI builds the releases, the launcher and the bridge from them. `scripts/pin.sh` sets it from the siblings' checkouts (refusing a commit GitHub doesn't have) and rewrites `library.json` to match; `scripts/pin.sh --check` fails when they disagree, which CI runs. A change that needs a newer sibling commits the new pin with it. A developer's builds use the checkouts beside the repository, whatever their commit, and CMake warns when one isn't the pinned commit.
- **Releases:** `.github/workflows/firmware.yml` is a reusable workflow for ESP-IDF projects. It computes the next version, builds each target in `espressif/idf`'s container (`idf-version`, `release-v5.5` by default) with `-DPROJECT_VER=<version>` (which becomes `/sys/firmware/version`), and publishes a release tagged `v<version>` with notes generated from the commits and, per target, `<name>-<target>.bin` (what `github()` installs) and `<name>-<target>-factory.bin` (bootloader, partition table and app merged, flashed at 0x0 on a new board). The version is the last `vX.Y.Z` tag with its patch raised; a commit since that tag with a `Release: minor` or `Release: major` trailer in its message (`git log v<last>..HEAD --format='%(trailers:key=Release,valueonly)'`, any case) raises the minor or the major instead, the largest one winning, so a trailer in a push whose CI failed still counts at the next release; and `version-command`'s output wins when it is higher (a first release, or a jump). `stamp-command` runs with the version in `VERSION` on top of the pushed commit; the job commits what it changed ("Release vX.Y.Z", by `github-actions[bot]`) on a detached head, tags that commit and pushes only the tag, so the branch never gets a commit from the bot, which matters with several sessions pushing to it. Without changes, or without a stamp command, the tag is on the pushed commit. The images are built from the pushed commit. A rerun on a released commit keeps its version: its tag is on it or on its release commit. Without `project` or `projects` the release has no images: TesserAPI and NowTP release this way, with the same version, stamp, tag and notes (2026-10-22, the stack's release model, Leo). A run whose commit is no longer the branch's head when its images are built releases nothing, with a notice: the newer run releases its own commit (and the token couldn't tag the older one if the newer commit changed a workflow file). `dependencies` checks out repositories next to the project, where a developer's siblings are (`owner/repo`, `owner/repo@ref`), and `dependencies-file` lists more of them the same way (`deps.lock`); each ref is resolved to a commit once, so every target builds the same commits, and the release's notes begin with them ("Built with: leokeba/TesserAPI `390dd57`…"), as does the run's summary. The workflow's `dependencies` output gives them as `{"owner/repo": "sha"}`. Each image's room in its app partition goes into the summary, with a warning under 5% free; under `min-free` bytes (TesserKIT: 16 KB, room for the next update's growth) the release fails. Several firmwares go into one release with `projects`, a JSON list of `{project, name, build-args, targets}`, instead of `project`, `name` and `build-args`; an entry's `targets` replaces the `targets` input for it. TesserKIT's `release.yml` calls it on each push to main, after the host tests, with `stamp-command: python3 scripts/version.py "$VERSION"`, for `examples/idf/minimal` with `-DWITH_GITHUB=1` (`tesserkit-minimal-*`) and `examples/idf/full` (`tesserkit-full-*`, NowTP checked out beside) on ESP32, ESP32-C3 and ESP32-S3, and for the full example's `classic` profile on ESP32 (`tesserkit-classic-esp32`, and `tesserkit-classic-gledopto-esp32` with the Gledopto's Ethernet) and its `psram` profile on the ESP32-S3 (`tesserkit-psram-esp32s3`; §11, Profiles); applications call it for their own firmware (`uses: leokeba/TesserKIT/.github/workflows/firmware.yml@main`). `[skip ci]` in a commit message skips the release.
- **Launcher:** `release.yml` also publishes the launcher (§8) to GitHub Pages after the host tests: TesserUI's bundle, built from the TesserUI commit `deps.lock` pins, as the firmware's is, with `pages/index.html`, which makes it a launcher titled TesserKIT (`window.TESSER = {launcher: true, title: "TesserKIT", firmware, bridge}`). It waits for the release, then copies the latest three releases' factory images next to the page (TesserUI's `web/scripts/firmware.mjs`, about 9 MB a release), and TesserUI's flasher with them: the launcher installs the full or minimal example on a board over USB, then sets up its Wi-Fi through `/sys/network/wifi` (TesserUI §9.7.1). It deploys even when the release failed, with the releases there are.
- **The bridge:** after the release, `release.yml` calls TesserUI's reusable `bridge.yml` (`page: pages/index.html`, `name: tesserkit-bridge`), which compiles TesserUI's bridge from the pinned TesserUI commit with that page, the bundle and the flasher built in, and attaches `tesserkit-bridge-macos-arm64.zip` and `-x64.zip` to the release. It serves the launcher at `http://localhost:8765/` with the releases' images fetched from GitHub (TesserUI §9.7). The launcher links to the latest release's, which lack them until that job is done.

## 14. Follow-ups wanted in other layers

**TesserAPI** (phase 7 of its roadmap; each with its section in TesserAPI's DESIGN.md). Done as of 2026-10-11, or on the date given, except what is marked *(later)*.

| Item | For |
|---|---|
| Access levels: `tesser::Access`, `Client::access`, `readAccess()`/`writeAccess()` on nodes, filtered reads, `api.authenticate(fn)` for tokens | §5.7, §7 |
| Secret values: `.secret()`, never rendered | passwords (§5.2, §5.7, §5.8) |
| Typed object arguments for actions, and typed deferred actions | `login({name, password})`, helionext's `add`, `download(url)` |
| Keyed lists: `.key("name")` | networks, users, timers, helionext's sources |
| Scalar arrays | calibration tables, spin-seq's 128 encoder offsets |
| File nodes: streamed upload and download over HTTP | firmware upload, backups, core dumps |
| File nodes over the message transports (serial, Bluetooth, WebSocket, NowTP), in base64 chunks (TesserAPI f3289b8, 2026-10-13) | the same, from the launcher over USB or Bluetooth, and NowTP-only nodes (§9) |
| Streamed subscriptions over plain HTTP (TesserAPI 20d403e, 2026-10-12) | toasts (§8) on the launcher over HTTPS |
| File transfers relayed through gateways to remote nodes (TesserAPI f204291, becd177, 2026-10-13) | firmware updates, backups and core dumps of NowTP-only peers through a gateway (§5.4) |
| Streamed replies on WebSocket and serial | large schemas without paging |
| `view=hash` | TesserUI's schema cache for directly reached nodes |
| Automatic re-advertisement when the tree changes | gateways with mounted peers |
| `Storage::erase()`, `Api::restore()` | factory reset, backups |
| A UDP transport: the envelopes in datagrams on the LAN, discovery, broadcast sets, a least level for peers with tokens in `auth` envelopes, a device mounted once whatever the transport (TesserAPI ffa9d3d, 2026-10-13) | UdpApi (§5.18) |
| Routes: one remote node per device with a route per transport, a route order (default and per node), failover, declared devices, state hooks, ordered streamed sets, peers lost across transports (TesserAPI 02c3bf4, by tesserapi-3d, 2026-10-13, for ballet-photophonique) | Peers (§5.19) |
| Tokens as a client: a remote node's token presented on every route, again when the peer forgets it, and the level granted reported (TesserAPI 05190c8, 2026-10-13) | Peers' sign-in (§5.19) |
| `Object::remote(std::string name, ...)`: a remote node declared at run time keeps a copy of its name (TesserAPI 1348199, 2026-10-13) | Peers' declared devices (§5.19) |
| UDP with its receive task but no worker (`Tasks::Receive`), and hooks that `begin()` and `end()` leave in place (TesserAPI 6336093, 2026-10-13) | transports in the device's task (§4.2) |
| UDP routes per interface, route orders naming them (`udp/ethernet`), UDP kept to chosen interfaces (TesserAPI 1cf73f9, 2026-10-13) | Peers' order (§5.19), UdpApi's `interfaces` (§5.18) |
| A node's own device id at the root of its schema and in its mDNS record (TesserAPI b852626, 2026-10-13) | TesserUI's tabs: one per device, however it is reached |
| No abort on a short or fragmented heap: replies, relayed bodies, copies and parses of large messages answer `busy` (or are dropped like a lost datagram) unless the largest block holds them and an 8 KB floor stays free; gateways relay a remote's body raw, ask a remote only for what their subscribers need, and admit a remote subscription only above twice the floor; NowTP's reassembled buffers are handed over, not copied (TesserAPI dba7c1f, 1afb23c, 49dd339, 66afc3d, 8d88c9f, 5842ba2, 9519967, 2026-10-13) | gateways without PSRAM (§9) |
| *(later)* `Api::owner()` (an event's first `emit()`) locks every other `Api` while the caller holds its own, which the host lock-order checker aborts on in two-device rigs (`peers_cases.h`) | host tests with several devices |

Done 2026-10-12, for Files (TesserAPI 0a44fdf): file nodes in list elements transfer (`/sys/files/entries/<name>/content`), and an upload carries the client's file name (`?filename=`, `FileRequest::filename`).

Done 2026-10-12, for the Sequencer (§5.13), in TesserAPI 9c07af2 (its §6.5): a `*` path segment in set, matching every child of an object or element of a list or array, the set applied to each match (remote nodes forwarded in parallel, offline ones failing at once, matches without the rest of the path skipped), and a reply with each match's result, or `"<n> of <total> failed: <name>: <message>"` and each failure.

Done as of 2026-10-11 too, for Bluetooth and NowTp: an `auth` envelope presenting a token on a line, an unauthenticated line or untrusted NowTP peer taking the level of the empty token, `BleTransport` controlled at runtime (`start()`, `stop()`, `setName()`, `peerAddress()`), read-only lists.

**NowTP** (done in NowTP d39e0d4, 2026-10-11; NowTp uses the first three; message buffers and acknowledgements from malloc, so a short heap drops rather than aborts, and reassembled buffers handed to the receiver, bb20ac0, 8320a40, adb543b, 2026-10-13):

| Item | For |
|---|---|
| Channel following | Network changing networks (§5.2) |
| Authenticated pairing, persistent peers | trusting peers without trusting MACs (§7) |
| A shared clock (offset estimated from pings) | synchronized starts (§9) |
| Streaming of large messages | no longer needed for files: TesserAPI moves them in chunks that fit a message (§5.4) |
| Following the station's connection from Wi-Fi's events, not by polling `esp_wifi_sta_get_ap_info()` each tick (`channelFixed()`), which ESP-IDF answers with a logged warning while the station isn't connected (NowTP 65e8176, 2026-10-13; checked on the Gledopto: no warnings with the station idle or after a disconnect, the channel still fixed while associated) | a board on Ethernet with its station idle (§5.2), whose console it flooded |

**TesserUI** (done as of 2026-10-10, or on the date given, except the custom elements and what is marked *(later)*):

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
| The events subscription over plain HTTP, streamed (TesserUI 2bfebb6, 2026-10-12), for the launcher on HTTPS | §8 |
| File widgets over serial and Bluetooth (and a serial port through the bridge), with TesserAPI's `upload` and `download` envelopes; HTTP wherever there is one (TesserUI 4e54d75, 2026-10-13) | §5.4, §5.5, §5.12 from the launcher with no network |
| Files below a remote node in WebSocket envelopes, so a relay never holds the gateway's HTTP task (TesserUI d8934b6, 2026-10-13) | §5.4 fleet updates |
| `ui.nodeName` (the root's label read again after a rename) and `ui.perNode` (left out of apply to all); a peer's tab label read again with each advertised hash (TesserUI 42f58bd, 2026-10-21) | §4.4 |

## 15. Roadmap

| Phase | Content | Acceptance |
|---|---|---|
| 0 | TesserAPI follow-ups of §14 | Host-tested; the HTTP and WebSocket parts checked on hardware. |
| 1 *(done)* | Device, System, Network, Time, Firmware (upload, download, rollback), Backup; the TesserUI widgets they need | A fresh classic ESP32 is provisioned from a phone through the captive portal, updated from the browser, rolled back after a crashing image, backed up and restored. Footprint within §11, compared with ESP32-sveltekit. |
| 2 *(done)* | Timers, Auth | spin-seq's timers and a locked-down device. |
| 3 | helionext on TesserKIT, in tiny-heliostat (§10.1) | Every helionext page usable, with custom elements only for the keyframes and the sky. |
| 4 | spin-seq on TesserKIT, NowTP channel following and pairing, shared clock | A master and three remotes run a synchronized sequence; fleet update through the master. |
| 5 *(done)* | Battery, Sleep, Ethernet (RMII), Log, Home Assistant discovery | Host-tested, and on hardware (§12). Ethernet on SPI stays *(later)* (§5.17). |

Mqtt and ArtNet were built ahead of the roadmap (2026-10-11), ArtNet for tiny-heliostat, and so were GitHub releases with the workflow that publishes them, and the Bluetooth and NowTp services with `examples/idf/full` (2026-10-11): phase 4's NowTP part, short of the synchronized sequence. Ethernet on the internal MAC came on 2026-10-12, for a Gledopto LED controller.

## 16. Decisions

- **A library rather than a template:** ESP32-sveltekit's forks (helionext, spin-seq) carry the framework's code and diverge from upstream. A library is updated by version.
- **Services under `/sys`, generic UI:** the alternative, a hand-written page per service, is what makes ESP32-sveltekit's interface 240 KB and every service cost a page.
- **NVS without a filesystem, the filesystem when there is one:** a typical persisted state is small (1 to 2 KB), NVS levels wear, and the flash a filesystem partition would take goes to the app slots instead, so a device without Files needs no partition for its settings. But NVS's 20 KB can't rewrite the worst case (§11), so a device with Files keeps them on its LittleFS partition (Leo, 2026-10-15), moved there by an OTA update without a new partition table (§6.1).
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
