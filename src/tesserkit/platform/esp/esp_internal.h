#pragma once

// Shared between the ESP implementations of the platform layer.

#if defined(ESP_PLATFORM)

#include <stdint.h>

#include <atomic>

#include "esp_err.h"
#include "esp_netif.h"
#include "tesserkit/platform.h"

namespace tesserkit {
namespace esp {

// The HTTP server's 404 handler redirects to the interface while on
// (device_esp.cpp).
void captiveRedirect(bool on, uint32_t apIp);

// Initializes NVS (Wi-Fi needs it too). A partition NVS can't use (left
// by a firmware with another partition table or NVS version) is erased:
// nothing in it could be read anyway (device_esp.cpp).
esp_err_t nvsInit();

// Answers every DNS query with `ip` while started (dns_server.cpp).
bool dnsStart(uint32_t ip);
void dnsStop();

// DHCP, or the static address in `ip`, on an interface (wifi_esp.cpp).
void applyIp(esp_netif_t* netif, const IpConfig& ip);

// Initializes esp_netif and the default event loop, which applications
// (and Arduino) may have done already (wifi_esp.cpp).
void netifInit();

// UDP sockets with multicast groups (udp_groups_esp.cpp), and what Ethernet
// installs to pass every multicast frame on while there are any
// (eth_esp.cpp): called on the first socket's groups and after the last's.
extern std::atomic<int> multicastSockets;
extern void (*multicastChanged)(bool on);

// a.b.c.d as (a << 24) | ... to and from lwIP's network byte order.
inline uint32_t toNetwork(uint32_t ip) { return __builtin_bswap32(ip); }
inline uint32_t fromNetwork(uint32_t addr) { return __builtin_bswap32(addr); }

}  // namespace esp
}  // namespace tesserkit

#endif
