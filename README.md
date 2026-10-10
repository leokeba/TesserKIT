# TesserKIT

A device framework for ESP32 firmware with a rich web interface and a small footprint. It provides what every connected device needs besides its own function, network setup, time, firmware updates, backups, users, MQTT and system status, as subtrees of a [TesserAPI](https://github.com/leokeba/TesserAPI) tree, drawn by [TesserUI](https://github.com/leokeba/TesserUI)'s generic renderer with no UI code.

> **Status: phase 1.** Device, System, Network, Time, Firmware and Backup work, on the host (55 tests) and on hardware (classic ESP32, `test/e2e_device.py`). Timers, Auth and Mqtt come next. The specification is [docs/DESIGN.md](docs/DESIGN.md).

```cpp
#include <TesserKIT.h>

tesserkit::Device dev("Heliostat");
tesserkit::System sys(dev);           // /sys/status, restart, factory reset, core dump
tesserkit::Network net(dev);          // Wi-Fi, access point, captive portal, mDNS
tesserkit::Time timekeeper(dev);      // SNTP, time zone, set from the browser
tesserkit::Firmware firmware(dev);    // upload, rollback; firmware.downloads() for URLs
tesserkit::Backup backup(dev);        // download and restore the settings

void setup() {
    dev.api().mount("heliostat", heliostat);   // the application's own tree
    dev.begin();
}
void loop() {}
```

A fresh device opens an access point named after it (`heliostat-a67c`) with a captive portal: join it from a phone and the interface opens. Add a Wi-Fi network there; once joined, the device is at `http://heliostat-a67c.local/`.

It aims at most of [ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit)'s functionality: a library rather than a template, every service a subtree reachable over HTTP, WebSocket, serial, ESP-NOW and MQTT, the whole interface within a 50 KB bundle, and settings in NVS with no filesystem partition.

## Services

| Service | Path | What it does |
|---|---|---|
| `System` | `/sys/status`, `/sys/restart`, ... | Chip, heap (charted), uptime, reset reason; restart, factory reset, core dump download |
| `Network` | `/sys/network` | Saved networks (a keyed list, passwords never readable), scan, station state machine with backoff, access point (always, when offline, never), captive portal, mDNS |
| `Time` | `/sys/time` | SNTP, POSIX time zone (the interface picks it from IANA names), set from the browser's clock, external sources (GPS, RTC) |
| `Firmware` | `/sys/firmware` | Upload with progress, checked for the chip before anything is written; download from a URL (opt-in); confirmation after a minute online, rollback |
| `Backup` | `/sys/backup` | The persisted state as JSON, secrets included (Admin only), restored leniently across firmware versions |

Without the Auth service (phase 2) the device is open: every client is Admin.

## Footprint

On a classic ESP32 with ESP-IDF and `-Os`, the five services add 113 KB of flash above TesserAPI and TesserUI, 78 KB without mDNS (`CONFIG_TESSERKIT_MDNS=n`, or `-DTESSERKIT_NO_MDNS`). Firmware downloads from URLs add TLS and an HTTP client, about 130 KB, only with `firmware.downloads()`. Details in [DESIGN §11](docs/DESIGN.md#11-footprint).

## Building

- **ESP-IDF:** `examples/idf/minimal` (it finds TesserAPI and TesserUI next to this repository, or through `TESSERAPI_PATH` and `TESSERUI_PATH`). Its `sdkconfig.defaults` selects `partitions/tesserkit_4mb.csv`, rollback and core dumps.
- **Arduino / PlatformIO:** `examples/arduino/Minimal`, with TesserAPI, TesserUI and ArduinoJson 7.
- **Host tests:** `cmake -S . -B build && cmake --build build -j && ctest --test-dir build`.
- **On hardware:** `python3 test/e2e_device.py http://<ip> [--image app.bin] [--crash-image crash.bin]`.

## License

MIT
