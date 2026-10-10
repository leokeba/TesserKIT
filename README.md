# TesserKIT

A device framework for ESP32 firmware with a rich web interface and a small footprint. It provides what every connected device needs besides its own function, network setup, time, firmware updates, backups, users, MQTT and system status, as subtrees of a [TesserAPI](https://github.com/leokeba/TesserAPI) tree, drawn by [TesserUI](https://github.com/leokeba/TesserUI)'s generic renderer with no UI code.

> **Status: design.** The specification is [docs/DESIGN.md](docs/DESIGN.md). There is no code yet.

```cpp
#include <TesserKIT.h>

tesserkit::Device dev("Heliostat");
tesserkit::System sys(dev);       // /sys/status, restart, factory reset
tesserkit::Network net(dev);      // Wi-Fi, access point, captive portal, mDNS
tesserkit::Time clock(dev);       // SNTP, time zone, set from the browser
tesserkit::Firmware fw(dev);      // upload, update from a URL, rollback
tesserkit::Backup backup(dev);    // download and restore the settings

void setup() {
    describe(dev.api().object("heliostat"), heliostat);   // the application's own tree
    dev.begin();
}
void loop() {}
```

It aims at most of [ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit)'s functionality: a library rather than a template, every service a subtree reachable over HTTP, WebSocket, serial, ESP-NOW and MQTT, the whole interface within a 50 KB bundle, and settings in NVS with no filesystem partition.

## License

MIT
