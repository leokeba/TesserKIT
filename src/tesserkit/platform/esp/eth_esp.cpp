// Ethernet and esp_netif on ESP-IDF's esp_eth (which Arduino-ESP32 3.x
// ships): a PHY on the chip's internal MAC (RMII), or a W5500 module on SPI.
// Linked only by applications that declare a board (Network::ethernet()).
#if defined(ESP_PLATFORM)

#include "esp_idf_version.h"
#include "sdkconfig.h"
#include "tesserkit/platform.h"

// The W5500's driver left ESP-IDF in 6.0 for the esp-eth-drivers components
// (TesserKIT's idf_component.yml adds espressif/w5500 there, on chips without
// the internal MAC); before, it is ESP-IDF's, with CONFIG_ETH_SPI_ETHERNET_W5500.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#if __has_include("esp_eth_mac_w5500.h")
#define TESSERKIT_W5500 1
#endif
#elif CONFIG_ETH_SPI_ETHERNET_W5500
#define TESSERKIT_W5500 1
#endif

#if CONFIG_ETH_USE_ESP32_EMAC || TESSERKIT_W5500

#include "tesser/platform.h"
#include "tesserkit/platform/esp/esp_internal.h"

#include <string>

#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if TESSERKIT_W5500
#include "driver/spi_master.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#endif
#endif

// The named PHY drivers left ESP-IDF in 6.0 (for the esp-eth-drivers
// components); the generic one, which drives any IEEE 802.3 PHY, came in 5.4.
#define TESSERKIT_NAMED_PHYS (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0))
#define TESSERKIT_GENERIC_PHY (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0))

namespace tesserkit {

namespace {

#if CONFIG_ETH_USE_ESP32_EMAC
esp_eth_phy_t* newPhy(EthernetBoard::Phy phy, const eth_phy_config_t* c) {
#if TESSERKIT_NAMED_PHYS
    switch (phy) {
        case EthernetBoard::Phy::Lan8720: return esp_eth_phy_new_lan87xx(c);
        case EthernetBoard::Phy::Ip101: return esp_eth_phy_new_ip101(c);
        case EthernetBoard::Phy::Rtl8201: return esp_eth_phy_new_rtl8201(c);
        case EthernetBoard::Phy::Dp83848: return esp_eth_phy_new_dp83848(c);
        case EthernetBoard::Phy::Ksz8081: return esp_eth_phy_new_ksz80xx(c);
        case EthernetBoard::Phy::Generic:
        case EthernetBoard::Phy::W5500: break;
    }
#else
    (void)phy;
#endif
#if TESSERKIT_GENERIC_PHY
    return esp_eth_phy_new_generic(c);
#else
    return nullptr;
#endif
}
#endif

// A PHY on the internal MAC. False with a message if a driver is missing;
// `mac` may be set then.
bool newRmii(const EthernetBoard& b, eth_mac_config_t& macConfig, esp_eth_mac_t*& mac, esp_eth_phy_t*& phy) {
#if CONFIG_ETH_USE_ESP32_EMAC
    if (b.power >= 0) {
        // Before the MAC: its reset needs the PHY's clock.
        gpio_num_t pin = static_cast<gpio_num_t>(b.power);
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 1);
        vTaskDelay(pdMS_TO_TICKS(20));  // the oscillator settles
    }
    // ESP-IDF's 4 KB is far more than the receive task uses (1 KB under
    // load on a GL-C-611WL, DESIGN.md section 11); the rest is headroom
    // for its error logs.
    macConfig.rx_task_stack_size = 2560;
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    emac.smi_gpio.mdc_num = b.mdc;
    emac.smi_gpio.mdio_num = b.mdio;
#else
    emac.smi_mdc_gpio_num = b.mdc;
    emac.smi_mdio_gpio_num = b.mdio;
#endif
    emac.clock_config.rmii.clock_mode = b.clock == EthernetBoard::Clock::In ? EMAC_CLK_EXT_IN : EMAC_CLK_OUT;
    emac.clock_config.rmii.clock_gpio = static_cast<decltype(emac.clock_config.rmii.clock_gpio)>(b.clockGpio);
    mac = esp_eth_mac_new_esp32(&emac, &macConfig);
    eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
    phyConfig.phy_addr = b.address;  // -1 is ESP_ETH_PHY_ADDR_AUTO
    phyConfig.reset_gpio_num = -1;   // the power pin isn't a reset
    phy = mac ? newPhy(b.phy, &phyConfig) : nullptr;
    if (!phy) tesser::logError("tesserkit: Ethernet: no %s driver", mac ? "PHY" : "MAC");
    return phy;
#else
    (void)b, (void)macConfig, (void)mac, (void)phy;
    tesser::logError("tesserkit: Ethernet: this chip has no internal MAC (a W5500 module on SPI works)");
    return false;
#endif
}

// A W5500 module on SPI2; a bus the application started already is shared.
bool newSpi(const EthernetBoard& b, eth_mac_config_t& macConfig, esp_eth_mac_t*& mac, esp_eth_phy_t*& phy) {
#if TESSERKIT_W5500
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
    if (b.irq < 0) {
        tesser::logError("tesserkit: Ethernet: a W5500 without its interrupt pin needs ESP-IDF 5.2");
        return false;
    }
#endif
    spi_bus_config_t bus = {};
    bus.mosi_io_num = b.mosi;
    bus.miso_io_num = b.miso;
    bus.sclk_io_num = b.sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        tesser::logError("tesserkit: Ethernet: SPI bus: %s", esp_err_to_name(err));
        return false;
    }
    if (b.irq >= 0) {
        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            tesser::logError("tesserkit: Ethernet: GPIO interrupts: %s", esp_err_to_name(err));
            return false;
        }
    }
    spi_device_interface_config_t device = {};  // copied by the driver
    device.mode = 0;
    device.clock_speed_hz = b.spiMhz * 1000 * 1000;
    device.queue_size = 20;
    device.spics_io_num = b.cs;
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &device);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    eth_wiznet_config_t& wiz = w5500.base;  // espressif/w5500 2.x
#else
    eth_w5500_config_t& wiz = w5500;
#endif
    wiz.int_gpio_num = b.irq;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    wiz.poll_period_ms = b.irq < 0 ? 10 : 0;
#endif
    mac = esp_eth_mac_new_w5500(&w5500, &macConfig);
    eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
    phyConfig.phy_addr = 1;  // the W5500's own: it has no MDIO bus
    phyConfig.reset_gpio_num = b.reset;
    phy = mac ? esp_eth_phy_new_w5500(&phyConfig) : nullptr;
    if (!mac) tesser::logError("tesserkit: Ethernet: no W5500 on SPI (CS %d)", b.cs);
    return phy;
#else
    (void)b, (void)macConfig, (void)mac, (void)phy;
    tesser::logError("tesserkit: Ethernet: no W5500 driver in this build (CONFIG_ETH_SPI_ETHERNET_W5500)");
    return false;
#endif
}

class EspEthernet : public Ethernet {
public:
    bool begin(const EthernetBoard& b, const char* hostname, const IpConfig& ip, EventSink sink) override {
        if (handle_) return true;
        esp::netifInit();
        eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
        esp_eth_mac_t* mac = nullptr;
        esp_eth_phy_t* phy = nullptr;
        if (!(b.spi() ? newSpi(b, macConfig, mac, phy) : newRmii(b, macConfig, mac, phy))) {
            if (mac) mac->del(mac);
            return false;
        }
        esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
        esp_err_t err = esp_eth_driver_install(&config, &handle_);
        if (err != ESP_OK) {
            // A PHY that doesn't answer on MDIO, no clock, or a module that
            // doesn't answer on SPI.
            tesser::logError("tesserkit: Ethernet: %s", esp_err_to_name(err));
            handle_ = nullptr;
            phy->del(phy);
            mac->del(mac);
            return false;
        }
        if (b.spi()) {
            // A module has no address of its own: the chip's for Ethernet
            // (its base address + 3, where it has four).
            uint8_t addr[6];
            if (esp_read_mac(addr, ESP_MAC_ETH) == ESP_OK) esp_eth_ioctl(handle_, ETH_CMD_S_MAC_ADDR, addr);
        }
        sink_ = std::move(sink);
        // "ETH_DEF", which mDNS and the IpStack follow. Its default route
        // priority (50) is below the station's (100): raised, so the cable
        // is the default route from the start, until Network chooses one
        // (IpStack::setDefault(), DESIGN.md section 5.2).
        static esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
        inherent.route_prio = 128;
        esp_netif_config_t netifConfig = ESP_NETIF_DEFAULT_ETH();
        netifConfig.base = &inherent;
        netif_ = esp_netif_new(&netifConfig);
        esp_netif_attach(netif_, esp_eth_new_netif_glue(handle_));
        esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, &EspEthernet::onEvent, this, nullptr);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &EspEthernet::onEvent, this, nullptr);
        hostname_ = hostname;
        esp_netif_set_hostname(netif_, hostname_.c_str());
        esp::applyIp(netif_, ip);
        err = esp_eth_start(handle_);
        if (err != ESP_OK) tesser::logError("tesserkit: esp_eth_start: %s", esp_err_to_name(err));
        running_ = err == ESP_OK;
        instance_ = this;
        esp::multicastChanged = &EspEthernet::allMulticast;
        if (esp::multicastSockets > 0) allMulticast(true);
        return running_;
    }

    void hostname(const char* name) override {
        hostname_ = name;
        if (netif_) esp_netif_set_hostname(netif_, hostname_.c_str());  // used from the next DHCP lease
    }

    void address(const IpConfig& ip) override {
        if (netif_) esp::applyIp(netif_, ip);
    }

    void enabled(bool on) override {
        if (!handle_ || on == running_) return;
        esp_err_t err = on ? esp_eth_start(handle_) : esp_eth_stop(handle_);
        if (err != ESP_OK) {
            tesser::logError("tesserkit: Ethernet %s: %s", on ? "start" : "stop", esp_err_to_name(err));
            return;
        }
        running_ = on;
        if (!on) link_ = false;  // stopping posts no disconnection
    }

    bool info(EthInfo& out) override {
        if (!handle_) return false;
        out = EthInfo();
        esp_eth_ioctl(handle_, ETH_CMD_G_MAC_ADDR, out.mac);
        out.link = link_;
        if (!link_) return true;
        eth_speed_t speed;
        if (esp_eth_ioctl(handle_, ETH_CMD_G_SPEED, &speed) == ESP_OK) out.mbps = speed == ETH_SPEED_10M ? 10 : 100;
        eth_duplex_t duplex;
        if (esp_eth_ioctl(handle_, ETH_CMD_G_DUPLEX_MODE, &duplex) == ESP_OK) out.fullDuplex = duplex == ETH_DUPLEX_FULL;
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif_, &ip) == ESP_OK) {
            out.ip = esp::fromNetwork(ip.ip.addr);
            out.gateway = esp::fromNetwork(ip.gw.addr);
        }
        return true;
    }

private:
    static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
        EspEthernet* self = static_cast<EspEthernet*>(arg);
        Event e;
        if (base == ETH_EVENT) {
            if (*static_cast<esp_eth_handle_t*>(data) != self->handle_) return;
            if (id != ETHERNET_EVENT_CONNECTED && id != ETHERNET_EVENT_DISCONNECTED) return;
            self->link_ = id == ETHERNET_EVENT_CONNECTED;
            e.type = Event::Type::EthLink;
            e.value = self->link_ ? 1 : 0;
        } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
            if (static_cast<ip_event_got_ip_t*>(data)->esp_netif != self->netif_) return;
            e.type = Event::Type::EthGotIp;
        } else {
            return;
        }
        if (self->sink_) self->sink_(std::move(e));
    }

    // Every multicast frame passed on while a UDP socket has groups
    // (udp_esp.cpp): the MAC's seven address filters can't hold them.
    static void allMulticast(bool on) {
        if (!instance_ || !instance_->handle_) return;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
        esp_eth_ioctl(instance_->handle_, ETH_CMD_S_ALL_MULTICAST, &on);
#else
        esp_eth_ioctl(instance_->handle_, ETH_CMD_S_PROMISCUOUS, &on);
#endif
    }

    static EspEthernet* instance_;
    EventSink sink_;
    esp_eth_handle_t handle_ = nullptr;
    esp_netif_t* netif_ = nullptr;
    std::string hostname_;
    // Set by the link's events themselves, so it holds whether or not the
    // device's queue took them (info()).
    volatile bool link_ = false;
    bool running_ = false;
};

EspEthernet* EspEthernet::instance_ = nullptr;

}  // namespace

Ethernet* espEthernet() {
    static EspEthernet eth;
    return &eth;
}

}  // namespace tesserkit

#else  // no internal Ethernet MAC on this chip, and no W5500 driver

namespace tesserkit {
Ethernet* espEthernet() { return nullptr; }
}  // namespace tesserkit

#endif
#endif
