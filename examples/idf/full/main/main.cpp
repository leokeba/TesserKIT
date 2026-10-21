// Every TesserKIT service and transport in one firmware, with ESP-IDF, and
// the factory defaults each one takes. A starting point to strip down: the
// minimal example is the other end.
//
// Reach it from the launcher (https://leokeba.github.io/TesserKIT/): over USB,
// Bluetooth, the network or the bridge; from its own page; over MQTT and
// Art-Net; and from the nodes nearby over ESP-NOW and the nodes on the LAN over
// UDP, which it shows under Nodes nearby (a UI host). Two boards running it
// show each other, and /sys/udp/send writes to every one of them at once.
//
// A gitignored main/secrets.h may define:
//   WIFI_SSID, WIFI_PASSWORD  the factory default network
//   WIFI_TX_POWER             dBm, for boards with a weak supply
//   MQTT_URI                  a broker, enabled from the start
//   ADMIN_PASSWORD            an "admin" account from the start (else the
//                             device is open until one is added)
//   NOWTP_PAIRING_CODE        what pairings between nodes must know
//
// Build options: -DPROFILE=classic leaves Bluetooth out, for a classic ESP32
// without PSRAM (profiles/, DESIGN.md 11); -DBATTERY_GPIO=n adds the Battery
// service on that ADC pin, through a divider of two equal resistors;
// -DETHERNET_BOARD=gledopto uses that board's Ethernet;
// -DETHERNET_W5500="sck,miso,mosi,cs,irq" a W5500 module on those GPIOs.
#include "TesserKIT.h"
#include "driver/ledc.h"
#include "sdkconfig.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {

// The LED: LED_GPIO (-DLED_GPIO=n), by default GPIO 2, the blue LED of most
// ESP32 devkits, dimmed with PWM. Each write applies at once, whoever makes it
// (the interface, a timer, Art-Net, MQTT, a node nearby, the settings
// restored at boot).
#if !defined(LED_GPIO)
#define LED_GPIO 2
#endif

// The release asset this build updates from: tesserkit-<profile>-<target>.bin,
// with the Ethernet board after the profile (tesserkit-classic-gledopto-esp32.bin),
// so a board never installs an image without its Ethernet.
#if !defined(PROFILE)
#define PROFILE "full"
#endif
#define TESSERKIT_STR2(x) #x
#define TESSERKIT_STR(x) TESSERKIT_STR2(x)
#if defined(ETHERNET_BOARD)
#define RELEASE_BOARD "-" TESSERKIT_STR(ETHERNET_BOARD)
#elif defined(ETHERNET_W5500)
#define RELEASE_BOARD "-w5500"
#else
#define RELEASE_BOARD ""
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

tesserkit::Device dev("TesserKIT");
tesserkit::System sys(dev);           // /sys/status, restart, factory reset, core dump
tesserkit::Network net(dev);          // Wi-Fi, Ethernet, access point, captive portal, mDNS
tesserkit::Time timekeeper(dev);      // SNTP, time zone
tesserkit::Firmware firmware(dev);    // upload, URL, GitHub releases, rollback
tesserkit::Files files(dev);          // LittleFS: stored backups, the application's files
tesserkit::Backup backup(dev);        // the settings as a file, and stored on the device
tesserkit::Timers timers(dev);        // daily schedules
tesserkit::Auth auth(dev);            // accounts and sign-in
tesserkit::Mqtt mqtt(dev);            // a broker bridge
tesserkit::ArtNet artnet(dev);        // lighting consoles
#if CONFIG_BT_ENABLED  // not on the ESP32-C3 (sdkconfig.defaults.esp32c3), nor in the classic profile
tesserkit::Bluetooth bluetooth(dev);  // the launcher from a phone
#endif
tesserkit::NowTp espNow(dev);         // the nodes nearby over ESP-NOW (after Network: Wi-Fi first)
tesserkit::UdpApi udp(dev);           // the nodes on the LAN over UDP, and writes to all at once
tesserkit::Peers peers(dev);          // how the nodes are reached: UDP, NowTP, in which order
tesserkit::Sequencer sequencer(dev);  // /sequencer: steps played in turn
tesserkit::Log logs(dev);             // the firmware's log, as events and a file
tesserkit::Sleep sleeper(dev);        // deep sleep, woken by a timer or the BOOT button
#if defined(BATTERY_GPIO)
tesserkit::Battery battery(dev);      // charge and voltage
#endif
// The launcher's USB way: envelopes on the console's UART, Admin (physical
// access). Logs stay at warnings (sdkconfig.defaults).
tesser::UartTransport serialApi(dev.api(), UART_NUM_0);

}  // namespace

extern "C" void app_main() {
    // Factory defaults: what a new device, or one reset, starts with.
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    net.defaultNetwork(WIFI_SSID, WIFI_PASSWORD);
#endif
#if defined(WIFI_TX_POWER)
    net.txPower(WIFI_TX_POWER);
#endif
#if defined(ETHERNET_BOARD)
    net.ethernet(tesserkit::boards::ETHERNET_BOARD());  // preferred while it has an address
#elif defined(ETHERNET_W5500)
    net.ethernet(tesserkit::boards::w5500(ETHERNET_W5500));
#endif
    net.apMode(tesserkit::ApMode::Fallback);  // its own network while no other is joined
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3").defaultServer("pool.ntp.org");
    // Updates from this repository's releases, and from any URL.
    firmware.github("leokeba/TesserKIT", "tesserkit-" PROFILE RELEASE_BOARD "-{target}.bin");
#if defined(ADMIN_PASSWORD)
    auth.defaultAdmin("admin", ADMIN_PASSWORD);
#endif
    tesserkit::Timer evening;  // the LED on at dusk on weekdays
    snprintf(evening.name, sizeof(evening.name), "%s", "evening");
    snprintf(evening.time, sizeof(evening.time), "%s", "19:30");
    evening.days = 0x1F;
    evening.path = "/led/on";
    evening.value = "true";
    timers.add(evening);
    // A blink: on and off every half second, until stopped (off then).
    for (bool lit : {true, false}) {
        tesserkit::Step s;
        snprintf(s.name, sizeof(s.name), "%s", lit ? "on" : "off");
        s.path = "/led/on";
        s.value = lit ? "true" : "false";
        s.wait = tesserkit::Wait::Time;
        s.ms = 500;
        s.next = lit ? "+1" : "on";
        sequencer.add(s);
    }
    sequencer.finally("/led/on", "false");
    // Writes to every node nearby (/peers/*/...) each wait in a pending slot.
    dev.api().config().maxPending = 8;
    mqtt.tls().publish("/led", 0).publish("/sys/status", 10000).homeAssistant(true);
#if defined(MQTT_URI)
    mqtt.broker(MQTT_URI).enable(true);
#endif
    // Universe 0: channel 1 dims the LED, channel 2 above half switches it on.
    tesserkit::ArtNetMapping brightness;
    brightness.path = "/led/brightness";
    brightness.coarse = 1;
    brightness.max = 255;
    tesserkit::ArtNetMapping on;
    on.path = "/led/on";
    on.coarse = 2;
    on.threshold = 128;
    artnet.universe(0).map(brightness).map(on);
#if defined(NOWTP_PAIRING_CODE)
    espNow.pairingCode(NOWTP_PAIRING_CODE);
#endif
    sleeper.wakePin(0);  // the BOOT button, low when pressed
#if defined(BATTERY_GPIO)
    battery.adc(BATTERY_GPIO, 2.0f);
#endif

    // The application: the LED, and a dashboard over it and the nodes nearby
    // ("$" stands for each node under /peers).
    led.begin();
    dev.onSleep([] {
        led.on = false;  // dark while asleep
        led.apply();
    });
    dev.api().mount("led", led).label("LED").ui("icon", "bulb");
    static tesserui::Views views(dev.api());
    auto& home = views.page("overview", "Overview").icon("home");
    home.card("LED").node("/led");
    home.card("Device").node("/sys/status/state").node("/sys/status/heap").node("/sys/network/wifi/status/rssi");
    home.card("Nodes nearby").each("/peers", "$/led");

    dev.cors("https://leokeba.github.io");  // the launcher, reaching the device by its address
    dev.begin();
    serialApi.begin();
}
