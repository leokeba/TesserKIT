#pragma once

#include <vector>

#include "tesserkit/dmx.h"

namespace tesserkit {

// /sys/sacn: an E1.31 (streaming ACN) receiver mapping DMX channels of one
// universe to nodes of the tree, and handing the universes an application
// asks for to its DmxSink, from the source with the highest priority
// (docs/DESIGN.md section 5.22).
class Sacn : public DmxService {
public:
    static constexpr uint16_t kPort = 5568;
    static constexpr uint32_t kLossMs = 2500;  // E1.31's network data loss timeout
    static constexpr size_t kMaxSyncAddresses = 4;

    explicit Sacn(Device& dev) : DmxService(dev, "sacn", DmxProtocol::Sacn, kPort, 1, 1, 63999) {}

    void describe(tesser::Object& o) override;

    // The multicast group of a universe: 239.255.<high byte>.<low byte>.
    static uint32_t group(uint16_t universe) { return 0xEFFF0000u | universe; }

protected:
    void received(const uint8_t* data, size_t len, uint32_t fromIp, uint64_t atUs) override;
    void ticked(uint32_t nowMs) override;
    void start() override;
    void opened() override;
    void closing() override;

private:
    // Under lock_.
    void drop(Track* t, Source* s, int i);
    void elect(Track* t, Source* s);

    std::vector<uint16_t> syncAddresses_;  // seen in data packets, under lock_
    UdpGroups* groups_ = nullptr;
    std::vector<uint32_t> joined_;         // the groups asked for the socket
    uint32_t joinedAtMs_ = 0;
    uint32_t joinedAddress_ = 0;  // the device's address then
};

}  // namespace tesserkit
