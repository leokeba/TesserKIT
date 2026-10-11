// A TesserKIT device on Arduino reachable over Bluetooth LE as well as Wi-Fi:
// the Minimal sketch plus TesserAPI's BleTransport (the Nordic UART Service).
// Open the launcher (https://leokeba.github.io/TesserKIT/) in Chrome on a
// phone or a laptop, choose Bluetooth, then "bluetooth-xxxx": the interface
// comes from the launcher, the tree over Bluetooth, with no Wi-Fi network in
// common. The device's own page, USB and the network work as in Minimal.
//
// Anyone in range can connect, so a Bluetooth client is treated like a
// network client: Admin while the device is open (no admin account yet), then
// public until it signs in (TesserKIT's docs/DESIGN.md section 5.7).
//
// Boards: ESP32-C3 and ESP32-S3 (about 1.5 MB). On a classic ESP32, Arduino's
// Bluetooth stack (Bluedroid, with classic Bluetooth) fills a whole 1.94 MB
// slot, leaving no room for an application: use examples/idf/bluetooth there,
// whose NimBLE adds 200 KB.
//
// Partitions: too large for the default app slots. Use TesserKIT's
// partitions/tesserkit_4mb.csv (two 1.94 MB slots, so updates still work):
// copy it next to the sketch as partitions.csv, or in PlatformIO set
// board_build.partitions to it.
#include <TesserKIT.h>
#include <tesser/transports/ble_transport.h>

#if __has_include("secrets.h")
#include "secrets.h"  // gitignored: WIFI_SSID, WIFI_PASSWORD for development
#endif

// The board's LED: LED_BUILTIN, BUILTIN_LED on older cores, and neither on
// the generic ESP32 variant (esp32dev), whose devkits have it on GPIO 2.
#if defined(LED_BUILTIN)
#define LAMP_PIN LED_BUILTIN
#elif defined(BUILTIN_LED)
#define LAMP_PIN BUILTIN_LED
#else
#define LAMP_PIN 2
#endif

struct Lamp {
    bool on = false;
    int brightness = 128;
} lamp;

void describe(tesser::Object& o, Lamp& l) {
    o.value("on", l.on).persist();
    o.value("brightness", l.brightness).range(0, 255).step(5).persist();
}

tesserkit::Device dev("Bluetooth");
tesserkit::System sys(dev);
tesserkit::Network net(dev);
tesserkit::Time timekeeper(dev);
tesserkit::Firmware firmware(dev);
tesserkit::Backup backup(dev);
tesserkit::Timers timers(dev);
tesserkit::Auth auth(dev);  // open until an admin account exists
tesser::StreamTransport serialApi(dev.api(), Serial);  // USB, as in Minimal
tesser::BleTransport ble(dev.api());

void setup() {
    Serial.begin(115200);
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    net.defaultNetwork(WIFI_SSID, WIFI_PASSWORD);
#endif
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
    dev.api().mount("lamp", lamp).label("Lamp").ui("icon", "bulb");
    dev.cors("https://leokeba.github.io");  // the launcher, reaching the device by its address
    dev.begin();
    ble.begin(dev.hostname().c_str());  // the name the browser's chooser shows
}

void loop() {
    serialApi.poll();
    ble.poll();  // requests from Bluetooth, handled here
    analogWrite(LAMP_PIN, lamp.on ? lamp.brightness : 0);
    delay(5);
}
