#pragma once

#include <string>
#include <vector>

#include "tesserkit/device.h"

namespace tesserkit {

struct StaticIp {
    bool enabled = false;
    std::string ip, mask = "255.255.255.0", gateway, dns;
};

// A saved Wi-Fi network: an element of /sys/network/wifi/networks.
struct WifiNetwork {
    char ssid[33] = "";
    char password[65] = "";
    StaticIp ip;
};
void describe(tesser::Object& o, WifiNetwork& n);

enum class ApMode : uint8_t { Always, Fallback, Never };

// "a.b.c.d" as (a << 24) | (b << 16) | (c << 8) | d; false if it isn't one.
bool parseIpv4(const std::string& text, uint32_t& out);
std::string formatIpv4(uint32_t ip);

// /sys/network: the Wi-Fi station, the access point and its captive portal,
// mDNS (docs/DESIGN.md section 5.2).
class Network : public Service {
public:
    explicit Network(Device& dev) : Service(dev, "network") {}

    // Factory defaults, before Device::begin(): used when the stored state
    // doesn't set them.
    Network& defaultNetwork(const char* ssid, const char* password);
    Network& apMode(ApMode mode);
    Network& apPassword(const char* password);
    Network& apChannel(uint8_t channel);
    // Caps the radio's transmit power (weak supplies brown out at 20 dBm).
    Network& txPower(int8_t dBm);

    void describe(tesser::Object& o) override;

    bool connected() const { return sta_ == Sta::Connected; }
    bool apActive() const { return apUp_; }

    // Station timing (docs/DESIGN.md section 5.2).
    static constexpr uint32_t kConnectTimeoutMs = 10000;  // within a deferred scan's wait
    static constexpr uint32_t kScanTimeoutMs = 15000;
    static constexpr uint32_t kApFallbackMs = 15000;  // not joined: the AP comes up
    static constexpr uint32_t kApLingerMs = 30000;    // joined: the AP goes down

protected:
    void start() override;
    void tick(uint32_t nowMs) override;
    void event(const Event& e) override;

private:
    enum class Sta : uint8_t { Idle, Scanning, Connecting, Connected, Waiting };

    void join(uint32_t nowMs);
    bool startScan(bool forJoin, uint32_t nowMs);
    void scanDone(const std::vector<ScanResult>& results, uint32_t nowMs);
    void connectNext(uint32_t nowMs);
    void wait(uint32_t nowMs);
    void connected(uint32_t nowMs);
    void lost(uint32_t nowMs);
    void checkSettings(uint32_t nowMs);
    void updateAp(uint32_t nowMs);
    void reportState();
    void applyMdns();
    StaConfig config(const WifiNetwork& n) const;
    const WifiNetwork* find(const std::string& ssid) const;
    uint32_t apFingerprint() const;
    std::string apSsid() const { return apSsid_.empty() ? dev_.hostname() : apSsid_; }

    Wifi* wifi_ = nullptr;
    Mdns* mdnsService_ = nullptr;

    // Settings (persisted).
    std::vector<WifiNetwork> networks_;
    ApMode apMode_ = ApMode::Fallback;
    std::string apSsid_;  // empty: the hostname
    std::string apPassword_;
    uint8_t apChannel_ = 1;
    bool apHidden_ = false;
    bool mdns_ = true;
    int8_t txPower_ = 0;

    // Nodes whose writes the service follows, by generation.
    tesser::Node* networksNode_ = nullptr;
    tesser::Node* hostnameNode_ = nullptr;
    tesser::Node* mdnsNode_ = nullptr;
    std::vector<tesser::Node*> apNodes_;
    tesser::Node* staStatus_ = nullptr;
    tesser::Node* apStatus_ = nullptr;
    tesser::EventNode* joined_ = nullptr;
    uint16_t networksGen_ = 0, hostnameGen_ = 0, mdnsGen_ = 0;
    uint32_t apGen_ = 0;

    // Station.
    Sta sta_ = Sta::Idle;
    uint32_t sinceMs_ = 0;
    uint32_t retryAtMs_ = 0;
    uint8_t backoff_ = 0;
    std::vector<std::string> candidates_;
    size_t candidate_ = 0;
    StaConfig current_;  // the network being joined or joined
    bool scanning_ = false;
    bool scanForJoin_ = false;
    bool scanWanted_ = false;
    uint32_t scanStartMs_ = 0;
    std::vector<tesser::Pending> scanWaiters_;
    uint32_t connectedAtMs_ = 0;
    uint32_t lostAtMs_ = 0;

    // Station status.
    bool linkUp_ = false;
    std::string linkSsid_, linkIp_, linkBssid_;
    uint8_t linkChannel_ = 0;

    // Access point.
    bool apUp_ = false;
    bool captive_ = false;
    std::string apIp_;
    int32_t apClients_ = 0;
};

}  // namespace tesserkit
