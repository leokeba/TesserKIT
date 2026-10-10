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
#include "esp_wifi.h"

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
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) tesser::logError("tesserkit: esp_netif_init: %s", esp_err_to_name(err));
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) tesser::logError("tesserkit: event loop: %s", esp_err_to_name(err));
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
            bool seen = false;
            for (ScanResult& s : out) {  // one entry per name, its strongest access point
                if (s.ssid != ssid) continue;
                seen = true;
                if (r.rssi > s.rssi) {
                    s.rssi = r.rssi;
                    s.channel = r.primary;
                }
            }
            if (seen) continue;
            ScanResult s;
            s.ssid = ssid;
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
        if (c.staticIp) {
            esp_netif_dhcpc_stop(sta_);
            esp_netif_ip_info_t ip = {};
            ip.ip.addr = esp::toNetwork(c.ip);
            ip.netmask.addr = esp::toNetwork(c.mask);
            ip.gw.addr = esp::toNetwork(c.gateway);
            esp_netif_set_ip_info(sta_, &ip);
            esp_netif_dns_info_t dns = {};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = esp::toNetwork(c.dns);
            esp_netif_set_dns_info(sta_, ESP_NETIF_DNS_MAIN, &dns);
        } else {
            esp_netif_dhcpc_start(sta_);  // already started is fine
        }
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
        if (self->sink_) self->sink_(std::move(e));
    }

    EventSink sink_;
    esp_netif_t* sta_ = nullptr;
    esp_netif_t* ap_ = nullptr;
    std::string hostname_;
    volatile bool linked_ = false;
};

}  // namespace

Wifi* espWifi() {
    static EspWifi wifi;
    return &wifi;
}

}  // namespace tesserkit

#endif
