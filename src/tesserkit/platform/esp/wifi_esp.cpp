// Wi-Fi station and access point, and the captive portal, on ESP-IDF's
// esp_wifi and esp_netif (which Arduino-ESP32 3.x ships).
#if defined(ESP_PLATFORM)

#include "tesser/platform.h"
#include "tesserkit/platform.h"
#include "tesserkit/platform/esp/esp_internal.h"

#include <string.h>

#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"

namespace tesserkit {

namespace {

constexpr uint16_t kMaxScanResults = 20;

class EspWifi : public Wifi {
public:
    void begin(const char* hostname, EventSink sink) override {
        sink_ = std::move(sink);
        // Applications (and Arduino) may have done some of this already.
        esp_err_t err = esp::nvsInit();  // the driver keeps its calibration there
        if (err != ESP_OK) tesser::logError("tesserkit: NVS: %s", esp_err_to_name(err));
        esp::netifInit();
        sta_ = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!sta_) sta_ = esp_netif_create_default_wifi_sta();
        ap_ = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!ap_) ap_ = esp_netif_create_default_wifi_ap();
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) tesser::logError("tesserkit: esp_wifi_init: %s", esp_err_to_name(err));
        esp_wifi_set_storage(WIFI_STORAGE_RAM);  // settings live in the tree
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EspWifi::onEvent, this, nullptr);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EspWifi::onEvent, this, nullptr);
        hostname_ = hostname;
        esp_netif_set_hostname(sta_, hostname_.c_str());
        esp_wifi_set_mode(WIFI_MODE_STA);
        err = esp_wifi_start();
        if (err != ESP_OK) tesser::logError("tesserkit: esp_wifi_start: %s", esp_err_to_name(err));
    }

    void hostname(const char* name) override {
        hostname_ = name;
        esp_netif_set_hostname(sta_, hostname_.c_str());  // used from the next DHCP lease
    }

    void txPower(int8_t dBm) override { esp_wifi_set_max_tx_power(static_cast<int8_t>(dBm * 4)); }

    void powerSave(PowerSave mode) override {
        esp_err_t err = esp_wifi_set_ps(mode == PowerSave::None  ? WIFI_PS_NONE
                                        : mode == PowerSave::Max ? WIFI_PS_MAX_MODEM
                                                                 : WIFI_PS_MIN_MODEM);
        if (err != ESP_OK) tesser::logWarning("tesserkit: esp_wifi_set_ps: %s", esp_err_to_name(err));
    }

    bool scan() override {
        wifi_scan_config_t config = {};
        return esp_wifi_scan_start(&config, false) == ESP_OK;
    }

    std::vector<ScanResult> scanResults() override {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > kMaxScanResults) n = kMaxScanResults;
        std::vector<wifi_ap_record_t> records(n);
        if (esp_wifi_scan_get_ap_records(&n, records.data()) != ESP_OK) n = 0;
        esp_wifi_clear_ap_list();  // frees what wasn't fetched
        std::vector<ScanResult> out;
        for (uint16_t i = 0; i < n; i++) {
            const wifi_ap_record_t& r = records[i];
            std::string ssid(reinterpret_cast<const char*>(r.ssid), strnlen(reinterpret_cast<const char*>(r.ssid), 32));
            if (ssid.empty()) continue;
            ScanResult s;
            s.ssid = ssid;
            memcpy(s.bssid, r.bssid, sizeof(s.bssid));
            s.rssi = r.rssi;
            s.channel = r.primary;
            s.secure = r.authmode != WIFI_AUTH_OPEN;
            out.push_back(s);
        }
        return out;
    }

    bool connect(const StaConfig& c) override {
        if (linked_) esp_wifi_disconnect();
        wifi_config_t config = {};
        strncpy(reinterpret_cast<char*>(config.sta.ssid), c.ssid.c_str(), sizeof(config.sta.ssid));
        strncpy(reinterpret_cast<char*>(config.sta.password), c.password.c_str(), sizeof(config.sta.password));
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        config.sta.threshold.authmode = WIFI_AUTH_OPEN;  // accept whatever the network uses
        if (c.bssidSet) {
            config.sta.bssid_set = true;
            memcpy(config.sta.bssid, c.bssid, sizeof(config.sta.bssid));
        }
        esp::applyIp(sta_, c);
        if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) return false;
        return esp_wifi_connect() == ESP_OK;
    }

    void disconnect() override { esp_wifi_disconnect(); }

    bool staInfo(StaInfo& out) override {
        if (!linked_) return false;
        wifi_ap_record_t r;
        if (esp_wifi_sta_get_ap_info(&r) != ESP_OK) return false;
        out.ssid.assign(reinterpret_cast<const char*>(r.ssid), strnlen(reinterpret_cast<const char*>(r.ssid), 32));
        out.rssi = r.rssi;
        out.channel = r.primary;
        memcpy(out.bssid, r.bssid, 6);
        esp_netif_ip_info_t ip;
        out.ip = esp_netif_get_ip_info(sta_, &ip) == ESP_OK ? esp::fromNetwork(ip.ip.addr) : 0;
        return true;
    }

    bool startAp(const ApConfig& c) override {
        wifi_config_t config = {};
        size_t len = c.ssid.size() < sizeof(config.ap.ssid) ? c.ssid.size() : sizeof(config.ap.ssid);
        memcpy(config.ap.ssid, c.ssid.data(), len);
        config.ap.ssid_len = static_cast<uint8_t>(len);
        strncpy(reinterpret_cast<char*>(config.ap.password), c.password.c_str(), sizeof(config.ap.password));
        config.ap.channel = c.channel;
        config.ap.authmode = c.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        config.ap.ssid_hidden = c.hidden ? 1 : 0;
        config.ap.max_connection = 4;
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return false;
        return esp_wifi_set_config(WIFI_IF_AP, &config) == ESP_OK;
    }

    void stopAp() override { esp_wifi_set_mode(WIFI_MODE_STA); }

    uint32_t apIp() override {
        esp_netif_ip_info_t ip;
        return esp_netif_get_ip_info(ap_, &ip) == ESP_OK ? esp::fromNetwork(ip.ip.addr) : 0;
    }

    void captivePortal(bool on) override {
        uint32_t ip = apIp();
        if (on) {
            esp::dnsStart(ip);
        } else {
            esp::dnsStop();
        }
        esp::captiveRedirect(on, ip);
    }

private:
    static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
        EspWifi* self = static_cast<EspWifi*>(arg);
        Event e;
        if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            self->linked_ = true;
            e.type = Event::Type::StaGotIp;
        } else if (base != WIFI_EVENT) {
            return;
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            e.type = Event::Type::StaAssociated;
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            e.type = Event::Type::ScanDone;
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            self->linked_ = false;
            e.type = Event::Type::StaDisconnected;
            e.value = static_cast<const wifi_event_sta_disconnected_t*>(data)->reason;
        } else if (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_sta_list_t list;
            e.type = Event::Type::ApClients;
            e.value = esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
        } else {
            return;
        }
        // After esp_netif's own handler (registered first), which started
        // the station's DHCP client.
        if (e.type == Event::Type::StaAssociated) esp::dnsRefresh();
        if (self->sink_) self->sink_(std::move(e));
    }

    EventSink sink_;
    esp_netif_t* sta_ = nullptr;
    esp_netif_t* ap_ = nullptr;
    std::string hostname_;
    volatile bool linked_ = false;
};

// lwIP has one list of DNS servers for all its interfaces. A lease's servers
// come in its ACK, seconds before its address (the address check, ACD), and
// esp_netif empties the list whenever an interface starts its DHCP client (a
// station associating, a cable's link) or takes a static address. A station
// trying to join in those seconds left a device on Ethernet without names
// until the lease's renewal, hours later. So an empty list is filled again
// (DESIGN.md section 5.2). Touched only in lwIP's task.
struct StaticDns {
    netif* interface = nullptr;
    uint32_t server = 0;  // network byte order; 0: none
};
StaticDns g_staticDns[3];  // the station, Ethernet, a spare
NETIF_DECLARE_EXT_CALLBACK(g_netifCallback)

// From an interface with an address: its static server, or its lease renewed,
// whose ACK brings the lease's servers back.
bool fillDns(netif* n) {
    if (!netif_is_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n))) return false;
    for (const StaticDns& s : g_staticDns) {
        if (s.interface == n && s.server) {
            ip_addr_t server;
            ip_addr_set_ip4_u32_val(server, s.server);
            dns_setserver(0, &server);
            return true;
        }
    }
    if (!dhcp_supplied_address(n)) return false;
    dhcp_renew(n);
    return true;
}

// The default interface first, which routes go through.
void fillEmptyDns() {
    if (!ip_addr_isany(dns_getserver(0))) return;
    if (netif_default && fillDns(netif_default)) return;
    netif* n;
    NETIF_FOREACH(n) {
        if (n != netif_default && fillDns(n)) return;
    }
}

void onNetifChange(netif*, netif_nsc_reason_t, const netif_ext_callback_args_t*) { fillEmptyDns(); }

struct StaticDnsChange {
    esp_netif_t* netif;
    uint32_t server;
};

// Before the address changes, whose callback fills the list.
esp_err_t setStaticDns(void* arg) {
    const StaticDnsChange* c = static_cast<const StaticDnsChange*>(arg);
    netif* n = static_cast<netif*>(esp_netif_get_netif_impl(c->netif));
    StaticDns* slot = nullptr;
    for (StaticDns& s : g_staticDns) {
        if (s.interface == n) {
            slot = &s;
            break;
        }
        if (!s.interface && !slot) slot = &s;
    }
    if (slot) {
        slot->interface = n;
        slot->server = c->server;
    }
    return ESP_OK;
}

}  // namespace

namespace esp {

void netifInit() {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) tesser::logError("tesserkit: esp_netif_init: %s", esp_err_to_name(err));
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) tesser::logError("tesserkit: event loop: %s", esp_err_to_name(err));
    esp_netif_tcpip_exec(
        [](void*) {
            if (!g_netifCallback.callback_fn) netif_add_ext_callback(&g_netifCallback, onNetifChange);
            return ESP_OK;
        },
        nullptr);
}

void dnsRefresh() {
    esp_netif_tcpip_exec(
        [](void*) {
            fillEmptyDns();
            return ESP_OK;
        },
        nullptr);
}

void applyIp(esp_netif_t* netif, const IpConfig& c) {
    if (!c.staticIp) {
        // Only a client a static address stopped is started here: esp_netif
        // starts the others on a link or an association. Starting one
        // empties the DNS servers' list, which an attempt to join shouldn't.
        StaticDnsChange dns = {netif, 0};
        esp_netif_tcpip_exec(setStaticDns, &dns);
        esp_netif_dhcp_status_t dhcp;
        if (esp_netif_dhcpc_get_status(netif, &dhcp) == ESP_OK && dhcp == ESP_NETIF_DHCP_STOPPED) {
            esp_netif_ip_info_t none = {};
            esp_netif_set_ip_info(netif, &none);  // the static address goes
            esp_netif_dhcpc_start(netif);
            dnsRefresh();
        }
        return;
    }
    StaticDnsChange dns = {netif, toNetwork(c.dns)};
    esp_netif_tcpip_exec(setStaticDns, &dns);
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip = {};
    ip.ip.addr = toNetwork(c.ip);
    ip.netmask.addr = toNetwork(c.mask);
    ip.gw.addr = toNetwork(c.gateway);
    esp_netif_set_ip_info(netif, &ip);  // empties the DNS servers' list
    dnsRefresh();
}

}  // namespace esp

Wifi* espWifi() {
    static EspWifi wifi;
    return &wifi;
}

}  // namespace tesserkit

#endif
