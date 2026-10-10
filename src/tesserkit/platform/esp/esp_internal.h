#pragma once

// Shared between the ESP implementations of the platform layer.

#if defined(ESP_PLATFORM)

#include <stdint.h>

namespace tesserkit {
namespace esp {

// The HTTP server's 404 handler redirects to the interface while on
// (device_esp.cpp).
void captiveRedirect(bool on, uint32_t apIp);

// Answers every DNS query with `ip` while started (dns_server.cpp).
bool dnsStart(uint32_t ip);
void dnsStop();

// a.b.c.d as (a << 24) | ... to and from lwIP's network byte order.
inline uint32_t toNetwork(uint32_t ip) { return __builtin_bswap32(ip); }
inline uint32_t fromNetwork(uint32_t addr) { return __builtin_bswap32(addr); }

}  // namespace esp
}  // namespace tesserkit

#endif
