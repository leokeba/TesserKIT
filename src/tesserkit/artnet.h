#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tesser/client.h"
#include "tesserkit/device.h"

namespace tesserkit {

// An element of /sys/artnet/mappings: DMX channels written to a node.
struct ArtNetMapping {
    std::string path;     // the node to set
    uint16_t coarse = 0;  // channel 1–512; 0: unused
    uint16_t fine = 0;    // its low byte's channel; 0: 8 bits
    double min = 0, max = 1;
    // 1–255: a gate, written as the boolean coarse >= threshold.
    uint8_t threshold = 0;
};

// /sys/artnet: an Art-Net node mapping DMX channels of one universe to
// nodes of the tree (docs/DESIGN.md section 5.9).
class ArtNet : public Service {
public:
    static constexpr uint16_t kPort = 6454;
    static constexpr size_t kMaxMappings = 8;

    explicit ArtNet(Device& dev) : Service(dev, "artnet") {}

    // Factory defaults, before Device::begin().
    ArtNet& enable(bool on);
    ArtNet& universe(uint16_t portAddress);
    ArtNet& map(const ArtNetMapping& m);

    void describe(tesser::Object& o) override;

    // The value a mapping writes for a DMX frame, as JSON ("" if the frame
    // doesn't reach its channels). `kind` is the target value's.
    static std::string mapped(const ArtNetMapping& m, const uint8_t* dmx, size_t len, tesser::ValueKind kind);

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    void receive(uint32_t nowMs);
    void apply();
    void pollReply(uint32_t ip);
    void failed(const std::string& error);

    bool enabled_ = true;
    uint16_t universe_ = 0;
    std::vector<ArtNetMapping> mappings_;

    // Status.
    uint32_t packets_ = 0;
    uint16_t lastUniverse_ = 0;
    std::string error_;
    tesser::Node* status_ = nullptr;
    tesser::EventNode* refused_ = nullptr;

    tesser::Node* universeNode_ = nullptr;
    tesser::Node* mappingsNode_ = nullptr;
    uint32_t settingsGen_ = 0;
    Udp* udp_ = nullptr;
    std::unique_ptr<UdpSocket> socket_;
    uint32_t openedAtMs_ = 0;
    bool opening_ = false;
    std::unique_ptr<tesser::LocalClient> client_;
    uint8_t frame_[512] = {};
    size_t frameLen_ = 0;
    bool fresh_ = false;  // a frame since the last apply()
    bool everFrame_ = false;
    uint32_t frameAtMs_ = 0;
    uint32_t nowMs_ = 0;
    std::vector<std::string> sent_;  // per mapping: the last value written
};

}  // namespace tesserkit
