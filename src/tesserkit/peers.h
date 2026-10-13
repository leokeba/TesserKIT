#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tesserkit/device.h"

namespace tesser {
class DatagramEndpoint;
class LocalClient;
class RemoteNode;
struct PeerAddress;
}  // namespace tesser

namespace tesserkit {

// An element of /sys/peers/nodes: what is set for one device, by its id.
struct PeerSettings {
    std::string id;       // the device's station MAC: 12 hex digits (colons allowed)
    std::string name;     // its node's name under /peers when declared by address (empty: from the id)
    std::string order;    // "udp, nowtp": its own route order; empty: /sys/peers/order
    std::string address;  // "192.168.1.97" or "192.168.1.97:8484": reached over UDP without being discovered
    std::string user;     // this device's account on that node (empty: /sys/peers/account)
    std::string password;
};
void describe(tesser::Object& o, PeerSettings& s);

// /sys/peers/account: the account this device signs in with on the nodes.
struct PeerAccount {
    std::string name;  // empty: none
    std::string password;
};
void describe(tesser::Object& o, PeerAccount& a);

// /sys/peers: how this device reaches the nodes under /peers (docs/DESIGN.md
// section 5.19). A device reached over UDP and NowTP has one node there with
// a route per transport (TesserAPI's RemoteNode); this sets the order the
// routes are tried in, for every node and per device, declares devices by
// address, signs in to the nodes with an account, shows each node's state,
// and tells the application when a device that wrote here is gone from every
// transport.
class Peers : public Service {
public:
    static constexpr size_t kMaxNodes = 16;  // settings, and nodes shown
    static constexpr uint32_t kRefreshMs = 1000;
    // A sign-in that failed for a passing reason (no answer, too many
    // attempts) is tried again after this; so is one whose token is refused
    // soon after it was given.
    static constexpr uint32_t kSignInRetryMs = 30000;

    // `device` is the MAC of a device gone from every transport (UDP's and
    // NowTP's), in the device's task, under the API lock.
    using DeviceLostHandler = std::function<void(const tesser::PeerAddress& device)>;

    explicit Peers(Device& dev);
    ~Peers() override;

    // Factory default of the route order: "udp, nowtp", before Device::begin().
    Peers& order(const char* order);
    // Factory default of the account signed in with on every node.
    Peers& account(const char* name, const char* password);
    // A device that wrote here (a master) is gone: the application undoes
    // what it did. It owns UdpApi's and NowTp's DatagramEndpoint::onPeerLost().
    Peers& onDeviceLost(DeviceLostHandler fn);

    void describe(tesser::Object& o) override;

    // The object the nodes are mounted under (/peers), with UdpApi and NowTp.
    tesser::Object& nodes();

    // UdpApi and NowTp report the peers their endpoint loses through this,
    // once the endpoint exists.
    static void watch(Device& dev, tesser::DatagramEndpoint& endpoint);

protected:
    void start() override;
    void tick(uint32_t nowMs) override;
    void event(const Event& e) override;

private:
    // An element of /sys/peers/status.
    struct State {
        std::string id, name;
        bool online = false;
        std::string route;   // the transport in use
        std::string routes;  // every transport that reaches it, in the order tried
        uint32_t rtt = 0;    // ms, smoothed, on the route in use (0: no call yet)
        int32_t heard = -1;  // s since the device was last heard (-1: never)
        std::string access;  // the level the node grants this device's token ("" without one)
        std::string signIn;  // how signing in went: "signed in as show", "wrong name or password"
    };
    // Signing in to one device.
    struct SignIn {
        std::string id;
        std::string credentials;  // name and password signed in with, or being
        enum class Phase : uint8_t { Needed, Pending, Done, Stopped } phase = Phase::Needed;
        uint32_t retryAtMs = 0;
        uint32_t signedInMs = 0;
        bool hasToken = false;
        std::string text;
    };

    void refresh(uint32_t nowMs);
    void signIn(tesser::RemoteNode& node, const PeerSettings* settings, uint32_t nowMs, State& state);
    void login(const std::string& nodeName, const std::string& id, const std::string& user,
               const std::string& password);
    SignIn& signInFor(const std::string& id);
    void declare();
    void applyDefaultOrder();
    const PeerSettings* settingsFor(const tesser::PeerAddress& device) const;
    std::vector<tesser::DatagramEndpoint*> endpoints() const;

    std::string order_;
    std::vector<PeerSettings> settings_;
    std::vector<State> states_;
    PeerAccount account_;
    std::vector<SignIn> signIns_;
    std::unique_ptr<tesser::LocalClient> client_;  // signs in, Admin
    DeviceLostHandler lost_;

    std::vector<std::string> declared_; // "id address" pairs done (nodes are never removed)
    tesser::Object* peers_ = nullptr;
    bool dirty_ = false;
    uint32_t refreshedMs_ = 0;

    tesser::Node* orderNode_ = nullptr;
    tesser::Node* settingsNode_ = nullptr;
    tesser::Node* accountNode_ = nullptr;
    tesser::Node* statusNode_ = nullptr;
    tesser::EventNode* lostEvent_ = nullptr;
    tesser::EventNode* reachedEvent_ = nullptr;
    uint16_t orderGen_ = 0, settingsGen_ = 0, accountGen_ = 0;
};

}  // namespace tesserkit
