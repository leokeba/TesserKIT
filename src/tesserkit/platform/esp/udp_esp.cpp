// UDP sockets on lwIP, read without blocking from the device's task, or by a
// task of their own (listen()).
#if defined(ESP_PLATFORM)

#include <errno.h>
#include <stdlib.h>

#include <atomic>

#include "tesserkit/platform.h"
#include "tesserkit/platform/esp/esp_internal.h"

#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace tesserkit {

namespace {

constexpr size_t kListenBuffer = 640;  // E1.31's largest data packet is 638 bytes

class EspUdpSocket : public UdpSocket {
public:
    explicit EspUdpSocket(int fd) : fd_(fd) {}
    ~EspUdpSocket() override {
        if (task_) {
            stop_ = true;
            // The task looks at stop_ after each receive's timeout (100 ms).
            xSemaphoreTake(done_, portMAX_DELAY);
            vSemaphoreDelete(done_);
        }
        close(fd_);
        free(buf_);
    }

    size_t receive(uint8_t* buf, size_t cap, uint32_t& fromIp, uint16_t& fromPort) override {
        if (task_) return 0;
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

    bool listen(const char* name, uint8_t priority, int8_t core, uint32_t stackBytes, UdpReceiver fn) override {
        if (task_) return false;
        buf_ = static_cast<uint8_t*>(malloc(kListenBuffer));
        done_ = xSemaphoreCreateBinary();
        if (!buf_ || !done_) return unlisten();
        timeval tv = {};
        tv.tv_usec = 100000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        fn_ = std::move(fn);
        BaseType_t affinity = core < 0 ? tskNO_AFFINITY : core;
        if (xTaskCreatePinnedToCore(run, name, stackBytes, this, priority, &task_, affinity) != pdPASS) {
            task_ = nullptr;
            return unlisten();
        }
        return true;
    }

private:
    static void run(void* arg) {
        EspUdpSocket* self = static_cast<EspUdpSocket*>(arg);
        while (!self->stop_) {
            sockaddr_in from = {};
            socklen_t len = sizeof(from);
            int n = recvfrom(self->fd_, self->buf_, kListenBuffer, 0, reinterpret_cast<sockaddr*>(&from), &len);
            if (n > 0) {
                self->fn_(self->buf_, static_cast<size_t>(n), esp::fromNetwork(from.sin_addr.s_addr),
                          static_cast<uint64_t>(esp_timer_get_time()));
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));  // not the timeout: don't spin at this priority
            }
        }
        xSemaphoreGive(self->done_);
        vTaskDelete(nullptr);
    }

    bool unlisten() {
        if (done_) vSemaphoreDelete(done_);
        done_ = nullptr;
        free(buf_);
        buf_ = nullptr;
        return false;
    }

    int fd_;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t done_ = nullptr;
    std::atomic<bool> stop_{false};
    uint8_t* buf_ = nullptr;
    UdpReceiver fn_;
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
