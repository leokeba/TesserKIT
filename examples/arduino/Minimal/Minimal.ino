// A TesserKIT device on Arduino: Wi-Fi with a captive portal, time, firmware
// updates, backups and status under /sys, next to the application's tree.
// Flash it, join the "minimal-xxxx" network from a phone: the interface opens.
// Add your Wi-Fi network there, then find the device at http://minimal-xxxx.local/.
// Or reach it from the launcher (https://leokeba.github.io/TesserKIT/): over
// USB, by its address, or listed under Nearby by TesserUI's bridge.
#include <TesserKIT.h>

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

tesserkit::Device dev("Minimal");
tesserkit::System sys(dev);
tesserkit::Network net(dev);
tesserkit::Time timekeeper(dev);
tesserkit::Firmware firmware(dev);
tesserkit::Backup backup(dev);
tesserkit::Timers timers(dev);
tesserkit::Auth auth(dev);  // open until an admin account exists
// The launcher's USB way: envelopes on Serial, one per line, with Admin access
// (physical access). It skips other lines, such as the state printed below.
tesser::StreamTransport serialApi(dev.api(), Serial);

void setup() {
    Serial.begin(115200);
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    net.defaultNetwork(WIFI_SSID, WIFI_PASSWORD);
#endif
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
    dev.api().mount("lamp", lamp).label("Lamp").ui("icon", "bulb");
    dev.cors("https://leokeba.github.io");  // the launcher, reaching the device by its address
    dev.onState([](tesserkit::State s) { Serial.printf("state: %s\n", tesserkit::toString(s)); });
    dev.begin();
}

void loop() {
    serialApi.poll();
    analogWrite(LAMP_PIN, lamp.on ? lamp.brightness : 0);
    delay(20);
}
