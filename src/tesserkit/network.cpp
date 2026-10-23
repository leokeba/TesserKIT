#include "tesserkit/network.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <iterator>
#include <utility>

#include "tesserkit/limits.h"

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

// A host's own address: not 0.0.0.0, loopback, multicast or broadcast.
bool hostAddress(uint32_t ip) { return ip != 0 && ip >> 24 != 127 && ip < 0xE0000000u; }

Check checkAddress(const std::string& text) {
    uint32_t ip;
    return text.empty() || (parseIpv4(text, ip) && hostAddress(ip))
               ? Check::ok()
               : Check::fail(Status::InvalidValue, "expected a host's address, a.b.c.d");
}

// A netmask: ones then zeros, /1 to /32. Never empty: a static address
// can't apply without one.
bool contiguousMask(uint32_t m) { return m != 0 && ((~m + 1) & ~m) == 0; }

Check checkMask(const std::string& text) {
    uint32_t m;
    return parseIpv4(text, m) && contiguousMask(m) ? Check::ok()
                                                   : Check::fail(Status::InvalidValue, "a netmask: 255.255.255.0");
}

// An IPv4 field of a static configuration.
tesser::ValueNode& ipField(tesser::Object& o, const char* name, const char* label, std::string& field,
                           Check (*check)(const std::string&) = checkIp) {
    return o.value(name, field).validate(check).label(label).ui("visibleWhen", "enabled=true");
}

// A static address's settings, collapsed: DHCP unless enabled; this node's
// own, which TesserUI's apply to all leaves out. Its values' nodes go to
// `nodes` when given.
tesser::Object& describeStatic(tesser::Object& parent, StaticIp& ip, std::vector<tesser::Node*>* nodes = nullptr) {
    auto& s = parent.object("static").label("Static IP").ui("collapsed").ui("perNode");
    tesser::Node* n[] = {
        &s.value("enabled", ip.enabled).label("Use a static address"),
        &ipField(s, "ip", "Address", ip.ip, checkAddress),
        &ipField(s, "mask", "Netmask", ip.mask, checkMask),
        &ipField(s, "gateway", "Gateway", ip.gateway).ui("placeholder", "none: no router"),
        &ipField(s, "dns", "DNS server", ip.dns),
    };
    if (nodes) nodes->assign(std::begin(n), std::end(n));
    return s;
}

// The setup a static address's settings make: DHCP unless they are enabled
// and have an address. The fields' validators refuse a malformed address or
// mask; an empty gateway is a network without a router (gateway 0).
IpConfig ipConfig(const StaticIp& s, const char* what) {
    IpConfig c;
    if (!s.enabled) return c;
    c.staticIp = parseIpv4(s.ip, c.ip) && hostAddress(c.ip) && parseIpv4(s.mask, c.mask) && contiguousMask(c.mask);
    if (!c.staticIp) {
        // Validators see one field at a time, so "enabled" with no address
        // yet can't be refused (DESIGN.md section 5.2).
        tesser::logWarning("tesserkit: no static address for %s, using DHCP", what);
        return IpConfig();
    }
    if (!parseIpv4(s.gateway, c.gateway)) c.gateway = 0;
    if (!parseIpv4(s.dns, c.dns)) c.dns = c.gateway;  // a router usually answers names; 0: none
    return c;
}

std::string formatMac(const uint8_t* m) {
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

// "3a:07:16:43:fd:24", any case.
bool parseMac(const char* s, uint8_t* out) {
    for (int i = 0; i < 6; i++) {
        int v = 0;
        for (int j = 0; j < 2; j++) {
            char c = *s++;
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + d;
        }
        out[i] = static_cast<uint8_t>(v);
        if (i < 5 && *s++ != ':') return false;
    }
    return *s == 0;
}

bool linkLocal(uint32_t ip) { return ip >> 16 == 0xA9FE; }  // 169.254/16

// Why a join failed: the platform's reason, and its own code for the rest.
std::string joinFailure(StaReason reason, const std::string& code) {
    switch (reason) {
        case StaReason::NotFound:
            return "not found";
        case StaReason::WrongPassword:
            return "wrong password?";
        case StaReason::Refused:
            return "refused the connection";
        default:
            return code.empty() ? std::string("failed") : "failed (" + code + ")";
    }
}

// A DHCP server that didn't answer a renewal is asked again after this.
constexpr uint32_t kRenewMs = 5000;

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
    // A form: a network is saved whole, its password with it, not field by
    // field as it is typed.
    o.ui("form");
    o.value("ssid", n.ssid).label("Network");
    o.value("password", n.password).secret();
    o.value("bssid", n.bssid)
        .label("Access point")
        .doc("Joins only this one (its BSSID, from a scan) when several share the network's name; any of them "
             "when it isn't heard")
        .validate([](const std::string& v) {
            uint8_t b[6];
            return v.empty() || parseMac(v.c_str(), b)
                       ? Check::ok()
                       : Check::fail(Status::InvalidValue, "a BSSID: 3a:07:16:43:fd:24, or empty for any");
        })
        .ui("placeholder", "any")
        .ui("advanced");
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

bool byName(const std::string& name, EthernetBoard& out) {
    static const struct {
        const char* name;
        EthernetBoard (*board)();
    } kBoards[] = {
        {"gledopto", gledopto},
        {"wt32-eth01", wt32Eth01},
        {"olimex-esp32-poe", olimexEsp32Poe},
        {"esp32-ethernet-kit", esp32EthernetKit},
    };
    for (const auto& b : kBoards) {
        if (name == b.name) {
            out = b.board();
            return true;
        }
    }
    return false;
}

EthernetBoard w5500(int8_t sck, int8_t miso, int8_t mosi, int8_t cs, int8_t irq, int8_t reset) {
    EthernetBoard b;
    b.phy = Phy::W5500;
    b.sck = sck;
    b.miso = miso;
    b.mosi = mosi;
    b.cs = cs;
    b.irq = irq;
    b.reset = reset;
    return b;
}

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

Network& Network::powerSave(PowerSave mode) {
    powerSave_ = mode;
    powerSaveChosen_ = true;
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

Network& Network::ethernet(std::function<bool(EthernetBoard&)> choose) {
    ethernet(EthernetBoard());
    ethChoose_ = std::move(choose);
    return *this;
}

void Network::migrate(uint32_t from, JsonObject settings) {
    // Before version 2 the hostname's setting was the hostname itself, its
    // default the label's (or the application's): one stored equal to it
    // would now hold the hostname when the user names the device. Before the
    // stored settings load, hostname() is still that default.
    if (from < 2 && settings["hostname"].is<const char*>() && dev_.hostname() == settings["hostname"].as<const char*>()) {
        settings.remove("hostname");
    }
}

void Network::describe(tesser::Object& o) {
    Device& dev = dev_;
    // Beside NowTp the factory default keeps the modem awake: ESP-NOW loses
    // the frames that arrive while it sleeps (NowTP 014c4d5).
    if (!powerSaveChosen_ && dev.stationAwake()) powerSave_ = PowerSave::None;
    o.label("Network").ui("icon", "wifi").writeAccess(Access::Admin);
    // Empty: from the device's name (docs/DESIGN.md section 4.4).
    o.value("hostname", [&dev] { return dev.hostnameSetting(); },
            [&dev](const std::string& h) { dev.hostnameSetting(h); })
        .validate([](const std::string& h) {
            return h.empty() || validHostname(h)
                       ? Check::ok()
                       : Check::fail(Status::InvalidValue, "1 to 32 letters, digits and inner hyphens");
        })
        .persist()
        .ui("placeholder", "from the name")
        .ui("perNode")
        .ui("advanced");
    // What the network knows the device as (<host>.local): the setting, or the
    // one made from the name.
    o.value("host", [&dev] { return dev.hostname(); })
        .readOnly()
        .label("Hostname in use")
        .watch()
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

    powerSaveNode_ = &w.value("powerSave", powerSave_, {"none", "min", "max"})
                          .persist()
                          .label("Power save")
                          .ui("labels", "Off: lowest latency,Modem sleep,Maximum")
                          .ui("advanced");

    networksNode_ = &w.list("networks", networks_)
                         .key("ssid")
                         .maxSize(5)
                         .readAccess(Access::User)
                         .writeAccess(Access::Admin)
                         .persist()
                         .label("Saved networks")
                         .ui("suggest", "scan");
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
    apNodes_.push_back(&ap.value("ssid", apSsid_)
                            .validate(limits::atMost<32>)
                            .persist()
                            .label("Name")
                            .ui("placeholder", "the hostname")
                            .ui("perNode"));
    apNodes_.push_back(&ap.value("password", apPassword_)
                           .validate([](const std::string& v) {
                               return v.empty() || (v.size() >= 8 && v.size() <= 63)
                                          ? Check::ok()
                                          : Check::fail(Status::InvalidValue, "empty, or 8 to 63 characters");
                           })
                           .secret()
                           .persist()
                           .ui("placeholder", "none: open, or securePassword in secure mode"));
    apSecretNode_ = &ap.value("securePassword", apSecret_)
                         .validate(limits::atMost<63>)
                         .readOnly()
                         .persist()
                         .backup(false)  // this device's
                         .readAccess(Access::Admin)
                         .label("Password in secure mode")
                         .doc("The access point's password while the device has an admin account and no password "
                              "is set above: made once for this device, printed on the serial console when the "
                              "access point comes up")
                         .ui("perNode")
                         .ui("advanced");
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
    if (!p.ipStack) p.ipStack = espIpStack();
    ipStack_ = p.ipStack;
    if (ipStack_) ipStack_->begin([&dev](Event e) { dev.post(std::move(e)); });
    if (txPower_) wifi_->txPower(txPower_);
    wifi_->powerSave(powerSave_);
    powerSaveGen_ = powerSaveNode_->generation();
    uint32_t now = static_cast<uint32_t>(dev.core().uptimeMs());
    if (eth_ && ethChoose_ && !ethChoose_(ethBoard_)) {
        eth_ = nullptr;
        ethState_ = "no board";
        ethStatus_->changed();
    }
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
    hostnameGen_ = dev.hostnameGeneration();
    if (mdnsNode_) mdnsGen_ = mdnsNode_->generation();
    apGen_ = apFingerprint();
    ethGen_ = ethFingerprint();
    staModeGen_ = staModeNode_->generation();
    if (ethEnabledNode_) ethEnabledGen_ = ethEnabledNode_->generation();
    settingsSeen_ = settingsFingerprint();
    applyMdns();
    lostAtMs_ = now;
    join(now);
    updateAp(now);
    reportState();  // from the start: services waiting for the network see it
}

std::string Network::apPasswordInUse() {
    if (!apPassword_.empty() || !dev_.secure()) return apPassword_;
    if (apSecret_.empty()) {
        // 12 characters of a 32-letter alphabet without look-alikes (60
        // bits), typed from a phone.
        static const char kAlphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
        uint8_t r[12];
        dev_.core().random(r, sizeof(r));
        for (uint8_t b : r) apSecret_ += kAlphabet[b % (sizeof(kAlphabet) - 1)];
        apSecretNode_->changed();
    }
    return apSecret_;
}

uint32_t Network::apFingerprint() const {
    // Generations only grow, so the sum changes whenever one of them does.
    uint32_t sum = 0;
    for (const tesser::Node* n : apNodes_) sum += n->generation();
    return sum + dev_.secureGeneration();  // secure mode changes the password
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
    // Its access point only, when the last scan heard it (or heard nothing:
    // a hidden network); otherwise any, rather than not joining at all.
    uint8_t b[6];
    if (n.bssid[0] && parseMac(n.bssid, b)) {
        bool heard = heard_.empty();
        for (const auto& h : heard_) heard = heard || memcmp(h.data(), b, sizeof(b)) == 0;
        if (heard) {
            c.bssidSet = true;
            memcpy(c.bssid, b, sizeof(b));
        }
    }
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
    heard_.clear();
    for (const ScanResult& r : results) {
        std::array<uint8_t, 6> b;
        memcpy(b.data(), r.bssid, b.size());
        heard_.push_back(b);
    }
    // One entry per network, strongest first, with its access points.
    std::vector<std::vector<const ScanResult*>> groups;
    for (const ScanResult& r : results) {
        bool found = false;
        for (auto& g : groups) {
            if (g[0]->ssid != r.ssid) continue;
            size_t at = 0;
            while (at < g.size() && g[at]->rssi >= r.rssi) at++;
            g.insert(g.begin() + static_cast<std::ptrdiff_t>(at), &r);
            found = true;
            break;
        }
        if (!found) groups.push_back({&r});
    }
    std::stable_sort(groups.begin(), groups.end(),
                     [](const std::vector<const ScanResult*>& a, const std::vector<const ScanResult*>& b) {
                         return a[0]->rssi > b[0]->rssi;
                     });
    for (tesser::Pending& p : scanWaiters_) {
        p.replyWith([&groups](tesser::JsonWriter& w) {
            w.beginArray();
            for (const auto& g : groups) {
                const ScanResult& r = *g[0];
                w.beginObject();
                w.key("ssid");
                w.string(r.ssid);
                w.key("rssi");
                w.integer(r.rssi);
                w.key("channel");
                w.uinteger(r.channel);
                w.key("secure");
                w.boolean(r.secure);
                w.key("accessPoints");
                w.beginArray();
                for (const ScanResult* a : g) {
                    w.beginObject();
                    w.key("bssid");
                    w.string(formatMac(a->bssid));
                    w.key("rssi");
                    w.integer(a->rssi);
                    w.key("channel");
                    w.uinteger(a->channel);
                    w.endObject();
                }
                w.endArray();
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
        // Its access point's signal when it has one (and it was heard).
        StaConfig c = config(n);
        int best = -1000;
        for (const ScanResult& r : results) {
            if (r.ssid != n.ssid || r.rssi <= best) continue;
            if (c.bssidSet && memcmp(r.bssid, c.bssid, sizeof(c.bssid)) != 0) continue;
            best = r.rssi;
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
            associated_ = false;
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
    ethGateway_ = i.gateway;
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
    ethGateway_ = 0;
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
    uint32_t now = dev_.loopMs();  // the tick()'s time, which these are compared with
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
        case Event::Type::StaAssociated:
            if (sta_ == Sta::Connecting && !associated_) {
                associated_ = true;
                sinceMs_ = now;  // the address's own time starts
            }
            break;
        case Event::Type::StaGotIp:
            if (sta_ == Sta::Connecting || sta_ == Sta::Connected) connected(now);
            break;
        case Event::Type::StaDisconnected:
            if (static_cast<StaReason>(e.value) == StaReason::Left) break;
            if (sta_ == Sta::Connecting) {
                failed(joinFailure(static_cast<StaReason>(e.value), e.text));
                connectNext(now);
            } else if (sta_ == Sta::Connected) {
                tesser::logWarning("tesserkit: lost \"%s\" (%s)", linkSsid_.c_str(), e.text.c_str());
                lost(now);
                join(now);
            }
            break;
        case Event::Type::ApClients:
            if (apUp_) setApClients(e.value);
            break;
        default:
            break;
    }
    // An interface came or went: the default route follows. And what
    // empties the DNS servers' list: an association or a cable's link starts
    // a DHCP client, an address comes or goes (DnsEmpty).
    switch (e.type) {
        case Event::Type::StaAssociated:
        case Event::Type::StaGotIp:
        case Event::Type::StaDisconnected:
        case Event::Type::EthLink:
        case Event::Type::EthGotIp:
        case Event::Type::DnsEmpty:
            updateRoute();
            refillDns(now);
            break;
        default:
            break;
    }
}

// The interface traffic beyond the interfaces' own subnets goes through:
// Ethernet while it has a gateway (its lease's router, or a static one),
// else the station while it is joined, else Ethernet with any address (its
// own subnet only). A cable without a router (a computer, a show network
// without DHCP, a static address without a gateway) then leaves the internet
// to the station: as the default, it would take every connection beyond its
// subnet and drop them (seen on board B, 2026-10-21: a link-local cable and
// a joined station, every HTTPS request "server unreachable").
bool Network::routeVia(NetIf& out) const {
    bool eth = eth_ && ethOnline_;
    if (eth && ethGateway_) {
        out = NetIf::Ethernet;
    } else if (sta_ == Sta::Connected && staAddr_) {
        out = NetIf::Station;
    } else if (eth) {
        out = NetIf::Ethernet;
    } else {
        return false;  // neither has an address: the next one decides
    }
    return true;
}

// The IP stack keeps the interface set until it is set again, so a change is
// set at once: from the events above, or the next poll if one was lost.
void Network::updateRoute() {
    NetIf n;
    if (!ipStack_ || !routeVia(n) || (routeSet_ && n == route_)) return;
    ipStack_->setDefault(n);
    routeSet_ = true;
    route_ = n;
    dev_.routeChanged();
}

// The IP stack has one list of DNS servers for every interface. A lease's
// servers come in its acknowledgement, seconds before its address (the
// address check, ACD), and the stack empties the list whenever an interface
// starts its DHCP client (a station associating, a cable's link) or takes a
// static address: a station trying to join in those seconds left a device
// on Ethernet without names until the lease's renewal, hours later. So an
// empty list is filled again from the default route's interface first, then
// the other: its static server, or its lease renewed, whose acknowledgement
// brings the lease's servers back (DESIGN.md section 5.2). A static address without a server
// of its own, or a link-local one, gives none: the next interface does.
void Network::refillDns(uint32_t nowMs) {
    if (!ipStack_ || ipStack_->dnsServer()) return;
    struct Option {
        NetIf netif;
        bool up;
        bool staticIp;
        uint32_t dns;
        bool lease;
    };
    IpConfig eth = ethConfig();
    Option options[] = {
        {NetIf::Ethernet, eth_ && ethOnline_, ethStaticOn_, eth.dns, ethLease_},
        {NetIf::Station, sta_ == Sta::Connected && staAddr_ != 0, current_.staticIp, current_.dns, !current_.staticIp},
    };
    NetIf first;
    if (routeVia(first) && first == NetIf::Station) std::swap(options[0], options[1]);
    for (const Option& o : options) {
        if (!o.up) continue;
        if (o.staticIp) {
            if (!o.dns) continue;
            ipStack_->setDnsServer(o.dns);
            return;
        }
        if (!o.lease) continue;
        size_t i = static_cast<size_t>(o.netif);
        if (renewed_[i] && nowMs - renewedAtMs_[i] < kRenewMs) return;  // its answer is on its way
        if (ipStack_->renewLease(o.netif)) {
            renewed_[i] = true;
            renewedAtMs_[i] = nowMs;
            return;
        }
    }
}

// The interfaces' state as the platform keeps it. The events that report
// it pass through the device's bounded queue, which a burst (MQTT's retained
// commands, peers flapping) during a long stall of the device's task (an OTA
// chunk, a sign-in) can fill: a lost edge is caught up here, so a dead cable
// or a lost access point can't keep the station away or the device online.
void Network::poll(uint32_t nowMs) {
    if (eth_) {
        EthInfo i;
        if (eth_->info(i)) {
            if (!i.link) {
                if (ethLink_) ethLink(false, nowMs);
            } else {
                if (!ethLink_) ethLink(true, nowMs);
                if (i.ip && (!ethOnline_ || i.ip != ethAddr_)) {
                    ethGotIp(nowMs);  // a new address: link-local (AutoIP posts no event), or a lost EthGotIp
                } else if (i.ip && i.gateway != ethGateway_) {
                    ethGateway_ = i.gateway;  // the same address, another gateway
                } else if (!i.ip && ethOnline_) {
                    tesser::logWarning("tesserkit: Ethernet lost its address");
                    ethDown(nowMs, kEthWaitMs);  // DHCP may give it back
                }
            }
        }
    }
    StaInfo info;
    if (sta_ == Sta::Connected) {
        if (!wifi_->staInfo(info)) {
            tesser::logWarning("tesserkit: lost \"%s\"", linkSsid_.c_str());
            lost(nowMs);
            join(nowMs);
        } else if (info.ip && info.ip != staAddr_) {
            staAddr_ = info.ip;
            linkIp_ = formatIpv4(info.ip);
            staStatus_->changed();
        }
    } else if (sta_ == Sta::Connecting) {
        if (associated_ && wifi_->staInfo(info) && info.ip) connected(nowMs);  // its address came, though the event didn't
    } else if (wifi_->staInfo(info)) {
        // Joined, though the station counts on nothing: a late event said
        // the attempt failed (a disconnection from before it). Idle wants
        // no station (kept away by Ethernet, off, nothing saved): it
        // leaves. Otherwise it is the network it was after.
        if (sta_ == Sta::Idle) {
            wifi_->disconnect();
        } else if (info.ip) {
            connected(nowMs);
        }
    }
    if (apUp_) setApClients(wifi_->apClients());
    updateRoute();  // an event that would have, lost
    refillDns(nowMs);
}

void Network::setApClients(int32_t n) {
    if (n == apClients_) return;
    apClients_ = n;
    dev_.apClients(n);
    apStatus_->changed();
}

// Every setting checkSettings() applies: generations only grow, so the sum
// changes whenever one of them does.
uint32_t Network::settingsFingerprint() const {
    uint32_t sum = networksNode_->generation() + dev_.hostnameGeneration() + powerSaveNode_->generation() +
                   staModeNode_->generation() + apFingerprint();
    if (mdnsNode_) sum += mdnsNode_->generation();
    if (eth_) sum += ethEnabledNode_->generation() + ethFingerprint();
    return sum;
}

void Network::checkSettings(uint32_t nowMs) {
    uint32_t seen = settingsFingerprint();
    if (seen != settingsSeen_) {
        settingsSeen_ = seen;
        settingsAtMs_ = nowMs;
    }
    if (nowMs - settingsAtMs_ < kApplyAfterMs) return;
    if (networksNode_->generation() != networksGen_) {
        networksGen_ = networksNode_->generation();
        backoff_ = 0;
        failureShown_.clear();  // a failure with the new settings is news
        if (sta_ == Sta::Connected) {
            // Reconnect only if the joined network changed or went away.
            const WifiNetwork* n = find(current_.ssid);
            StaConfig c = n ? config(*n) : StaConfig();
            // An access point set: only if it isn't the one joined.
            StaInfo joined;
            bool otherAp = c.bssidSet && (!wifi_->staInfo(joined) || memcmp(joined.bssid, c.bssid, sizeof(c.bssid)) != 0);
            if (!n || c.password != current_.password || c.staticIp != current_.staticIp || c.ip != current_.ip ||
                c.mask != current_.mask || c.gateway != current_.gateway || c.dns != current_.dns || otherAp) {
                wifi_->disconnect();
                lost(nowMs);
                join(nowMs);
            } else {
                current_ = c;  // the access point it is on, from now on
            }
        } else {
            if (sta_ == Sta::Connecting) wifi_->disconnect();
            join(nowMs);
        }
    }
    if (dev_.hostnameGeneration() != hostnameGen_) {  // its setting, or the name
        hostnameGen_ = dev_.hostnameGeneration();
        wifi_->hostname(dev_.hostname().c_str());
        if (eth_) eth_->hostname(dev_.hostname().c_str());
        applyMdns();
    }
    if (mdnsNode_ && mdnsNode_->generation() != mdnsGen_) {
        mdnsGen_ = mdnsNode_->generation();
        applyMdns();
    }
    if (powerSaveNode_->generation() != powerSaveGen_) {
        powerSaveGen_ = powerSaveNode_->generation();
        wifi_->powerSave(powerSave_);
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
        c.password = apPasswordInUse();
        c.channel = apChannel_;
        c.hidden = apHidden_;
        if (wifi_->startAp(c)) {
            apUp_ = true;
            apAddr_ = wifi_->apIp();
            apIp_ = formatIpv4(apAddr_);
            apClients_ = 0;
            dev_.apClients(0);
            apStatus_->changed();
            if (apPassword_.empty() && !c.password.empty()) {
                tesser::logWarning("tesserkit: access point \"%s\" at %s, password %s (secure mode)", c.ssid.c_str(),
                                   apIp_.c_str(), c.password.c_str());
            } else {
                tesser::logWarning("tesserkit: access point \"%s\" at %s", c.ssid.c_str(), apIp_.c_str());
            }
        }
    } else if (!want && apUp_) {
        wifi_->stopAp();
        apUp_ = false;
        apAddr_ = 0;
        apIp_.clear();
        apClients_ = 0;
        dev_.apClients(0);
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
    if (sta_ == Sta::Connecting && nowMs - sinceMs_ > (associated_ ? kAddressTimeoutMs : kConnectTimeoutMs)) {
        wifi_->disconnect();
        failed(associated_ ? "no address (DHCP)" : "no answer");
        connectNext(nowMs);
    } else if (sta_ == Sta::Waiting && static_cast<int32_t>(nowMs - retryAtMs_) >= 0) {
        join(nowMs);
    }
    // A requested scan waits for an attempt to associate or fail: the radio
    // can't scan while it joins.
    if (scanWanted_ && !(sta_ == Sta::Connecting && !associated_)) {
        scanWanted_ = false;
        if (!startScan(false, nowMs)) {
            for (tesser::Pending& p : scanWaiters_) p.fail(Status::Busy, "the radio can't scan now");
            scanWaiters_.clear();
        }
    }
    if (nowMs - polledAtMs_ >= kPollMs) {
        polledAtMs_ = nowMs;
        poll(nowMs);
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
            case Sta::Connecting:
                s = (associated_ ? "getting an address from \"" : "joining \"") + current_.ssid + "\"";
                break;
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
