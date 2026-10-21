// lwIP's list of DNS servers and its default route, on ESP-IDF's esp_netif
// (which Arduino-ESP32 3.x ships). What they should be is Network's to
// decide (DESIGN.md section 5.2): this file only reads and writes them,
// renews a lease, and says when the list emptied. Every lwIP call runs in
// lwIP's task (esp_netif_tcpip_exec).
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"
#include "tesserkit/platform/esp/esp_internal.h"

#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"

namespace tesserkit {

namespace {

NETIF_DECLARE_EXT_CALLBACK(g_netifCallback)
EventSink* g_sink = nullptr;  // set once, before the callback is added

// In lwIP's task, on any interface's change. A station's association or a
// cable's link empties the list too, without a change here: Network refills
// on those events itself.
void onNetifChange(netif*, netif_nsc_reason_t, const netif_ext_callback_args_t*) {
    if (!g_sink || !*g_sink || !ip_addr_isany(dns_getserver(0))) return;
    Event e;
    e.type = Event::Type::DnsEmpty;
    (*g_sink)(std::move(e));
}

esp_netif_t* handle(NetIf n) {
    return esp_netif_get_handle_from_ifkey(n == NetIf::Station ? "WIFI_STA_DEF" : "ETH_DEF");
}

class EspIpStack : public IpStack {
public:
    void begin(EventSink sink) override {
        sink_ = std::move(sink);
        g_sink = &sink_;
        esp::netifInit();
        esp_netif_tcpip_exec(
            [](void*) {
                if (!g_netifCallback.callback_fn) netif_add_ext_callback(&g_netifCallback, onNetifChange);
                return ESP_OK;
            },
            nullptr);
    }

    uint32_t dnsServer() override {
        uint32_t out = 0;
        esp_netif_tcpip_exec(
            [](void* arg) {
                const ip_addr_t* s = dns_getserver(0);
                if (!ip_addr_isany(s) && IP_IS_V4(s)) *static_cast<uint32_t*>(arg) = ip4_addr_get_u32(ip_2_ip4(s));
                return ESP_OK;
            },
            &out);
        return esp::fromNetwork(out);
    }

    void setDnsServer(uint32_t ip) override {
        uint32_t addr = esp::toNetwork(ip);
        esp_netif_tcpip_exec(
            [](void* arg) {
                ip_addr_t server;
                ip_addr_set_ip4_u32_val(server, *static_cast<uint32_t*>(arg));
                dns_setserver(0, &server);
                return ESP_OK;
            },
            &addr);
    }

    bool renewLease(NetIf n) override {
        esp_netif_t* h = handle(n);
        if (!h) return false;
        netif* lw = static_cast<netif*>(esp_netif_get_netif_impl(h));
        if (!lw) return false;
        return esp_netif_tcpip_exec(
                   [](void* arg) {
                       netif* i = static_cast<netif*>(arg);
                       if (!netif_is_up(i) || !dhcp_supplied_address(i)) return ESP_FAIL;
                       dhcp_renew(i);
                       return ESP_OK;
                   },
                   lw) == ESP_OK;
    }

    // esp_netif's manual default: kept until that interface is destroyed,
    // rather than chosen again by route priority on each address event.
    void setDefault(NetIf n) override {
        if (esp_netif_t* h = handle(n)) esp_netif_set_default_netif(h);
    }

private:
    EventSink sink_;
};

}  // namespace

IpStack* espIpStack() {
    static EspIpStack ipStack;
    return &ipStack;
}

}  // namespace tesserkit

#endif
