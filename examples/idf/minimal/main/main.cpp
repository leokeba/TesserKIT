// A TesserKIT device with ESP-IDF: Wi-Fi with a captive portal, time,
// firmware updates, backups, timers, accounts and status under /sys, next to a small
// application tree. Flash it, join the "tesserkit-demo-xxxx" network from a
// phone, and the interface opens; add your Wi-Fi network there. Or reach it
// from the launcher (https://leokeba.github.io/TesserKIT/): over USB, with no
// network at all, by its address, or listed under Nearby by TesserUI's bridge.
//
// For development, a gitignored main/secrets.h may define WIFI_SSID and
// WIFI_PASSWORD (the factory default network), WIFI_TX_POWER (dBm) for
// boards with a weak supply, MQTT_URI (a broker, with -DWITH_MQTT=1), and
// GITHUB_REPO and GITHUB_ASSET (another release feed, with -DWITH_GITHUB=1).
#include "TesserKIT.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#include "driver/ledc.h"
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

// The LED: LED_GPIO (-DLED_GPIO=n), by default GPIO 2, the blue LED of most
// ESP32 devkits, dimmed with PWM. Each write applies at once, whoever makes it
// (the interface, a timer, Art-Net, MQTT, the settings restored at boot).
#if !defined(LED_GPIO)
#define LED_GPIO 2
#endif

struct Led {
    bool on = false;
    int brightness = 128;

    void begin() {
        ledc_timer_config_t timer = {};
        timer.speed_mode = LEDC_LOW_SPEED_MODE;
        timer.duty_resolution = LEDC_TIMER_8_BIT;
        timer.timer_num = LEDC_TIMER_0;
        timer.freq_hz = 5000;
        timer.clk_cfg = LEDC_AUTO_CLK;
        ledc_timer_config(&timer);
        ledc_channel_config_t channel = {};
        channel.gpio_num = LED_GPIO;
        channel.speed_mode = LEDC_LOW_SPEED_MODE;
        channel.channel = LEDC_CHANNEL_0;
        channel.timer_sel = LEDC_TIMER_0;
        ledc_channel_config(&channel);
        apply();
    }
    void apply() const {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? static_cast<uint32_t>(brightness) : 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
};

void describe(tesser::Object& o, Led& led) {
    o.value("on", [&led] { return led.on; }, [&led](bool on) { led.on = on; led.apply(); }).persist();
    o.value("brightness", [&led] { return led.brightness; }, [&led](int b) { led.brightness = b; led.apply(); })
        .range(0, 255)
        .step(5)
        .persist();
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
    led.begin();
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
// The launcher's USB way: envelopes on the console's UART, one per line, with
// Admin access (physical access). Logs stay at warnings, so that few land in
// the middle of a reply (sdkconfig.defaults).
tesser::UartTransport serialApi(dev.api(), UART_NUM_0);
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
    led.begin();
    dev.api().mount("led", led).label("LED").ui("icon", "bulb");
    dev.cors("https://leokeba.github.io");  // the launcher, reaching the device by its address
    dev.begin();
    serialApi.begin();
}
#endif
