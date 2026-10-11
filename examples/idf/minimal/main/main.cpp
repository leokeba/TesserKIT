// A TesserKIT device with ESP-IDF: Wi-Fi with a captive portal, time,
// firmware updates, backups, timers, accounts and status under /sys, next to a small
// application tree. Flash it, join the "tesserkit-demo-xxxx" network from a
// phone, and the interface opens; add your Wi-Fi network there.
//
// For development, a gitignored main/secrets.h may define WIFI_SSID and
// WIFI_PASSWORD (the factory default network), WIFI_TX_POWER (dBm) for
// boards with a weak supply, MQTT_URI (a broker, with -DWITH_MQTT=1), and
// GITHUB_REPO and GITHUB_ASSET (another release feed, with -DWITH_GITHUB=1).
#include "TesserKIT.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The releases .github/workflows/release.yml publishes for each push to main.
#if !defined(GITHUB_REPO)
#define GITHUB_REPO "leokeba/TesserKIT"
#define GITHUB_ASSET "tesserkit-minimal-{target}.bin"
#endif

#if FOOTPRINT_BASELINE
#include "esp_wifi.h"
#include "nvs_flash.h"
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

#if FOOTPRINT_BASELINE
// The same application on TesserAPI and TesserUI alone, Wi-Fi joined the
// plain way: what TesserKIT's footprint is measured against.
tesser::Api api;
tesser::HttpServer http(api);
tesserui::WebUi web;
}  // namespace

extern "C" void app_main() {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    wifi_config_t config = {};
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD) && !NO_DEFAULT_NETWORK
    strncpy(reinterpret_cast<char*>(config.sta.ssid), WIFI_SSID, sizeof(config.sta.ssid));
    strncpy(reinterpret_cast<char*>(config.sta.password), WIFI_PASSWORD, sizeof(config.sta.password));
#endif
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &config);
    esp_wifi_start();
    esp_wifi_connect();
    static tesser::NvsStorage storage("tesserkit");
    api.mount("led", led).label("LED");
    api.persistence(storage);
    api.load();
    http.enableWebSocket("/ws");
    http.begin(80, "/api");
    web.serve(http.handle());
    api.startTask();
}
#else
tesserkit::Device dev("TesserKIT Demo");
tesserkit::System sys(dev);
tesserkit::Network net(dev);
tesserkit::Time timekeeper(dev);
tesserkit::Firmware firmware(dev);
tesserkit::Backup backup(dev);
tesserkit::Timers timers(dev);
tesserkit::Auth auth(dev);  // open until an admin account exists
#if WITH_ARTNET
tesserkit::ArtNet artnet(dev);
#endif
#if WITH_MQTT
tesserkit::Mqtt mqtt(dev);
#endif

}  // namespace

extern "C" void app_main() {
#if CRASH_AT_BOOT
    // A broken update: the bootloader must go back to the previous image.
    vTaskDelay(pdMS_TO_TICKS(2000));
    *static_cast<volatile int*>(nullptr) = 1;
#endif
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD) && !NO_DEFAULT_NETWORK
    net.defaultNetwork(WIFI_SSID, WIFI_PASSWORD);
#endif
#if defined(WIFI_TX_POWER)
    net.txPower(WIFI_TX_POWER);
#endif
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
#if WITH_GITHUB
    firmware.github(GITHUB_REPO, GITHUB_ASSET);  // and from a URL, as downloads()
#elif WITH_DOWNLOADS
    firmware.downloads();  // updates from a URL: TLS and an HTTP client, about 130 KB
#endif
#if WITH_ARTNET
    // Universe 0: channel 1 is the brightness, channel 2 above half turns it on.
    tesserkit::ArtNetMapping brightness;
    brightness.path = "/led/brightness";
    brightness.coarse = 1;
    brightness.max = 255;
    tesserkit::ArtNetMapping on;
    on.path = "/led/on";
    on.coarse = 2;
    on.threshold = 128;
    artnet.map(brightness).map(on);
#endif
#if WITH_MQTT
    mqtt.publish("/led", 0).publish("/sys/status", 10000);
#if defined(MQTT_URI)
    mqtt.broker(MQTT_URI).enable(true);
#endif
#endif
    dev.api().mount("led", led).label("LED");
    dev.begin();
}
#endif
