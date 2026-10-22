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
    // Calls in flight to other nodes (default 8): at least the number of
    // nodes a master streams to at once (RemoteNode::stream()).
    NowTp& maxCalls(size_t n);
    // By default (false), no worker task of its own: requests from the nodes
    // are handled in the device's task, woken as they arrive, which saves
    // 6 KB of heap at no measurable latency. true: a worker task.
    NowTp& ownTask(bool own);
    // Network time: "auto" (default), "this", or the MAC of the node whose
    // clock is the network time (docs/DESIGN.md section 5.11).
    NowTp& timeReference(const char* ref);
    // Between synchronization rounds once synchronized (default 5 s).
    NowTp& timeInterval(uint32_t seconds);

    void describe(tesser::Object& o) override;

    bool started() const { return started_; }

    // Network time in µs: the reference node's clock, the same instant on
    // every synchronized node. The local clock's while NowTP is off or
    // unsynchronized. From any task.
    uint64_t networkTimeUs() const;
    // The local clock's value (esp_timer_get_time()) at network time
    // `networkUs`: an action scheduled there happens at the same instant on
    // every synchronized node.
    int64_t localTimeUs(uint64_t networkUs) const;
    // Synchronized with the reference, or the reference: as of the last
    // second (the service polls the radio), under the API lock.
    bool timeSynced() const { return status_.timeSynced; }

protected:
    void start() override;
    void tick(uint32_t nowMs) override;
    void event(const Event& e) override;
    void factoryReset() override;
    // ESP-NOW loses frames while the station's modem sleeps: Network's power
    // save defaults to none (docs/DESIGN.md section 5.2).
    bool needsStationAwake() const override { return true; }

private:
    struct Node {
        std::string name, mac;
        int32_t rssi = 0;
        uint8_t channel = 0;
        bool otherChannel = false, tesser = false, authenticated = false;
    };

    uint32_t restartGen() const;
    bool settled(uint32_t nowMs) const;
    void begin();
    void refresh();
    void setPairing(const std::string& s);
    void applyTimeReference();
    tesser::Check pairing(bool accept);

    bool enabled_ = true;
    bool mountPeers_ = true;
    uint8_t channel_ = 1;
    bool longRange_ = false;
    int8_t txPower_ = 0;
    size_t maxCalls_ = 8;
    bool ownTask_ = false;
    std::string pairingCode_;
    std::string timeReference_ = "auto";
    uint32_t timeInterval_ = 5;  // s

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
    tesser::Node* timeNode_ = nullptr;
    tesser::Node* timeReferenceNode_ = nullptr;
    tesser::Node* timeIntervalNode_ = nullptr;
    tesser::Node* pairingNode_ = nullptr;
    tesser::Node* restartNode_ = nullptr;
    std::vector<tesser::Node*> restartSettings_;  // what applies at the next boot
    uint32_t enabledGen_ = 0, channelGen_ = 0, longRangeGen_ = 0, timeReferenceGen_ = 0, timeIntervalGen_ = 0;
    uint32_t restartGen_ = 0;
    uint32_t polledAtMs_ = 0;
    uint32_t startMs_ = 0;
};

}  // namespace tesserkit
