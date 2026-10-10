#include "tesserkit/network.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
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
void ipField(tesser::Object& o, const char* name, const char* label, std::string& field) {
    o.value(name, [&field] { return field; },
            [&field](const std::string& v) -> Check {
                Check c = checkIp(v);
                if (c.isOk()) field = v;
                return c;
            })
        .label(label)
        .ui("visibleWhen", "enabled=true");
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
    auto& s = o.object("static").label("Static IP").ui("collapsed");
    s.value("enabled", n.ip.enabled).label("Use a static address");
    ipField(s, "ip", "Address", n.ip.ip);
    ipField(s, "mask", "Netmask", n.ip.mask);
    ipField(s, "gateway", "Gateway", n.ip.gateway);
    ipField(s, "dns", "DNS server", n.ip.dns);
}

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

void Network::describe(tesser::Object& o) {
    Device& dev = dev_;
    o.label("Network").writeAccess(Access::Admin);
    hostnameNode_ = &o.value("hostname", [&dev] { return dev.hostname(); },
                             [&dev](const std::string& h) -> Check {
                                 if (!validHostname(h)) {
                                     return Check::fail(Status::InvalidValue,
                                                        "1 to 32 letters, digits and inner hyphens");
                                 }
                                 dev.hostname(h.c_str());
                                 return Check::ok();
                             })
                         .persist()
                         .ui("advanced");

    auto& w = o.object("wifi").label("Wi-Fi");
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
    auto& st = w.object("status").label("Connection");
    staStatus_ = &st;
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

    auto& ap = o.object("ap").label("Access point").ui("form");
    ap.value("mode", apMode_, {"always", "fallback", "never"})
        .persist()
        .ui("labels", "Always,When offline,Never");
    apNodes_.clear();
    apNodes_.push_back(&ap.value("ssid", apSsid_).persist().label("Name").ui("placeholder", "the hostname"));
    apNodes_.push_back(&ap.value("password", [this] { return apPassword_; },
                                 [this](const std::string& v) -> Check {
                                     if (!v.empty() && (v.size() < 8 || v.size() > 63)) {
                                         return Check::fail(Status::InvalidValue, "empty, or 8 to 63 characters");
                                     }
                                     apPassword_ = v;
                                     return Check::ok();
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

    mdnsNode_ = &o.value("mdns", mdns_).persist().label("mDNS").ui("advanced");
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
    networksGen_ = networksNode_->generation();
    hostnameGen_ = hostnameNode_->generation();
    mdnsGen_ = mdnsNode_->generation();
    apGen_ = apFingerprint();
    applyMdns();
    uint32_t now = static_cast<uint32_t>(dev.core().uptimeMs());
    lostAtMs_ = now;
    join(now);
    updateAp(now);
}

uint32_t Network::apFingerprint() const {
    // Generations only grow, so the sum changes whenever one of them does.
    uint32_t sum = 0;
    for (const tesser::Node* n : apNodes_) sum += n->generation();
    return sum;
}

const WifiNetwork* Network::find(const std::string& ssid) const {
    for (const WifiNetwork& n : networks_) {
        if (ssid == n.ssid) return &n;
    }
    return nullptr;
}

StaConfig Network::config(const WifiNetwork& n) const {
    StaConfig c;
    c.ssid = n.ssid;
    c.password = n.password;
    if (n.ip.enabled) {
        c.staticIp = parseIpv4(n.ip.ip, c.ip) && parseIpv4(n.ip.mask, c.mask) && parseIpv4(n.ip.gateway, c.gateway);
        if (!c.staticIp) {
            tesser::logWarning("tesserkit: incomplete static address for \"%s\", using DHCP", n.ssid);
        } else if (!parseIpv4(n.ip.dns, c.dns)) {
            c.dns = c.gateway;
        }
    }
    return c;
}

void Network::join(uint32_t nowMs) {
    if (networks_.empty()) {
        sta_ = Sta::Idle;
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
    std::vector<std::pair<int, std::string>> seen;
    for (const WifiNetwork& n : networks_) {
        int best = -1000;
        for (const ScanResult& r : results) {
            if (r.ssid == n.ssid && r.rssi > best) best = r.rssi;
        }
        if (best > -1000) seen.emplace_back(best, n.ssid);
    }
    std::stable_sort(seen.begin(), seen.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    candidates_.clear();
    for (auto& s : seen) candidates_.push_back(s.second);
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
    wait(nowMs);
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
    StaInfo info;
    linkUp_ = true;
    if (wifi_->staInfo(info)) {
        linkSsid_ = info.ssid;
        linkIp_ = formatIpv4(info.ip);
        linkChannel_ = info.channel;
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", info.bssid[0], info.bssid[1], info.bssid[2],
                 info.bssid[3], info.bssid[4], info.bssid[5]);
        linkBssid_ = b;
    } else {
        linkSsid_ = current_.ssid;
    }
    staStatus_->changed();
    tesser::logWarning("tesserkit: joined \"%s\" as %s", linkSsid_.c_str(), linkIp_.c_str());
    if (linkChannel_) dev_.channel(linkChannel_);
}

void Network::lost(uint32_t nowMs) {
    linkUp_ = false;
    linkSsid_.clear();
    linkIp_.clear();
    linkBssid_.clear();
    linkChannel_ = 0;
    staStatus_->changed();
    lostAtMs_ = nowMs;
}

void Network::event(const Event& e) {
    if (!wifi_) return;
    uint32_t now = static_cast<uint32_t>(dev_.core().uptimeMs());
    switch (e.type) {
        case Event::Type::ScanDone:
            if (scanning_) scanDone(wifi_->scanResults(), now);
            break;
        case Event::Type::StaGotIp:
            if (sta_ == Sta::Connecting || sta_ == Sta::Connected) connected(now);
            break;
        case Event::Type::StaDisconnected:
            if (e.value == Event::kReasonLeft) break;
            if (sta_ == Sta::Connecting) {
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
        applyMdns();
    }
    if (mdnsNode_->generation() != mdnsGen_) {
        mdnsGen_ = mdnsNode_->generation();
        applyMdns();
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

void Network::applyMdns() {
    wifi_->mdns(mdns_, dev_.hostname().c_str(), dev_.version().c_str(), dev_.httpPort());
}

void Network::updateAp(uint32_t nowMs) {
    bool want;
    if (networks_.empty() || apMode_ == ApMode::Always) {
        want = true;  // recovery: a device without a network is always reachable
    } else if (apMode_ == ApMode::Never) {
        want = false;
    } else if (sta_ == Sta::Connected) {
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
            apIp_ = formatIpv4(wifi_->apIp());
            apClients_ = 0;
            apStatus_->changed();
            tesser::logWarning("tesserkit: access point \"%s\" at %s", c.ssid.c_str(), apIp_.c_str());
        }
    } else if (!want && apUp_) {
        wifi_->stopAp();
        apUp_ = false;
        apIp_.clear();
        apClients_ = 0;
        apStatus_->changed();
    }
    bool captive = apUp_ && sta_ != Sta::Connected;
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
    updateAp(nowMs);
    dev_.networkState(sta_ == Sta::Connected ? State::Online
                      : networks_.empty()    ? State::Provisioning
                                             : State::Connecting);
}

}  // namespace tesserkit
