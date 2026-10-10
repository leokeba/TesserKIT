// A TesserKIT device on Arduino: Wi-Fi with a captive portal, time, firmware
// updates, backups and status under /sys, next to the application's tree.
// Flash it, join the "minimal-xxxx" network from a phone: the interface opens.
// Add your Wi-Fi network there, then find the device at http://minimal-xxxx.local/.
#include <TesserKIT.h>

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

void setup() {
    Serial.begin(115200);
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
    dev.api().mount("lamp", lamp).label("Lamp");
    dev.onState([](tesserkit::State s) { Serial.printf("state: %s\n", tesserkit::toString(s)); });
    dev.begin();
}

void loop() {
    analogWrite(LED_BUILTIN, lamp.on ? lamp.brightness : 0);
    delay(20);
}
