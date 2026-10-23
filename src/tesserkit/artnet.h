#pragma once

#include <vector>

#include "tesserkit/dmx.h"

namespace tesserkit {

// An element of /sys/artnet/mappings.
using ArtNetMapping = DmxMapping;

// /sys/artnet: an Art-Net node mapping DMX channels of one universe to nodes
// of the tree, and handing the universes an application asks for to its
// DmxSink (docs/DESIGN.md section 5.9).
class ArtNet : public DmxService {
public:
    static constexpr uint16_t kPort = 6454;

    explicit ArtNet(Device& dev) : DmxService(dev, "artnet", DmxProtocol::ArtNet, kPort, 0, 0, 32767) {}

    void describe(tesser::Object& o) override;

protected:
    void received(const uint8_t* data, size_t len, uint32_t fromIp, uint64_t atUs) override;
    void ticked(uint32_t nowMs) override;

private:
    void pollReply(uint32_t ip);

    std::vector<uint32_t> polls_;  // controllers to answer, under lock_
};

}  // namespace tesserkit
