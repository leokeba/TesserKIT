// IPv4 multicast groups for UDP sockets, through lwIP's IGMP: lwIP's socket
// option allows only as many memberships as there are sockets in all, while
// ESP-IDF allocates IGMP groups from the heap.
#if defined(ESP_PLATFORM)

#include <algorithm>
#include <atomic>
#include <map>

#include "tesserkit/platform.h"
#include "tesserkit/platform/esp/esp_internal.h"

#include "lwip/igmp.h"
#include "lwip/netif.h"
#include "lwip/priv/tcpip_priv.h"

namespace tesserkit {

namespace esp {
std::atomic<int> multicastSockets{0};
void (*multicastChanged)(bool on) = nullptr;
}  // namespace esp

namespace {

class EspUdpGroups : public UdpGroups {
public:
    bool set(UdpSocket& socket, const std::vector<uint32_t>& groups) override {
        std::vector<uint32_t>& joined = joined_[&socket];
        Call c;
        c.joined = &joined;
        c.wanted = &groups;
        // Ethernet passes every multicast frame before the first group is
        // joined, and until the last is left.
        bool had = !joined.empty(), has = !groups.empty();
        if (has && !had && ++esp::multicastSockets == 1 && esp::multicastChanged) esp::multicastChanged(true);
        tcpip_api_call(apply, &c.call);
        if (had && !has && --esp::multicastSockets == 0 && esp::multicastChanged) esp::multicastChanged(false);
        if (!has) joined_.erase(&socket);
        return true;
    }

private:
    struct Call {
        tcpip_api_call_data call;  // first: lwIP hands it back
        std::vector<uint32_t>* joined;
        const std::vector<uint32_t>* wanted;
    };

    // In lwIP's task.
    static err_t apply(tcpip_api_call_data* data) {
        Call* c = reinterpret_cast<Call*>(data);
        const std::vector<uint32_t>& wanted = *c->wanted;
        netif* nif = nullptr;
        ip4_addr_t a;
        for (uint32_t g : *c->joined) {
            if (std::find(wanted.begin(), wanted.end(), g) != wanted.end()) continue;
            a.addr = esp::toNetwork(g);
            NETIF_FOREACH(nif) {
                if (igmp_lookfor_group(nif, &a)) unfiltered(nif, [&] { igmp_leavegroup_netif(nif, &a); });
            }
        }
        // Interfaces come and go (Ethernet, the access point): each that
        // lacks a group joins it.
        NETIF_FOREACH(nif) {
            if (!(nif->flags & NETIF_FLAG_IGMP)) continue;
            for (uint32_t g : wanted) {
                a.addr = esp::toNetwork(g);
                if (!igmp_lookfor_group(nif, &a)) unfiltered(nif, [&] { igmp_joingroup_netif(nif, &a); });
            }
        }
        *c->joined = wanted;
        return ERR_OK;
    }

    // Without the interface's MAC filter: Ethernet passes every multicast
    // frame meanwhile, and its seven filters would only log failures.
    template <typename F>
    static void unfiltered(netif* nif, F fn) {
        netif_igmp_mac_filter_fn filter = nif->igmp_mac_filter;
        nif->igmp_mac_filter = nullptr;
        fn();
        nif->igmp_mac_filter = filter;
    }

    std::map<const UdpSocket*, std::vector<uint32_t>> joined_;  // a.b.c.d as (a << 24) | ...
};

}  // namespace

UdpGroups* espUdpGroups() {
    static EspUdpGroups groups;
    return &groups;
}

}  // namespace tesserkit

#endif
