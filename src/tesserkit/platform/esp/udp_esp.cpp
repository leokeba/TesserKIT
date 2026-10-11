// UDP sockets on lwIP, read without blocking from the device's task.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"
#include "tesserkit/platform/esp/esp_internal.h"

#include "esp_netif.h"
#include "lwip/sockets.h"

namespace tesserkit {

namespace {

class EspUdpSocket : public UdpSocket {
public:
    explicit EspUdpSocket(int fd) : fd_(fd) {}
    ~EspUdpSocket() override { close(fd_); }

    size_t receive(uint8_t* buf, size_t cap, uint32_t& fromIp, uint16_t& fromPort) override {
        sockaddr_in from = {};
        socklen_t len = sizeof(from);
        int n = recvfrom(fd_, buf, cap, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &len);
        if (n <= 0) return 0;
        fromIp = esp::fromNetwork(from.sin_addr.s_addr);
        fromPort = ntohs(from.sin_port);
        return static_cast<size_t>(n);
    }

    bool send(uint32_t ip, uint16_t port, const uint8_t* data, size_t len) override {
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = esp::toNetwork(ip);
        return sendto(fd_, data, len, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == static_cast<int>(len);
    }

private:
    int fd_;
};

class EspUdp : public Udp {
public:
    std::unique_ptr<UdpSocket> open(uint16_t port) override {
        esp_netif_init();  // lwIP must be up; ESP_ERR_INVALID_STATE once it is
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) return nullptr;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(fd);
            return nullptr;
        }
        return std::unique_ptr<UdpSocket>(new EspUdpSocket(fd));
    }
};

}  // namespace

Udp* espUdp() {
    static EspUdp udp;
    return &udp;
}

}  // namespace tesserkit

#endif
