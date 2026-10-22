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
// What it runs is in Kconfig (main/Kconfig.projbuild, idf.py menuconfig), so a
// profile is a configuration file: -DPROFILE=classic leaves Bluetooth out, for
// a classic ESP32 without PSRAM (profiles/, DESIGN.md 11). The board's options
// also come as -D, written into the build's defaults (../CMakeLists.txt):
// -DBATTERY_GPIO=n adds the Battery service on that ADC pin, through a divider
// of two equal resistors; -DETHERNET_BOARD=gledopto uses that board's
// Ethernet; -DETHERNET_W5500="sck,miso,mosi,cs,irq" a W5500 module on those
// GPIOs; -DLED_GPIO=n the LED's pin.
#include "TesserKIT.h"
#include "driver/ledc.h"
#include "sdkconfig.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {

// The LED: CONFIG_FULL_LED_GPIO, by default GPIO 2, the blue LED of most
// ESP32 devkits, dimmed with PWM. Each write applies at once, whoever makes it
// (the interface, a timer, Art-Net, MQTT, a node nearby, the settings
// restored at boot).
constexpr int kLedGpio = CONFIG_FULL_LED_GPIO;

// The release asset this build updates from: tesserkit-<profile>-<target>.bin,
// with the Ethernet board after the profile (tesserkit-classic-gledopto-esp32.bin),
// so a board never installs an image without its Ethernet.
#if defined(CONFIG_FULL_ETHERNET_NONE)
#define RELEASE_BOARD ""
#else
#define RELEASE_BOARD "-" CONFIG_FULL_ETHERNET_BOARD
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
        channel.gpio_num = kLedGpio;
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
#if CONFIG_FULL_TIMERS
tesserkit::Timers timers(dev);        // daily schedules
#endif
#if CONFIG_FULL_AUTH
tesserkit::Auth auth(dev);            // accounts and sign-in
#endif
#if CONFIG_FULL_MQTT
tesserkit::Mqtt mqtt(dev);            // a broker bridge
#endif
#if CONFIG_FULL_ARTNET
tesserkit::ArtNet artnet(dev);        // lighting consoles
#endif
#if CONFIG_FULL_BLUETOOTH  // not on the ESP32-C3 (sdkconfig.defaults.esp32c3), nor in the classic profile
tesserkit::Bluetooth bluetooth(dev);  // the launcher from a phone
#endif
#if CONFIG_FULL_NOWTP
tesserkit::NowTp espNow(dev);         // the nodes nearby over ESP-NOW (after Network: Wi-Fi first)
#endif
#if CONFIG_FULL_UDP
tesserkit::UdpApi udp(dev);           // the nodes on the LAN over UDP, and writes to all at once
#endif
#if CONFIG_FULL_PEERS
tesserkit::Peers peers(dev);          // how the nodes are reached: UDP, NowTP, in which order
#endif
#if CONFIG_FULL_SEQUENCER
tesserkit::Sequencer sequencer(dev);  // /sequencer: steps played in turn
#endif
#if CONFIG_FULL_LOG
tesserkit::Log logs(dev);             // the firmware's log, as events and a file
#endif
#if CONFIG_FULL_SLEEP
tesserkit::Sleep sleeper(dev);        // deep sleep, woken by a timer or the BOOT button
#endif
#if CONFIG_FULL_BATTERY_GPIO >= 0
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
    // Ethernet, preferred while it has an address.
#if CONFIG_FULL_ETHERNET_GLEDOPTO
    net.ethernet(tesserkit::boards::gledopto());
#elif CONFIG_FULL_ETHERNET_WT32_ETH01
    net.ethernet(tesserkit::boards::wt32Eth01());
#elif CONFIG_FULL_ETHERNET_OLIMEX_ESP32_POE
    net.ethernet(tesserkit::boards::olimexEsp32Poe());
#elif CONFIG_FULL_ETHERNET_ESP32_ETHERNET_KIT
    net.ethernet(tesserkit::boards::esp32EthernetKit());
#elif CONFIG_FULL_ETHERNET_W5500
    net.ethernet(tesserkit::boards::w5500(CONFIG_FULL_W5500_SCK, CONFIG_FULL_W5500_MISO, CONFIG_FULL_W5500_MOSI,
                                          CONFIG_FULL_W5500_CS, CONFIG_FULL_W5500_IRQ, CONFIG_FULL_W5500_RESET));
#endif
    net.apMode(tesserkit::ApMode::Fallback);  // its own network while no other is joined
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3").defaultServer("pool.ntp.org");
    // Updates from this repository's releases, and from any URL.
    firmware.github("leokeba/TesserKIT", "tesserkit-" CONFIG_FULL_PROFILE RELEASE_BOARD "-{target}.bin");
#if CONFIG_FULL_AUTH && defined(ADMIN_PASSWORD)
    auth.defaultAdmin("admin", ADMIN_PASSWORD);
#endif
#if CONFIG_FULL_TIMERS
    tesserkit::Timer evening;  // the LED on at dusk on weekdays
    snprintf(evening.name, sizeof(evening.name), "%s", "evening");
    snprintf(evening.time, sizeof(evening.time), "%s", "19:30");
    evening.days = 0x1F;
    evening.path = "/led/on";
    evening.value = "true";
    timers.add(evening);
#endif
#if CONFIG_FULL_SEQUENCER
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
#endif
    // Writes to every node nearby (/peers/*/...) each wait in a pending slot.
    dev.api().config().maxPending = 8;
#if CONFIG_FULL_MQTT
    mqtt.tls().publish("/led", 0).publish("/sys/status", 10000).homeAssistant(true);
#if defined(MQTT_URI)
    mqtt.broker(MQTT_URI).enable(true);
#endif
#endif
#if CONFIG_FULL_ARTNET
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
#endif
#if CONFIG_FULL_NOWTP && defined(NOWTP_PAIRING_CODE)
    espNow.pairingCode(NOWTP_PAIRING_CODE);
#endif
#if CONFIG_FULL_SLEEP
    sleeper.wakePin(0);  // the BOOT button, low when pressed
#endif
#if CONFIG_FULL_BATTERY_GPIO >= 0
    battery.adc(CONFIG_FULL_BATTERY_GPIO, 2.0f);
#endif

    // The application: the LED, and a dashboard over it and the nodes nearby
    // ("$" stands for each node under /peers).
    led.begin();
#if CONFIG_FULL_SLEEP
    dev.onSleep([] {
        led.on = false;  // dark while asleep
        led.apply();
    });
#endif
    dev.api().mount("led", led).label("LED").ui("icon", "bulb");
    static tesserui::Views views(dev.api());
    auto& home = views.page("overview", "Overview").icon("home");
    home.card("LED").node("/led");
    home.card("Device").node("/sys/status/state").node("/sys/status/heap").node("/sys/network/wifi/status/rssi");
#if CONFIG_FULL_NOWTP || CONFIG_FULL_UDP
    home.card("Nodes nearby").each("/peers", "$/led");
#endif

    dev.cors("https://leokeba.github.io");  // the launcher, reaching the device by its address
    dev.begin();
    serialApi.begin();
}
