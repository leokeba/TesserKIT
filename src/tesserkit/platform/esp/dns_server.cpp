// The captive portal's DNS server: every A query is answered with the access
// point's address, so any name a phone looks up on joining leads to the
// device. Other query types get an empty answer, quickly.
#if defined(ESP_PLATFORM)

#include "tesser/platform.h"
#include "tesserkit/platform/esp/esp_internal.h"

#include <string.h>

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace tesserkit {
namespace esp {

namespace {

std::atomic<bool> g_running{false};
std::atomic<bool> g_taskAlive{false};
std::atomic<uint32_t> g_ip{0};

constexpr size_t kMaxPacket = 512;
constexpr uint8_t kAnswer[] = {
    0xC0, 0x0C,              // the name: a pointer to the question's
    0x00, 0x01, 0x00, 0x01,  // type A, class IN
    0x00, 0x00, 0x00, 0x3C,  // TTL 60 s
    0x00, 0x04,              // 4 bytes of address
};

// Turns the query in `buf` into its answer; returns the answer's length,
// or 0 to stay silent.
size_t answer(uint8_t* buf, size_t len) {
    if (len < 12 || (buf[2] & 0x80) || (buf[2] & 0x78)) return 0;  // a response, or not a standard query
    if (buf[4] != 0 || buf[5] != 1) return 0;                        // one question
    size_t i = 12;
    while (i < len && buf[i]) {
        if (buf[i] & 0xC0) return 0;  // no compression in questions
        i += buf[i] + 1u;
    }
    i += 1;
    if (i + 4 > len) return 0;
    bool a = buf[i] == 0 && (buf[i + 1] == 1 || buf[i + 1] == 255);  // A or ANY
    size_t end = i + 4;
    buf[2] = static_cast<uint8_t>(0x84 | (buf[2] & 0x01));  // response, authoritative, RD kept
    buf[3] = 0x80;                                          // recursion available, no error
    buf[6] = 0;
    buf[7] = a ? 1 : 0;
    memset(buf + 8, 0, 4);  // no authority or additional records
    if (!a) return end;
    if (end + sizeof(kAnswer) + 4 > kMaxPacket) return 0;
    memcpy(buf + end, kAnswer, sizeof(kAnswer));
    end += sizeof(kAnswer);
    uint32_t ip = g_ip.load();
    buf[end++] = static_cast<uint8_t>(ip >> 24);
    buf[end++] = static_cast<uint8_t>(ip >> 16);
    buf[end++] = static_cast<uint8_t>(ip >> 8);
    buf[end++] = static_cast<uint8_t>(ip);
    return end;
}

void dnsTask(void*) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(53);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        timeval timeout = {1, 0};  // to notice dnsStop()
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            uint8_t buf[kMaxPacket];
            while (g_running.load()) {
                sockaddr_in from;
                socklen_t fromLen = sizeof(from);
                int n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n <= 0) continue;
                size_t out = answer(buf, static_cast<size_t>(n));
                if (out) sendto(sock, buf, out, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
            }
        } else {
            tesser::logError("tesserkit: DNS server can't bind port 53");
        }
        close(sock);
    }
    g_taskAlive.store(false);
    // Started again while this one was stopping: hand over to a new one.
    if (g_running.load() && !g_taskAlive.exchange(true)) {
        if (xTaskCreate(dnsTask, "tk_dns", 3072, nullptr, 2, nullptr) != pdPASS) g_taskAlive.store(false);
    }
    vTaskDelete(nullptr);
}

}  // namespace

bool dnsStart(uint32_t ip) {
    g_ip.store(ip);
    if (g_running.exchange(true)) return true;
    if (g_taskAlive.exchange(true)) return true;  // still stopping: it carries on
    if (xTaskCreate(dnsTask, "tk_dns", 3072, nullptr, 2, nullptr) != pdPASS) {
        g_running.store(false);
        g_taskAlive.store(false);
        return false;
    }
    return true;
}

void dnsStop() { g_running.store(false); }

}  // namespace esp
}  // namespace tesserkit

#endif
