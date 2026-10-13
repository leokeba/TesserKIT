// TesserAPI's UdpTransport: the API over UDP on the LAN.
#if defined(ESP_PLATFORM)

#include <memory>

#include "tesser/transports/udp_transport.h"
#include "tesserkit/platform.h"

namespace tesserkit {

namespace {

class EspUdpApi : public UdpApiTransport {
public:
    bool begin(tesser::Api& api, uint16_t port, const std::string& name, size_t maxCalls, bool ownTask,
               std::function<void()> queued, uint8_t interfaces) override {
        if (!transport_) {
            tesser::DatagramEndpoint::Options o = tesser::UdpLink::endpointOptions();
            o.maxCalls = maxCalls;
            transport_.reset(new tesser::UdpTransport(&api, tesser::UdpLink::Options(), o));
        }
        transport_->setName(name);
        transport_->setInterfaces(interfaces);
        polled_ = !ownTask;
        if (polled_) transport_->link().onQueued(std::move(queued));
        return transport_->begin(port, polled_ ? tesser::UdpTransport::Tasks::Receive : tesser::UdpTransport::Tasks::Both);
    }
    void poll() override {
        if (polled_ && transport_) transport_->poll();
    }
    void end() override {
        if (transport_) transport_->end();
    }
    tesser::UdpLink* link() override { return transport_ ? &transport_->link() : nullptr; }

private:
    std::unique_ptr<tesser::UdpTransport> transport_;
    bool polled_ = false;  // no worker task: poll() does its work
};

}  // namespace

UdpApiTransport* espUdpApi() {
    static EspUdpApi udp;
    return &udp;
}

}  // namespace tesserkit

#endif
