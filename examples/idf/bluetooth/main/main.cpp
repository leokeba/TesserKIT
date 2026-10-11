// A TesserKIT device reachable over Bluetooth LE as well as Wi-Fi, with
// ESP-IDF: the minimal example's services and LED, plus TesserAPI's
// BleTransport (NimBLE, the Nordic UART Service). Open the launcher
// (https://leokeba.github.io/TesserKIT/) in Chrome on a phone or a laptop,
// choose Bluetooth, then "tesserkit-bt-xxxx": the interface comes from the
// launcher, the tree over Bluetooth, with no Wi-Fi network in common. The
// device's own page, USB and the network work as in the minimal example.
//
// Anyone in range can connect, so a Bluetooth client is treated like a
// network client: Admin while the device is open (no admin account yet), then
// public until it signs in (docs/DESIGN.md section 5.7).
//
// For development, a gitignored main/secrets.h may define WIFI_SSID and
// WIFI_PASSWORD (the factory default network) and WIFI_TX_POWER (dBm).
#include "TesserKIT.h"
#include "tesser/transports/ble_transport.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {

struct Led {
    bool on = false;
    int brightness = 128;
};

void describe(tesser::Object& o, Led& led) {
    o.value("on", led.on).persist();
    o.value("brightness", led.brightness).range(0, 255).step(5).persist();
}

Led led;

tesserkit::Device dev("TesserKIT BT");
tesserkit::System sys(dev);
tesserkit::Network net(dev);
tesserkit::Time timekeeper(dev);
tesserkit::Firmware firmware(dev);
tesserkit::Backup backup(dev);
tesserkit::Timers timers(dev);
tesserkit::Auth auth(dev);  // open until an admin account exists
tesser::UartTransport serialApi(dev.api(), UART_NUM_0);  // USB, as in the minimal example
tesser::BleTransport ble(dev.api());

}  // namespace

extern "C" void app_main() {
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    net.defaultNetwork(WIFI_SSID, WIFI_PASSWORD);
#endif
#if defined(WIFI_TX_POWER)
    net.txPower(WIFI_TX_POWER);
#endif
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
    dev.api().mount("led", led).label("LED").ui("icon", "bulb");
    dev.cors("https://leokeba.github.io");  // the launcher, reaching the device by its address
    dev.begin();                             // NVS too, which NimBLE needs
    serialApi.begin();
    ble.begin(dev.hostname().c_str());  // the name the browser's chooser shows
}
