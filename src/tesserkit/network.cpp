#include "tesserkit/network.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <iterator>
#include <utility>

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

// Waits between rounds of failed attempts.
constexpr uint32_t kBackoffMs[] = {1000, 2000, 5000, 10000, 30000};
constexpr size_t kBackoffSteps = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);

Check checkIp(const std::string& text) {
    uint32_t ip;
    return text.empty() || parseIpv4(text, ip) ? Check::ok() : Check::fail(Status::InvalidValue, "expected a.b.c.d");
}

// An IPv4 field of a static configuration: empty, or a.b.c.d.
tesser::Node& ipField(tesser::Object& o, const char* name, const char* label, std::string& field) {
    return o.value(name, field).validate(checkIp).label(label).ui("visibleWhen", "enabled=true");
}

// A static address's settings, collapsed: DHCP unless enabled. Its values'
// nodes go to `nodes` when given.
tesser::Object& describeStatic(tesser::Object& parent, StaticIp& ip, std::vector<tesser::Node*>* nodes = nullptr) {
    auto& s = parent.object("static").label("Static IP").ui("collapsed");
    tesser::Node* n[] = {
        &s.value("enabled", ip.enabled).label("Use a static address"),
        &ipField(s, "ip", "Address", ip.ip),
        &ipField(s, "mask", "Netmask", ip.mask),
        &ipField(s, "gateway", "Gateway", ip.gateway),
        &ipField(s, "dns", "DNS server", ip.dns),
    };
    if (nodes) nodes->assign(std::begin(n), std::end(n));
    return s;
}

// The setup a static address's settings make: DHCP unless they are enabled
// and complete. A missing DNS server is the gateway.
IpConfig ipConfig(const StaticIp& s, const char* what) {
    IpConfig c;
    if (!s.enabled) return c;
    c.staticIp = parseIpv4(s.ip, c.ip) && parseIpv4(s.mask, c.mask) && parseIpv4(s.gateway, c.gateway);
    if (!c.staticIp) {
        tesser::logWarning("tesserkit: incomplete static address for %s, using DHCP", what);
        return IpConfig();
    }
    if (!parseIpv4(s.dns, c.dns)) c.dns = c.gateway;
    return c;
}

std::string formatMac(const uint8_t* m) {
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

bool linkLocal(uint32_t ip) { return ip >> 16 == 0xA9FE; }  // 169.254/16

// Why a join failed, from ESP-IDF's disconnection reasons (which the fake
// platform uses too).
std::string joinFailure(int32_t reason) {
    switch (reason) {
        case 201:  // NO_AP_FOUND
        case 210:  // NO_AP_FOUND_W_COMPATIBLE_SECURITY
        case 211:  // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
        case 212:  // NO_AP_FOUND_IN_RSSI_THRESHOLD
            return "not found";
        case 2:    // AUTH_EXPIRE
        case 14:   // MIC_FAILURE
        case 15:   // 4WAY_HANDSHAKE_TIMEOUT
        case 202:  // AUTH_FAIL
        case 204:  // HANDSHAKE_TIMEOUT
            return "wrong password?";
        case 205:  // CONNECTION_FAIL
            return "refused the connection";
        default:
            return "failed (reason " + std::to_string(reason) + ")";
    }
}

bool validHostname(const std::string& h) {
    if (h.empty() || h.size() > 32 || h.front() == '-' || h.back() == '-') return false;
    for (char c : h) {
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
    }
    return true;
}

}  // namespace

bool parseIpv4(const std::string& text, uint32_t& out) {
    uint32_t ip = 0;
    int parts = 0;
    const char* p = text.c_str();
    while (parts < 4) {
        if (!isdigit(static_cast<unsigned char>(*p))) return false;
        uint32_t part = 0;
        int digits = 0;
        while (isdigit(static_cast<unsigned char>(*p))) {
            part = part * 10 + static_cast<uint32_t>(*p++ - '0');
            if (++digits > 3 || part > 255) return false;
        }
        ip = ip << 8 | part;
        if (++parts < 4 && *p++ != '.') return false;
    }
    if (*p) return false;
    out = ip;
    return true;
}

std::string formatIpv4(uint32_t ip) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(ip >> 24), static_cast<unsigned>(ip >> 16 & 255),
             static_cast<unsigned>(ip >> 8 & 255), static_cast<unsigned>(ip & 255));
    return buf;
}

void describe(tesser::Object& o, WifiNetwork& n) {
    o.value("ssid", n.ssid).label("Network");
    o.value("password", n.password).secret();
    describeStatic(o, n.ip);
}

namespace boards {

namespace {
EthernetBoard board(EthernetBoard::Phy phy, int8_t address, int8_t mdc, int8_t mdio, int8_t power,
                    EthernetBoard::Clock clock, int8_t clockGpio) {
    EthernetBoard b;
    b.phy = phy;
    b.address = address;
    b.mdc = mdc;
    b.mdio = mdio;
    b.power = power;
    b.clock = clock;
    b.clockGpio = clockGpio;
    return b;
}
}  // namespace

using Phy = EthernetBoard::Phy;
using Clock = EthernetBoard::Clock;
EthernetBoard gledopto() { return board(Phy::Lan8720, 1, 23, 33, 5, Clock::In, 0); }
EthernetBoard wt32Eth01() { return board(Phy::Lan8720, 1, 23, 18, 16, Clock::In, 0); }
EthernetBoard olimexEsp32Poe() { return board(Phy::Lan8720, 0, 23, 18, 12, Clock::Out, 17); }
EthernetBoard esp32EthernetKit() { return board(Phy::Ip101, 1, 23, 18, 5, Clock::In, 0); }

}  // namespace boards

Network& Network::defaultNetwork(const char* ssid, const char* password) {
    WifiNetwork n;
    snprintf(n.ssid, sizeof(n.ssid), "%s", ssid ? ssid : "");
    snprintf(n.password, sizeof(n.password), "%s", password ? password : "");
    networks_.push_back(n);
    return *this;
}

Network& Network::apMode(ApMode mode) {
    apMode_ = mode;
    return *this;
}

Network& Network::stationMode(StationMode mode) {
    staMode_ = mode;
    return *this;
}

Network& Network::apPassword(const char* password) {
    apPassword_ = password ? password : "";
    return *this;
}

Network& Network::apChannel(uint8_t channel) {
    apChannel_ = channel;
    return *this;
}

Network& Network::txPower(int8_t dBm) {
    txPower_ = dBm;
    return *this;
}

Network& Network::ethernet(const EthernetBoard& board) {
    ethWanted_ = true;
    ethBoard_ = board;
    // Here rather than in describe(): applications without Ethernet don't
    // link its driver.
    Platform& p = dev_.platform();
    if (!p.ethernet) p.ethernet = espEthernet();
    return *this;
}

void Network::describe(tesser::Object& o) {
    Device& dev = dev_;
    o.label("Network").ui("icon", "wifi").writeAccess(Access::Admin);
    hostnameNode_ = &o.value("hostname", [&dev] { return dev.hostname(); },
                             [&dev](const std::string& h) { dev.hostname(h.c_str()); })
                         .validate([](const std::string& h) {
                             return validHostname(h)
                                        ? Check::ok()
                                        : Check::fail(Status::InvalidValue, "1 to 32 letters, digits and inner hyphens");
                         })
                         .persist()
                         .ui("advanced");

    // First: preferred while it has an address.
    eth_ = ethWanted_ ? dev.platform().ethernet : nullptr;
    if (ethWanted_ && !eth_) tesser::logWarning("tesserkit: no Ethernet on this chip");
    if (eth_) {
        auto& e = o.object("ethernet").label("Ethernet").ui("card");
        ethEnabledNode_ = &e.value("enabled", ethEnabled_).persist().label("Use Ethernet");
        auto& es = e.object("status").label("Connection");
        ethStatus_ = &es;
        es.value("state", static_cast<const std::string&>(ethState_));
        es.value("link", static_cast<const bool&>(ethLink_)).label("Cable");
        es.value("connected", static_cast<const bool&>(ethOnline_));
        es.value("ip", static_cast<const std::string&>(ethIp_)).label("Address");
        es.value("speed", static_cast<const uint16_t&>(ethMbps_)).unit("Mbps");
        es.value("mac", static_cast<const std::string&>(ethMac_)).label("MAC").ui("advanced");
        ethJoined_ = &e.event("joined").label("Joined").ui("toast", "ok").ui("hidden");
        describeStatic(e, ethStatic_, &ethIpNodes_).ui("form").persist();
    }

    auto& w = o.object("wifi").label("Wi-Fi").ui("card");
    auto& st = w.object("status").label("Connection");
    staStatus_ = &st;
    st.value("state", static_cast<const std::string&>(staState_));
    st.value("connected", static_cast<const bool&>(linkUp_));
    st.value("ssid", static_cast<const std::string&>(linkSsid_)).label("Network");
    st.value("ip", static_cast<const std::string&>(linkIp_)).label("Address");
    st.value("rssi", [this] {
          StaInfo i;
          return wifi_ && linkUp_ && wifi_->staInfo(i) ? i.rssi : int8_t(0);
      })
        .label("Signal")
        .unit("dBm")
        .watch()
        .ui("widget", "chart");
    st.value("channel", static_cast<const uint8_t&>(linkChannel_));
    st.value("bssid", static_cast<const std::string&>(linkBssid_)).label("BSSID").ui("advanced");
    // For clients on the access point, which must follow the new address.
    joined_ = &w.event("joined").label("Joined").ui("toast", "ok").ui("hidden");
    failed_ = &w.event("failed").label("Couldn't join").ui("toast", "warn").ui("hidden");
    // Fallback only with Ethernet: the station then keeps away while
    // Ethernet has a DHCP lease.
    if (eth_) {
        staModeNode_ = &w.value("mode", staMode_, {"always", "never", "fallback"})
                            .persist()
                            .label("Station")
                            .ui("labels", "Always,Never,As a fallback to Ethernet");
    } else {
        if (staMode_ == StationMode::Fallback) staMode_ = StationMode::Always;
        staModeNode_ = &w.value("mode", staMode_, {"always", "never"}).persist().label("Station").ui("labels", "On,Off");
    }

    networksNode_ = &w.list("networks", networks_)
                         .key("ssid")
                         .maxSize(5)
                         .readAccess(Access::User)
                         .writeAccess(Access::Admin)
                         .persist()
                         .label("Saved networks");
    w.action("scan", [this](tesser::Call& call) {
         tesser::Pending p = call.defer();
         if (!p) return;
         scanWaiters_.push_back(std::move(p));
         scanWanted_ = true;
     }).label("Scan");
    // Cards of their own, like Wi-Fi: the access point is set up apart from
    // the station.
    auto& ap = o.object("ap").label("Access point").ui("form").ui("card");
    ap.value("mode", apMode_, {"always", "fallback", "never"})
        .persist()
        .ui("labels", "Always,When offline,Never");
    apNodes_.clear();
    apNodes_.push_back(&ap.value("ssid", apSsid_).persist().label("Name").ui("placeholder", "the hostname"));
    apNodes_.push_back(&ap.value("password", apPassword_)
                           .validate([](const std::string& v) {
                               return v.empty() || (v.size() >= 8 && v.size() <= 63)
                                          ? Check::ok()
                                          : Check::fail(Status::InvalidValue, "empty, or 8 to 63 characters");
                           })
                           .secret()
                           .persist()
                           .ui("placeholder", "none: an open network"));
    apNodes_.push_back(&ap.value("channel", apChannel_).range(1, 13).persist());
    apNodes_.push_back(&ap.value("hidden", apHidden_).persist().ui("advanced"));
    auto& as = ap.object("status").label("Status");
    apStatus_ = &as;
    as.value("active", static_cast<const bool&>(apUp_));
    as.value("ip", static_cast<const std::string&>(apIp_)).label("Address");
    as.value("clients", static_cast<const int32_t&>(apClients_));

    Platform& p = dev.platform();
#if !defined(TESSERKIT_NO_MDNS)
    if (!p.mdns) p.mdns = espMdns();
#endif
    mdnsService_ = p.mdns;
    if (mdnsService_) mdnsNode_ = &o.value("mdns", mdns_).persist().label("mDNS").ui("advanced");
}

void Network::start() {
    Platform& p = dev_.platform();
    if (!p.wifi) p.wifi = espWifi();
    wifi_ = p.wifi;
    if (!wifi_) {
        tesser::logError("tesserkit: Network has no Wi-Fi");
        return;
    }
    Device& dev = dev_;
    wifi_->begin(dev.hostname().c_str(), [&dev](Event e) { dev.post(std::move(e)); });
    if (txPower_) wifi_->txPower(txPower_);
    uint32_t now = static_cast<uint32_t>(dev.core().uptimeMs());
    if (eth_) {
        if (eth_->begin(ethBoard_, dev.hostname().c_str(), ethConfig(), [&dev](Event e) { dev.post(std::move(e)); })) {
            EthInfo i;
            if (eth_->info(i)) {
                memcpy(ethMacBytes_, i.mac, sizeof(ethMacBytes_));
                ethMac_ = formatMac(i.mac);
            }
            ethStaticOn_ = ethConfig().staticIp;
            if (!ethEnabled_) eth_->enabled(false);
            ethWaitUntilMs_ = now + kEthWaitMs;
        } else {
            tesser::logError("tesserkit: Ethernet didn't start");
            eth_ = nullptr;
            ethState_ = "didn't start";
        }
        ethStatus_->changed();
    }
    networksGen_ = networksNode_->generation();
    hostnameGen_ = hostnameNode_->generation();
    if (mdnsNode_) mdnsGen_ = mdnsNode_->generation();
    apGen_ = apFingerprint();
    ethGen_ = ethFingerprint();
    staModeGen_ = staModeNode_->generation();
    if (ethEnabledNode_) ethEnabledGen_ = ethEnabledNode_->generation();
    applyMdns();
    lostAtMs_ = now;
    join(now);
    updateAp(now);
    reportState();  // from the start: services waiting for the network see it
}

uint32_t Network::apFingerprint() const {
    // Generations only grow, so the sum changes whenever one of them does.
    uint32_t sum = 0;
    for (const tesser::Node* n : apNodes_) sum += n->generation();
    return sum;
}

uint32_t Network::ethFingerprint() const {
    uint32_t sum = 0;
    for (const tesser::Node* n : ethIpNodes_) sum += n->generation();
    return sum;
}

IpConfig Network::ethConfig() const { return ipConfig(ethStatic_, "Ethernet"); }

const WifiNetwork* Network::find(const std::string& ssid) const {
    for (const WifiNetwork& n : networks_) {
        if (ssid == n.ssid) return &n;
    }
    return nullptr;
}

StaConfig Network::config(const WifiNetwork& n) const {
    StaConfig c;
    static_cast<IpConfig&>(c) = ipConfig(n.ip, (std::string("\"") + n.ssid + "\"").c_str());
    c.ssid = n.ssid;
    c.password = n.password;
    return c;
}

bool Network::stationKeptAway() const {
    return staMode_ == StationMode::Never || (staMode_ == StationMode::Fallback && ethOnline_ && ethLease_);
}

// The station gives Ethernet time for a lease, at start and after a link.
bool Network::ethWaits(uint32_t nowMs) const {
    return staMode_ == StationMode::Fallback && eth_ && ethEnabled_ && !ethStaticOn_ &&
           static_cast<int32_t>(ethWaitUntilMs_ - nowMs) > 0;
}

// Joins, or leaves, as the mode and Ethernet now say.
void Network::applyStation(uint32_t nowMs) {
    if (stationKeptAway()) {
        if (sta_ == Sta::Connected || sta_ == Sta::Connecting) wifi_->disconnect();
        if (sta_ == Sta::Connected) lost(nowMs);
        sta_ = Sta::Idle;
    } else if (sta_ == Sta::Idle) {
        backoff_ = 0;
        join(nowMs);
    }
}

void Network::join(uint32_t nowMs) {
    if (networks_.empty() || stationKeptAway()) {
        sta_ = Sta::Idle;
        return;
    }
    if (ethWaits(nowMs)) {
        sta_ = Sta::Waiting;
        retryAtMs_ = ethWaitUntilMs_;
        return;
    }
    sta_ = Sta::Scanning;
    if (!startScan(true, nowMs)) wait(nowMs);
}

bool Network::startScan(bool forJoin, uint32_t nowMs) {
    if (forJoin) scanForJoin_ = true;
    if (scanning_) return true;
    if (!wifi_->scan()) {
        tesser::logWarning("tesserkit: Wi-Fi scan didn't start");
        return false;
    }
    scanning_ = true;
    scanStartMs_ = nowMs;
    return true;
}

void Network::scanDone(const std::vector<ScanResult>& results, uint32_t nowMs) {
    scanning_ = false;
    for (tesser::Pending& p : scanWaiters_) {
        p.replyWith([&results](tesser::JsonWriter& w) {
            w.beginArray();
            for (const ScanResult& r : results) {
                w.beginObject();
                w.key("ssid");
                w.string(r.ssid);
                w.key("rssi");
                w.integer(r.rssi);
                w.key("channel");
                w.uinteger(r.channel);
                w.key("secure");
                w.boolean(r.secure);
                w.endObject();
            }
            w.endArray();
        });
    }
    scanWaiters_.clear();
    if (!scanForJoin_) return;
    scanForJoin_ = false;
    if (sta_ != Sta::Scanning) return;
    // Saved networks in range, strongest first; if none is, every saved
    // network in order (hidden ones don't show in scans).
    candidates_.clear();
    std::vector<int> rssi;  // of each candidate, kept sorted strongest first (a few at most)
    for (const WifiNetwork& n : networks_) {
        int best = -1000;
        for (const ScanResult& r : results) {
            if (r.ssid == n.ssid && r.rssi > best) best = r.rssi;
        }
        if (best == -1000) continue;
        size_t at = 0;
        while (at < rssi.size() && rssi[at] >= best) at++;
        rssi.insert(rssi.begin() + static_cast<std::ptrdiff_t>(at), best);
        candidates_.insert(candidates_.begin() + static_cast<std::ptrdiff_t>(at), n.ssid);
    }
    if (candidates_.empty()) {
        for (const WifiNetwork& n : networks_) candidates_.push_back(n.ssid);
    }
    candidate_ = 0;
    connectNext(nowMs);
}

void Network::connectNext(uint32_t nowMs) {
    while (candidate_ < candidates_.size()) {
        const WifiNetwork* n = find(candidates_[candidate_++]);
        if (!n) continue;
        current_ = config(*n);
        if (wifi_->connect(current_)) {
            sta_ = Sta::Connecting;
            sinceMs_ = nowMs;
            return;
        }
    }
    // The round failed: told once for each new reason.
    if (!failure_.empty() && failure_ != failureShown_) {
        failureShown_ = failure_;
        failed_->emit("Couldn't join " + failure_);
    }
    wait(nowMs);
}

void Network::failed(const std::string& why) {
    failure_ = "\"" + current_.ssid + "\": " + why;
    tesser::logWarning("tesserkit: couldn't join %s", failure_.c_str());
}

void Network::wait(uint32_t nowMs) {
    sta_ = Sta::Waiting;
    retryAtMs_ = nowMs + kBackoffMs[backoff_];
    if (backoff_ + 1u < kBackoffSteps) backoff_++;
}

void Network::connected(uint32_t nowMs) {
    sta_ = Sta::Connected;
    backoff_ = 0;
    connectedAtMs_ = nowMs;
    failure_.clear();
    failureShown_.clear();
    StaInfo info;
    linkUp_ = true;
    if (wifi_->staInfo(info)) {
        linkSsid_ = info.ssid;
        linkIp_ = formatIpv4(info.ip);
        staAddr_ = info.ip;
        linkChannel_ = info.channel;
        linkBssid_ = formatMac(info.bssid);
    } else {
        linkSsid_ = current_.ssid;
    }
    staStatus_->changed();
    tesser::logWarning("tesserkit: joined \"%s\" as %s", linkSsid_.c_str(), linkIp_.c_str());
    joined_->emit(linkIp_.empty() ? linkSsid_ : linkIp_ + " on " + linkSsid_);
    if (linkChannel_) dev_.channel(linkChannel_);
}

void Network::lost(uint32_t nowMs) {
    linkUp_ = false;
    linkSsid_.clear();
    linkIp_.clear();
    linkBssid_.clear();
    linkChannel_ = 0;
    staAddr_ = 0;
    staStatus_->changed();
    lostAtMs_ = nowMs;
}

void Network::ethLink(bool up, uint32_t nowMs) {
    if (up == ethLink_) return;
    ethLink_ = up;
    if (up) {
        EthInfo i;
        if (eth_->info(i)) ethMbps_ = i.mbps;
        ethStatus_->changed();
        ethWaitUntilMs_ = nowMs + kEthWaitMs;  // the station waits for DHCP
        return;
    }
    if (ethOnline_) tesser::logWarning("tesserkit: Ethernet down");
    ethDown(nowMs);
}

// Ethernet has an address (or a new one): from an event, a settings change,
// or found by polling (link-local).
void Network::ethGotIp(uint32_t nowMs) {
    EthInfo i;
    if (!eth_->info(i) || !i.ip) return;
    // Only a lease shows a network beyond the cable, which the station
    // can leave to Ethernet: not a static address, nor a link-local one.
    bool lease = !ethStaticOn_ && !linkLocal(i.ip);
    bool was = ethOnline_;
    if (was && i.ip == ethAddr_ && lease == ethLease_) return;
    ethOnline_ = ethLink_ = true;
    ethLease_ = lease;
    ethAddr_ = i.ip;
    ethIp_ = formatIpv4(i.ip);
    ethMbps_ = i.mbps;
    ethStatus_->changed();
    tesser::logWarning("tesserkit: Ethernet up as %s", ethIp_.c_str());
    ethJoined_->emit(ethIp_ + " on Ethernet" + (linkLocal(i.ip) ? " (link-local)" : ""));
    if (!was) connectedAtMs_ = nowMs;  // clients on the access point follow the new address
    applyStation(nowMs);
}

// Ethernet lost its address; the station waits `waitMs` more for a new one.
void Network::ethDown(uint32_t nowMs, uint32_t waitMs) {
    bool was = ethOnline_;
    // The station waits for this cable: it needn't any more.
    bool waited = sta_ == Sta::Waiting && static_cast<int32_t>(ethWaitUntilMs_ - nowMs) > 0;
    ethWaitUntilMs_ = nowMs + waitMs;
    ethOnline_ = false;
    ethLease_ = false;
    ethIp_.clear();
    ethAddr_ = 0;
    ethMbps_ = 0;
    ethStatus_->changed();
    if (was) lostAtMs_ = nowMs;
    if (was ? sta_ == Sta::Idle || sta_ == Sta::Waiting : waited) {
        backoff_ = 0;
        join(nowMs);
    }
}

void Network::event(const Event& e) {
    if (!wifi_) return;
    uint32_t now = static_cast<uint32_t>(dev_.core().uptimeMs());
    switch (e.type) {
        case Event::Type::EthLink:
            if (eth_) ethLink(e.value != 0, now);
            break;
        case Event::Type::EthGotIp:
            if (eth_) ethGotIp(now);
            break;
        case Event::Type::ScanDone:
            if (scanning_) scanDone(wifi_->scanResults(), now);
            break;
        case Event::Type::StaGotIp:
            if (sta_ == Sta::Connecting || sta_ == Sta::Connected) connected(now);
            break;
        case Event::Type::StaDisconnected:
            if (e.value == Event::kReasonLeft) break;
            if (sta_ == Sta::Connecting) {
                failed(joinFailure(e.value));
                connectNext(now);
            } else if (sta_ == Sta::Connected) {
                tesser::logWarning("tesserkit: lost \"%s\" (reason %d)", linkSsid_.c_str(), static_cast<int>(e.value));
                lost(now);
                join(now);
            }
            break;
        case Event::Type::ApClients:
            apClients_ = e.value;
            apStatus_->changed();
            break;
        default:
            break;
    }
}

void Network::checkSettings(uint32_t nowMs) {
    if (networksNode_->generation() != networksGen_) {
        networksGen_ = networksNode_->generation();
        backoff_ = 0;
        failureShown_.clear();  // a failure with the new settings is news
        if (sta_ == Sta::Connected) {
            // Reconnect only if the joined network changed or went away.
            const WifiNetwork* n = find(current_.ssid);
            StaConfig c = n ? config(*n) : StaConfig();
            if (!n || c.password != current_.password || c.staticIp != current_.staticIp || c.ip != current_.ip ||
                c.mask != current_.mask || c.gateway != current_.gateway || c.dns != current_.dns) {
                wifi_->disconnect();
                lost(nowMs);
                join(nowMs);
            }
        } else {
            if (sta_ == Sta::Connecting) wifi_->disconnect();
            join(nowMs);
        }
    }
    if (hostnameNode_->generation() != hostnameGen_) {
        hostnameGen_ = hostnameNode_->generation();
        wifi_->hostname(dev_.hostname().c_str());
        if (eth_) eth_->hostname(dev_.hostname().c_str());
        applyMdns();
    }
    if (mdnsNode_ && mdnsNode_->generation() != mdnsGen_) {
        mdnsGen_ = mdnsNode_->generation();
        applyMdns();
    }
    if (staModeNode_->generation() != staModeGen_) {
        staModeGen_ = staModeNode_->generation();
        applyStation(nowMs);
    }
    if (eth_ && ethEnabledNode_->generation() != ethEnabledGen_) {
        ethEnabledGen_ = ethEnabledNode_->generation();
        eth_->enabled(ethEnabled_);
        if (!ethEnabled_) ethLink(false, nowMs);  // the link goes with the MAC
    }
    if (eth_ && ethFingerprint() != ethGen_) {
        ethGen_ = ethFingerprint();
        ethReaddress(nowMs);
    }
    uint32_t fp = apFingerprint();
    if (fp != apGen_) {
        apGen_ = fp;
        if (apUp_) {  // restarted with the new settings by updateAp()
            wifi_->stopAp();
            apUp_ = false;
        }
    }
}

void Network::ethReaddress(uint32_t nowMs) {
    IpConfig c = ethConfig();
    ethStaticOn_ = c.staticIp;
    eth_->address(c);
    // The old address goes with the old settings: a static one is there at
    // once, a lease when DHCP answers (EthGotIp), which the station waits for
    // as after a link.
    EthInfo i;
    if (ethLink_ && eth_->info(i) && i.ip) {
        ethGotIp(nowMs);
    } else {
        ethDown(nowMs, ethLink_ ? kEthWaitMs : 0);
    }
}

void Network::applyMdns() {
    if (mdnsService_) mdnsService_->set(mdns_, dev_.hostname().c_str(), dev_.version().c_str(), dev_.httpPort());
}

void Network::updateAp(uint32_t nowMs) {
    // Recovery: a device without a network is always reachable; one with
    // Ethernet gives its cable the fallback's time first.
    bool recovery = networks_.empty();
    bool want;
    if (apMode_ == ApMode::Always || (recovery && !eth_)) {
        want = true;
    } else if (apMode_ == ApMode::Never && !recovery) {
        want = false;
    } else if (online()) {
        want = apUp_ && nowMs - connectedAtMs_ < kApLingerMs;
    } else {
        want = apUp_ || nowMs - lostAtMs_ >= kApFallbackMs;
    }
    if (want && !apUp_) {
        ApConfig c;
        c.ssid = apSsid();
        c.password = apPassword_;
        c.channel = apChannel_;
        c.hidden = apHidden_;
        if (wifi_->startAp(c)) {
            apUp_ = true;
            apAddr_ = wifi_->apIp();
            apIp_ = formatIpv4(apAddr_);
            apClients_ = 0;
            apStatus_->changed();
            tesser::logWarning("tesserkit: access point \"%s\" at %s", c.ssid.c_str(), apIp_.c_str());
        }
    } else if (!want && apUp_) {
        wifi_->stopAp();
        apUp_ = false;
        apAddr_ = 0;
        apIp_.clear();
        apClients_ = 0;
        apStatus_->changed();
    }
    bool captive = apUp_ && !online();
    if (captive != captive_) {
        captive_ = captive;
        wifi_->captivePortal(captive);
    }
}

void Network::tick(uint32_t nowMs) {
    if (!wifi_) return;
    checkSettings(nowMs);
    if (scanning_ && nowMs - scanStartMs_ > kScanTimeoutMs) {
        tesser::logWarning("tesserkit: Wi-Fi scan timed out");
        scanDone({}, nowMs);
    }
    if (sta_ == Sta::Connecting && nowMs - sinceMs_ > kConnectTimeoutMs) {
        wifi_->disconnect();
        failed("no answer");
        connectNext(nowMs);
    } else if (sta_ == Sta::Waiting && static_cast<int32_t>(nowMs - retryAtMs_) >= 0) {
        join(nowMs);
    }
    // A requested scan waits for a connection attempt to end: the radio
    // can't scan while it joins.
    if (scanWanted_ && sta_ != Sta::Connecting) {
        scanWanted_ = false;
        if (!startScan(false, nowMs)) {
            for (tesser::Pending& p : scanWaiters_) p.fail(Status::Busy, "the radio can't scan now");
            scanWaiters_.clear();
        }
    }
    // An address without an event: link-local, while DHCP keeps asking.
    if (eth_ && ethLink_ && !ethOnline_ && nowMs - ethPollAtMs_ >= kEthPollMs) {
        ethPollAtMs_ = nowMs;
        ethGotIp(nowMs);
    }
    updateAp(nowMs);
    reportState();
    updateAddress();
    updateStatus(nowMs);
}

// Each interface's state, in words: what it does, or why it doesn't.
void Network::updateStatus(uint32_t nowMs) {
    std::string s;
    if (staMode_ == StationMode::Never) {
        s = "off";
    } else if (networks_.empty()) {
        s = "no network saved";
    } else {
        switch (sta_) {
            case Sta::Connected: s = "connected"; break;
            case Sta::Connecting: s = "joining \"" + current_.ssid + "\""; break;
            case Sta::Scanning: s = "looking for networks"; break;
            case Sta::Waiting:
                s = ethWaits(nowMs)        ? "waiting for Ethernet"
                    : failure_.empty() ? "waiting to retry"
                                       : failure_ + "; retrying";
                break;
            case Sta::Idle: s = stationKeptAway() ? "not needed: Ethernet has a DHCP address" : "idle"; break;
        }
    }
    if (s != staState_) {
        staState_ = s;
        staStatus_->changed();
    }
    if (!eth_) return;
    s = !ethEnabled_   ? "off"
        : !ethLink_    ? "no cable"
        : !ethOnline_  ? (ethStaticOn_ ? "applying the static address" : "waiting for DHCP")
        : ethStaticOn_ ? "connected, static address"
        : !ethLease_   ? "connected, link-local address (no DHCP server)"
                       : "connected";
    if (s != ethState_) {
        ethState_ = s;
        ethStatus_->changed();
    }
}

void Network::reportState() {
    dev_.networkState(online()                          ? State::Online
                      : networks_.empty() && !ethLink_ ? State::Provisioning
                                                       : State::Connecting);
}

void Network::updateAddress() {
    if (ethOnline_) {
        dev_.address(ethAddr_, ethMacBytes_);
    } else {
        dev_.address(connected() ? staAddr_ : apAddr_, dev_.info().mac);
    }
}

}  // namespace tesserkit
