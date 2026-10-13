#pragma once

#include <string>

#include "tesserkit/device.h"

namespace tesser {
class Pending;
}

namespace tesserkit {

// The argument of /sys/udp/send.
struct UdpSend {
    std::string path;   // the node to set or call on the others
    std::string value;  // its JSON value or argument ("null" or empty for an action without one)
    std::string to;     // "192.168.1.59" or "192.168.1.59:8484"; empty: every node of the local networks
};
void describe(tesser::Object& o, UdpSend& s);

// /sys/udp: the API over UDP on the local networks, Ethernet or Wi-Fi
// (docs/DESIGN.md section 5.18), through TesserAPI's UdpTransport. The
// device serves its tree to the LAN, mounts the TesserAPI nodes it finds
// there under /peers, and writes to all of them at once with one broadcast.
class UdpApi : public Service {
public:
    static constexpr uint16_t kDefaultPort = 8484;

    explicit UdpApi(Device& dev) : Service(dev, "udp") {}

    // Factory defaults, before Device::begin().
    UdpApi& enable(bool on);
    UdpApi& port(uint16_t port);
    // The level of a node on the LAN that presents no token (default User).
    UdpApi& peerAccess(tesser::Access level);
    // Mount the nodes found under /peers (default true).
    UdpApi& mountPeers(bool on);
    // Calls in flight to other nodes (default 8, fixed at the first start):
    // at least the number of nodes a master streams to at once
    // (RemoteNode::stream()).
    UdpApi& maxCalls(size_t n);
    // By default (false), no worker task of its own: the receive task stays,
    // and requests from the nodes are handled in the device's task, woken as
    // they arrive, which saves 6 KB of heap for about 10 ms per round trip.
    // true: a worker task.
    UdpApi& ownTask(bool own);

    void describe(tesser::Object& o) override;

    // Writes `valueJson` (empty: null) to `path` on every node of the local
    // networks at once, or on the node at `to`. A broadcast isn't answered,
    // and this node doesn't apply it; a write to one node replies through
    // `reply` when given. Under the API lock.
    tesser::Check send(const std::string& path, const std::string& valueJson, const std::string& to,
                       tesser::Pending* reply = nullptr);
    bool running() const { return running_; }

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    enum class Level : uint8_t { Public, User, Admin };

    void begin();
    void end();
    void refresh();

    bool enabled_ = true;
    uint16_t port_ = kDefaultPort;
    Level peerAccess_ = Level::User;
    bool mountPeers_ = true;
    size_t maxCalls_ = 8;
    bool ownTask_ = false;

    UdpApiTransport* transport_ = nullptr;
    tesser::Object* peersObject_ = nullptr;
    bool running_ = false;
    bool begun_ = false;    // the first start, once the network has an address
    bool mounted_ = false;  // peers are mounted for the transport's lifetime
    bool restart_ = false;
    uint32_t startMs_ = 0;
    uint32_t polledAtMs_ = 0;
    uint32_t sent_ = 0, received_ = 0, retransmits_ = 0, failed_ = 0;

    tesser::Node* enabledNode_ = nullptr;
    tesser::Node* portNode_ = nullptr;
    tesser::Node* accessNode_ = nullptr;
    tesser::Node* mountNode_ = nullptr;
    tesser::Node* restartNode_ = nullptr;
    tesser::Node* statusNode_ = nullptr;
    uint16_t enabledGen_ = 0, portGen_ = 0, accessGen_ = 0, mountGen_ = 0;
};

}  // namespace tesserkit
