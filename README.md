# TesserKIT

A device framework for ESP32 firmware with a rich web interface and a small footprint. It provides what every connected device needs besides its own function, network setup, time, firmware updates, backups, users, MQTT, Art-Net, Bluetooth, ESP-NOW and system status, as subtrees of a [TesserAPI](https://github.com/leokeba/TesserAPI) tree, drawn by [TesserUI](https://github.com/leokeba/TesserUI)'s generic renderer with no UI code.

> **Status: phase 2, with parts of 4.** Device, System, Network, Time, Firmware, Backup, Timers, Auth, Mqtt, ArtNet, Bluetooth and NowTp work, on the host (121 tests) and on hardware (classic ESP32, ESP-IDF and Arduino, `test/e2e_device.py`). Porting helionext is under way in its own repository. The specification is [docs/DESIGN.md](docs/DESIGN.md).

```cpp
#include <TesserKIT.h>

tesserkit::Device dev("Heliostat");
tesserkit::System sys(dev);           // /sys/status, restart, factory reset, core dump
tesserkit::Network net(dev);          // Wi-Fi, access point, captive portal, mDNS
tesserkit::Time timekeeper(dev);      // SNTP, time zone, set from the browser
tesserkit::Firmware firmware(dev);    // upload, rollback; .downloads() for URLs, .github() for releases
tesserkit::Backup backup(dev);        // download and restore the settings
tesserkit::Timers timers(dev);        // daily schedules over the whole tree
tesserkit::Auth auth(dev);            // accounts, roles, sign-in
tesserkit::Bluetooth bluetooth(dev);  // the interface from a phone, over Bluetooth LE
tesserkit::NowTp nowtp(dev);          // the nodes nearby over ESP-NOW, each with its page

void setup() {
    dev.api().mount("heliostat", heliostat);   // the application's own tree
    dev.begin();
}
void loop() {}
```

A fresh device opens an access point named after it (`heliostat-a67c`) with a captive portal: join it from a phone and the interface opens. Add a Wi-Fi network there; once joined, the device is at `http://heliostat-a67c.local/`.

Or open the launcher, [leokeba.github.io/TesserKIT](https://leokeba.github.io/TesserKIT/), in Chrome: it reaches a device over USB or Bluetooth with no network at all, by its address, or among those [TesserUI's bridge](https://github.com/leokeba/TesserUI/tree/main/bridge) finds nearby.

It aims at most of [ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit)'s functionality: a library rather than a template, every service a subtree reachable over HTTP, WebSocket, serial, ESP-NOW and MQTT, the whole interface within a 50 KB bundle, and settings in NVS with no filesystem partition.

## Services

| Service | Path | What it does |
|---|---|---|
| `System` | `/sys/status`, `/sys/restart`, ... | Chip, heap (charted), uptime, reset reason; restart, factory reset, core dump download |
| `Network` | `/sys/network` | Saved networks (a keyed list, passwords never readable), scan, station state machine with backoff, access point (always, when offline, never), captive portal, mDNS |
| `Time` | `/sys/time` | SNTP, POSIX time zone (the interface picks it from IANA names), set from the browser's clock, external sources (GPS, RTC) |
| `Firmware` | `/sys/firmware` | Upload with progress, checked for the chip before anything is written; download from a URL and updates from GitHub releases, checked daily on the device (opt-in); confirmation after a minute online, rollback |
| `Backup` | `/sys/backup` | The persisted state as JSON, secrets included (Admin only), restored leniently across firmware versions |
| `Timers` | `/sys/timers` | Daily schedules on chosen weekdays: set any value or call any action of the tree at a local time |
| `Auth` | `/sys/auth` | Accounts with user and admin roles, salted PBKDF2 hashes, stateless signed tokens, sign-in from TesserUI's dialog |
| `Mqtt` | `/sys/mqtt` | A broker bridge: chosen subtrees published as retained topics, one per leaf, with intervals; `<prefix>/<path>/set` commands with user access; online/offline availability; TLS with the certificate bundle (opt-in) |
| `Bluetooth` | `/sys/bluetooth` | TesserAPI's Bluetooth LE transport, for TesserUI's launcher on a phone: on and off at runtime, named after the hostname, the client shown; clients sign in with a token |
| `NowTp` | `/sys/nowtp`, `/peers` | NowTP over ESP-NOW: the TesserAPI nodes nearby mounted under `/peers`, each with its page (a UI host), and this device on theirs; pairing with an installation key, whose holders are trusted; channel following, network time, the nodes heard |
| `ArtNet` | `/sys/artnet` | An Art-Net node: DMX channels of one universe (8 or 16 bits, scaled, or gates) written to any nodes, only when they change; answers ArtPoll |

Without the Auth service, or until it holds an admin account, the device is open: every client is Admin.

## Footprint

On a classic ESP32 with ESP-IDF and `-Os`, the five services add 113 KB of flash above TesserAPI and TesserUI, 78 KB without mDNS (`CONFIG_TESSERKIT_MDNS=n`, or `-DTESSERKIT_NO_MDNS`). Firmware downloads from URLs add TLS and an HTTP client, about 130 KB, only with `firmware.downloads()`; GitHub releases 10 KB more, only with `firmware.github()`. ArtNet adds 9 KB; Mqtt about 110 KB (esp-mqtt with its transports), more than Arduino's default 1.25 MB app slot holds next to every other service on ESP32. Bluetooth adds about 200 KB on ESP-IDF; every service and transport together (`examples/idf/full`) is 1.47 MB. Details in [DESIGN §11](docs/DESIGN.md#11-footprint).

## Building

- **ESP-IDF:** `examples/idf/minimal` (it finds TesserAPI and TesserUI next to this repository, or through `TESSERAPI_PATH` and `TESSERUI_PATH`). Its `sdkconfig.defaults` selects `partitions/tesserkit_4mb.csv`, rollback and core dumps.
- **Arduino / PlatformIO:** `examples/arduino/Minimal`, with TesserAPI, TesserUI and ArduinoJson 7.
- **Bluetooth:** `examples/idf/bluetooth` and `examples/arduino/Bluetooth` (ESP32-C3, ESP32-S3) add the Bluetooth service beside Wi-Fi: the launcher reaches the device from a phone with no network in common.
- **Everything:** `examples/idf/full` has every service and transport, with the factory defaults each one takes, and a dashboard; it needs NowTP next to TesserKIT (or `NOWTP_PATH`). Two boards running it show each other. On a classic ESP32 without PSRAM it is at the edge of the RAM: HTTPS waits while an MQTT session runs (DESIGN §11).
- **Host tests:** `cmake -S . -B build && cmake --build build -j && ctest --test-dir build`.
- **On hardware:** `python3 test/e2e_device.py http://<ip> [--image app.bin] [--crash-image crash.bin]`; ArtNet and Mqtt: the example built with `-DWITH_ARTNET=1 -DWITH_MQTT=1`, then `uv run --with paho-mqtt python test/e2e_services.py http://<ip> --broker <host>`; GitHub releases: `python3 test/e2e_github.py http://<ip> --image newer.bin --tag v9.9.9` (instructions in the script).

## Releases

Each push to main runs the host tests, then [`release.yml`](.github/workflows/release.yml) builds the minimal and full examples for ESP32, ESP32-C3 and ESP32-S3 and publishes a release under the next patch version: `tesserkit-minimal-<target>.bin` and `tesserkit-full-<target>.bin` for updates, `...-<target>-factory.bin` to flash a new board at 0x0. Devices running the example built with `-DWITH_GITHUB=1` find it and offer it under Firmware. The same run publishes the launcher above to GitHub Pages, with the TesserUI the release is built with. For a minor or major release, raise the version in `library.json`; `[skip ci]` in a commit message skips the release.

Applications release their own firmware with the same workflow:

```yaml
on:
  push:
    branches: [main]
jobs:
  firmware:
    permissions:
      contents: write
    uses: leokeba/TesserKIT/.github/workflows/firmware.yml@main
    with:
      project: firmware                  # the ESP-IDF project
      name: heliostat                    # assets: heliostat-esp32.bin, ...
      targets: '["esp32"]'
      dependencies: |                    # checked out next to the repository
        leokeba/TesserAPI
        leokeba/TesserUI
        leokeba/TesserKIT
```

and `firmware.github("owner/repo", "heliostat-{target}.bin")` in the firmware.

## License

MIT
