#pragma once

#include <string>
#include <vector>

#include "tesserkit/device.h"

namespace tesserkit {

// /sys/nowtp: the nodes nearby over ESP-NOW (docs/DESIGN.md section 5.11),
// through NowTP and TesserAPI's NowTpTransport. The device serves its tree to
// them, and mounts the TesserAPI nodes it finds under /peers, so its
// interface shows a page per node (a UI host). Pairing hands nodes the
// installation key; nodes holding it are trusted (Admin).
class NowTp : public Service {
public:
    static constexpr size_t kMaxNodes = 20;  // ESP-NOW's peers
    static constexpr uint32_t kPairingMs = 60000;

    explicit NowTp(Device& dev) : Service(dev, "nowtp") {}

    // Factory defaults, before Device::begin().
    NowTp& enable(bool on);
    // Mount the nodes found under /peers (default true).
    NowTp& mountPeers(bool on);
    NowTp& channel(uint8_t ch);
    NowTp& longRange(bool on);
    NowTp& txPower(int8_t dBm);
    NowTp& pairingCode(const char* code);

    void describe(tesser::Object& o) override;

    bool started() const { return started_; }

protected:
    void start() override;
    void tick(uint32_t nowMs) override;
    void event(const Event& e) override;
    void factoryReset() override;

private:
    struct Node {
        std::string name, mac;
        int32_t rssi = 0;
        uint8_t channel = 0;
        bool tesser = false, authenticated = false;
    };

    uint32_t restartGen() const;
    bool settled(uint32_t nowMs) const;
    void begin();
    void refresh();
    void setPairing(const std::string& s);
    tesser::Check pairing(bool accept);

    bool enabled_ = true;
    bool mountPeers_ = true;
    uint8_t channel_ = 1;
    bool longRange_ = false;
    int8_t txPower_ = 0;
    std::string pairingCode_;

    NowTpRadio* radio_ = nullptr;
    tesser::Object* peersObject_ = nullptr;
    bool started_ = false;
    bool begun_ = false;    // the first start, once the network settled
    bool restart_ = false;  // a change waits for a restart
    bool pairingBusy_ = false;
    std::string pairing_;
    NowTpStatus status_;
    std::vector<Node> nodes_;

    tesser::Node* enabledNode_ = nullptr;
    tesser::Node* channelNode_ = nullptr;
    tesser::Node* longRangeNode_ = nullptr;
    tesser::Node* nodesNode_ = nullptr;
    tesser::Node* statusNode_ = nullptr;
    tesser::Node* pairingNode_ = nullptr;
    tesser::Node* restartNode_ = nullptr;
    std::vector<tesser::Node*> restartSettings_;  // what applies at the next boot
    uint16_t enabledGen_ = 0, channelGen_ = 0, longRangeGen_ = 0;
    uint32_t restartGen_ = 0;
    uint32_t polledAtMs_ = 0;
    uint32_t startMs_ = 0;
};

}  // namespace tesserkit
