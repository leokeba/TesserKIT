#pragma once

#include <array>
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
    char bssid[18] = "";  // "3a:07:16:43:fd:24": only this access point of the network; empty: any
    StaticIp ip;
};
void describe(tesser::Object& o, WifiNetwork& n);

enum class ApMode : uint8_t { Always, Fallback, Never };
// When the station joins a network: always, never, or while Ethernet has no
// DHCP lease (Fallback, the default with a board; Always without).
enum class StationMode : uint8_t { Always, Never, Fallback };

// Ethernet wiring of known boards, for Network::ethernet() (as WLED lists
// them).
namespace boards {
// Gledopto's controllers with Ethernet: LAN8720 at 1, its oscillator powered
// by GPIO 5.
EthernetBoard gledopto();
// WT32-ETH01: LAN8720 at 1, its oscillator powered by GPIO 16.
EthernetBoard wt32Eth01();
// Olimex ESP32-POE and ESP32-POE-ISO: LAN8720 at 0, powered by GPIO 12,
// clocked by the ESP32 on GPIO 17.
EthernetBoard olimexEsp32Poe();
// Espressif's ESP32-Ethernet-Kit: IP101 at 1, powered by GPIO 5.
EthernetBoard esp32EthernetKit();
// A W5500 module on SPI, wired to these GPIOs; without `irq` it is polled.
// spin-sequencer's XIAO ESP32-S3: w5500(7, 8, 9, 44, 43) (D8, D9, D10, D7, D6).
EthernetBoard w5500(int8_t sck, int8_t miso, int8_t mosi, int8_t cs, int8_t irq = -1, int8_t reset = -1);
}  // namespace boards

// "a.b.c.d" as (a << 24) | (b << 16) | (c << 8) | d; false if it isn't one.
bool parseIpv4(const std::string& text, uint32_t& out);
std::string formatIpv4(uint32_t ip);

// /sys/network: the Wi-Fi station, Ethernet, the access point and its
// captive portal, mDNS (docs/DESIGN.md section 5.2).
class Network : public Service {
public:
    explicit Network(Device& dev) : Service(dev, "network") {}

    // Factory defaults, before Device::begin(): used when the stored state
    // doesn't set them.
    Network& defaultNetwork(const char* ssid, const char* password);
    Network& apMode(ApMode mode);
    Network& stationMode(StationMode mode);
    Network& apPassword(const char* password);
    Network& apChannel(uint8_t channel);
    // Caps the radio's transmit power (weak supplies brown out at 20 dBm).
    Network& txPower(int8_t dBm);
    // The station's power save (wifi/powerSave): PowerSave::None for the
    // lowest latency, at about 100 mA more.
    Network& powerSave(PowerSave mode);
    // The board's Ethernet (boards::gledopto(), ...): /sys/network/ethernet.
    // The station keeps away while it has a DHCP lease (StationMode::Fallback).
    Network& ethernet(const EthernetBoard& board);

    void describe(tesser::Object& o) override;
    void migrate(uint32_t from, JsonObject settings) override;

    // The station joined a network.
    bool connected() const { return sta_ == Sta::Connected; }
    bool ethernetConnected() const { return ethOnline_; }
    // Reachable on a network: over Wi-Fi or Ethernet.
    bool online() const { return connected() || ethOnline_; }
    bool apActive() const { return apUp_; }

    // Station timing (docs/DESIGN.md section 5.2).
    // An attempt has this long to associate (within a deferred scan's
    // wait), then this long more for an address: DHCP with lwIP's address
    // conflict detection (ESP-IDF's default) takes 5 to 10 s.
    static constexpr uint32_t kConnectTimeoutMs = 10000;
    static constexpr uint32_t kAddressTimeoutMs = 20000;
    static constexpr uint32_t kScanTimeoutMs = 15000;
    static constexpr uint32_t kApFallbackMs = 15000;  // not joined: the AP comes up
    static constexpr uint32_t kApLingerMs = 30000;    // joined: the AP goes down
    // The station waits this long for Ethernet at start and after its link
    // comes up (time for DHCP), before joining a network.
    static constexpr uint32_t kEthWaitMs = 5000;
    // How often the interfaces' state is read back from the platform: the
    // link, the addresses and the access point's clients. Events only make
    // a change known sooner: one the device's queue lost (or an address that
    // came without one, link-local from AutoIP) is caught up within this.
    static constexpr uint32_t kPollMs = 1000;
    // A changed setting is applied once the settings have been still this
    // long: the reply to the write goes out first (TesserAPI sends replies
    // after the API lock, so a new address could otherwise come first), and
    // a few quick patches make one reconnection.
    static constexpr uint32_t kApplyAfterMs = 150;

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
    void ethLink(bool up, uint32_t nowMs);
    void ethGotIp(uint32_t nowMs);
    void ethDown(uint32_t nowMs, uint32_t waitMs = 0);
    void ethReaddress(uint32_t nowMs);
    void poll(uint32_t nowMs);
    void setApClients(int32_t n);
    bool stationKeptAway() const;
    bool ethWaits(uint32_t nowMs) const;
    void applyStation(uint32_t nowMs);
    void failed(const std::string& why);
    void updateStatus(uint32_t nowMs);
    void updateAddress();
    void applyMdns();
    bool routeVia(NetIf& out) const;
    void updateRoute();
    void refillDns(uint32_t nowMs);
    StaConfig config(const WifiNetwork& n) const;
    const WifiNetwork* find(const std::string& ssid) const;
    uint32_t apFingerprint() const;
    uint32_t settingsFingerprint() const;
    uint32_t ethFingerprint() const;
    IpConfig ethConfig() const;
    std::string apSsid() const { return apSsid_.empty() ? dev_.hostname() : apSsid_; }

    Wifi* wifi_ = nullptr;
    Ethernet* eth_ = nullptr;  // with a board, on a chip that has the MAC
    IpStack* ipStack_ = nullptr;
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
    PowerSave powerSave_ = PowerSave::Min;
    StationMode staMode_ = StationMode::Fallback;  // Always without Ethernet
    bool ethWanted_ = false;
    bool ethEnabled_ = true;
    EthernetBoard ethBoard_;
    StaticIp ethStatic_;

    // Nodes whose writes the service follows, by generation.
    tesser::Node* networksNode_ = nullptr;
    tesser::Node* mdnsNode_ = nullptr;
    std::vector<tesser::Node*> apNodes_;
    tesser::Node* staStatus_ = nullptr;
    tesser::Node* apStatus_ = nullptr;
    tesser::EventNode* joined_ = nullptr;
    tesser::EventNode* failed_ = nullptr;
    tesser::Node* staModeNode_ = nullptr;
    tesser::Node* powerSaveNode_ = nullptr;
    tesser::Node* ethEnabledNode_ = nullptr;
    std::vector<tesser::Node*> ethIpNodes_;
    tesser::Node* ethStatus_ = nullptr;
    tesser::EventNode* ethJoined_ = nullptr;
    uint32_t networksGen_ = 0, hostnameGen_ = 0, mdnsGen_ = 0;
    uint32_t apGen_ = 0, ethGen_ = 0;
    uint32_t settingsSeen_ = 0, settingsAtMs_ = 0;
    uint32_t staModeGen_ = 0, ethEnabledGen_ = 0, powerSaveGen_ = 0;

    // Station.
    Sta sta_ = Sta::Idle;
    uint32_t sinceMs_ = 0;      // the attempt's start, then its association
    bool associated_ = false;   // the attempt waits for its address
    uint32_t retryAtMs_ = 0;
    uint8_t backoff_ = 0;
    std::vector<std::string> candidates_;
    size_t candidate_ = 0;
    StaConfig current_;  // the network being joined or joined
    std::vector<std::array<uint8_t, 6>> heard_;  // the access points the last scan heard
    bool scanning_ = false;
    bool scanForJoin_ = false;
    bool scanWanted_ = false;
    uint32_t scanStartMs_ = 0;
    std::vector<tesser::Pending> scanWaiters_;
    uint32_t connectedAtMs_ = 0;
    uint32_t lostAtMs_ = 0;
    std::string failure_;      // why the last attempt failed: "\"Home\": not found"
    std::string failureShown_; // the last one toasted
    std::string staState_;     // wifi/status/state

    // Station status.
    bool linkUp_ = false;
    std::string linkSsid_, linkIp_, linkBssid_;
    uint8_t linkChannel_ = 0;

    // Ethernet.
    bool ethLink_ = false;
    bool ethOnline_ = false;  // with an address
    bool ethLease_ = false;   // from DHCP (not static, not link-local)
    bool ethStaticOn_ = false;  // a static address is applied
    uint32_t ethGateway_ = 0;   // its address's gateway: the lease's router, or the static one
    std::string ethState_;      // ethernet/status/state
    uint32_t ethWaitUntilMs_ = 0;  // the station waits for Ethernet until then
    std::string ethIp_, ethMac_;
    uint16_t ethMbps_ = 0;

    uint32_t polledAtMs_ = 0;  // poll()
    // DNS: when each interface's lease was last renewed to refill the list
    // (by NetIf), so a silent DHCP server isn't asked again at once.
    uint32_t renewedAtMs_[2] = {};
    bool renewed_[2] = {};
    // The default route's interface, as last set (updateRoute()).
    bool routeSet_ = false;
    NetIf route_ = NetIf::Station;

    // Addresses, for Device::address().
    uint32_t staAddr_ = 0, ethAddr_ = 0, apAddr_ = 0;
    uint8_t ethMacBytes_[6] = {};

    // Access point.
    bool apUp_ = false;
    bool captive_ = false;
    std::string apIp_;
    int32_t apClients_ = 0;
};

}  // namespace tesserkit
