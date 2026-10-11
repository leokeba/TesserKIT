// A TesserKIT device reachable over Bluetooth LE as well as Wi-Fi, with
// ESP-IDF: the minimal example's services and LED, plus the Bluetooth service
// (TesserAPI's BleTransport on NimBLE, the Nordic UART Service). Open the launcher
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
#include "driver/ledc.h"

#if __has_include("secrets.h")
#include "secrets.h"
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

tesserkit::Device dev("TesserKIT BT");
tesserkit::System sys(dev);
tesserkit::Network net(dev);
tesserkit::Time timekeeper(dev);
tesserkit::Firmware firmware(dev);
tesserkit::Backup backup(dev);
tesserkit::Timers timers(dev);
tesserkit::Auth auth(dev);  // open until an admin account exists
tesserkit::Bluetooth bluetooth(dev);  // /sys/bluetooth: on or off, its name, who is connected
tesser::UartTransport serialApi(dev.api(), UART_NUM_0);  // USB, as in the minimal example

}  // namespace

extern "C" void app_main() {
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    net.defaultNetwork(WIFI_SSID, WIFI_PASSWORD);
#endif
#if defined(WIFI_TX_POWER)
    net.txPower(WIFI_TX_POWER);
#endif
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
    led.begin();
    dev.api().mount("led", led).label("LED").ui("icon", "bulb");
    dev.cors("https://leokeba.github.io");  // the launcher, reaching the device by its address
    dev.begin();  // Bluetooth too, under the hostname: what the browser's chooser shows
    serialApi.begin();
}
