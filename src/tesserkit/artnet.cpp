#include "tesserkit/artnet.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr uint8_t kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
constexpr uint16_t kOpPoll = 0x2000;
constexpr uint16_t kOpPollReply = 0x2100;
constexpr uint16_t kOpDmx = 0x5000;
constexpr size_t kDmxHeader = 18;
constexpr size_t kPollReplySize = 239;
constexpr uint32_t kReopenMs = 5000;
constexpr uint32_t kGoodOutputMs = 3000;  // a frame this recent: "data being output"

void copyName(uint8_t* dst, const char* src, size_t cap) {
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
}

}  // namespace

ArtNet& ArtNet::enable(bool on) {
    enabled_ = on;
    return *this;
}

ArtNet& ArtNet::universe(uint16_t portAddress) {
    universe_ = portAddress;
    return *this;
}

ArtNet& ArtNet::map(const ArtNetMapping& m) {
    if (mappings_.size() < kMaxMappings) mappings_.push_back(m);
    return *this;
}

std::string ArtNet::mapped(const ArtNetMapping& m, const uint8_t* dmx, size_t len, tesser::ValueKind kind) {
    if (m.coarse == 0 || m.coarse > len || m.fine > len) return "";
    uint8_t coarse = dmx[m.coarse - 1];
    if (m.threshold) return coarse >= m.threshold ? "true" : "false";
    // 8 bits fill the range too: 255 is the maximum, as 65535 is.
    uint32_t v = m.fine ? (uint32_t(coarse) << 8 | dmx[m.fine - 1]) : coarse * 257u;
    if (kind == tesser::ValueKind::Boolean) return v >= 32768 ? "true" : "false";
    double x = m.min + static_cast<double>(v) / 65535.0 * (m.max - m.min);
    char buf[32];
    if (kind == tesser::ValueKind::Integer) {
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(llround(x)));
    } else {
        snprintf(buf, sizeof(buf), "%.7g", x);
    }
    return buf;
}

void ArtNet::describe(tesser::Object& o) {
    o.label("Art-Net")
        .ui("icon", "bulb")
        .doc("Lighting consoles drive nodes of the tree through the DMX channels of one universe")
        .writeAccess(Access::Admin);
    o.value("enabled", enabled_).persist();
    universeNode_ = &o.value("universe", universe_)
                         .range(0, 32767)
                         .doc("The port-address: net, sub-net and universe as one 15-bit number")
                         .persist();
    mappingsNode_ =
        &o.list("mappings", mappings_,
                [this](tesser::Object& e, ArtNetMapping& m) {
                    e.value("path", m.path)
                        .validate([this](const std::string& v) {
                            // Before the service starts the tree may be
                            // incomplete: a boot load keeps what it stored.
                            return v.empty() || !client_ || resolves(dev_.api(), v)
                                       ? Check::ok()
                                       : Check::fail(Status::NotFound, "no such node");
                        })
                        .ui("placeholder", "/lamp/brightness");
                    e.value("coarse", m.coarse).range(0, 512).label("Channel").doc("1 to 512; 0: unused");
                    e.value("fine", m.fine).range(0, 512).label("Fine channel").doc("The low byte's channel; 0: 8 bits");
                    e.value("min", m.min);
                    e.value("max", m.max);
                    e.value("threshold", m.threshold)
                        .range(0, 255)
                        .label("Gate")
                        .doc("1 to 255: writes true while the channel is at or above it; 0: a value from min to max")
                        .ui("advanced");
                })
             .maxSize(kMaxMappings)
             .persist()
             .doc("Each mapping writes its channels to a node when they change");
    auto& s = o.object("status").label("Status");
    status_ = &s;
    s.value("packets", static_cast<const uint32_t&>(packets_)).label("DMX packets").watch();
    s.value("universe", static_cast<const uint16_t&>(lastUniverse_)).label("Last universe").watch();
    s.value("received", static_cast<const bool&>(everFrame_))
        .doc("A frame of this universe arrived since the start")
        .watch()
        .ui("hidden");
    s.value("age", [this]() -> int32_t {
          return everFrame_ ? static_cast<int32_t>((nowMs_ - frameAtMs_) / 1000) : -1;
      })
        .label("Last frame")
        .unit("s")
        .doc("Seconds since a frame of this universe; -1: none yet")
        .watch()
        .ui("visibleWhen", "received=true");
    s.value("error", static_cast<const std::string&>(error_)).label("Last error");
    refused_ = &o.event("refused").label("Write refused").ui("toast", "warn").ui("hidden");
}

void ArtNet::start() {
    Platform& p = dev_.platform();
    if (!p.udp) p.udp = espUdp();
    udp_ = p.udp;
    if (!udp_) tesser::logError("tesserkit: ArtNet has no UDP");
    client_.reset(new tesser::LocalClient(dev_.api()));  // Admin, as the mappings were set
    settingsGen_ = universeNode_->generation() + mappingsNode_->generation();
    sent_.assign(mappings_.size(), std::string());
}

void ArtNet::tick(uint32_t nowMs) {
    nowMs_ = nowMs;
    if (client_) client_->process();
    uint32_t gen = universeNode_->generation() + mappingsNode_->generation();
    if (gen != settingsGen_) {
        // New mappings or universe: everything is written again from the
        // next frame.
        settingsGen_ = gen;
        sent_.assign(mappings_.size(), std::string());
        frameLen_ = 0;
        fresh_ = false;
    }
    if (!enabled_) {
        socket_.reset();
        opening_ = false;
        return;
    }
    if (!socket_) {
        if (!udp_ || (opening_ && nowMs - openedAtMs_ < kReopenMs)) return;
        opening_ = true;
        openedAtMs_ = nowMs;
        socket_ = udp_->open(kPort);
        if (!socket_) {
            failed("can't open UDP port 6454");
            return;
        }
    }
    receive(nowMs);
    if (fresh_) {
        fresh_ = false;
        apply();
    }
}

void ArtNet::receive(uint32_t nowMs) {
    uint8_t buf[kDmxHeader + 512];
    // Consoles send up to 44 frames a second: a period holds one or two.
    for (int i = 0; i < 32; i++) {
        uint32_t ip = 0;
        uint16_t port = 0;
        size_t n = socket_->receive(buf, sizeof(buf), ip, port);
        if (n == 0) break;
        if (n < 10 || memcmp(buf, kId, sizeof(kId)) != 0) continue;
        uint16_t op = static_cast<uint16_t>(buf[8] | buf[9] << 8);
        if (op == kOpPoll) {
            pollReply(ip);
            continue;
        }
        if (op != kOpDmx || n < kDmxHeader) continue;
        uint16_t u = static_cast<uint16_t>(buf[14] | (buf[15] & 0x7F) << 8);
        size_t len = static_cast<size_t>(buf[16] << 8 | buf[17]);
        if (len > n - kDmxHeader) len = n - kDmxHeader;
        if (len > sizeof(frame_)) len = sizeof(frame_);
        packets_++;
        lastUniverse_ = u;
        if (u != universe_) continue;
        memcpy(frame_, buf + kDmxHeader, len);
        frameLen_ = len;
        fresh_ = true;
        everFrame_ = true;
        frameAtMs_ = nowMs;
    }
}

void ArtNet::apply() {
    if (sent_.size() != mappings_.size()) sent_.assign(mappings_.size(), std::string());
    for (size_t i = 0; i < mappings_.size(); i++) {
        const ArtNetMapping& m = mappings_[i];
        if (m.path.empty()) continue;
        tesser::ValueKind kind = tesser::ValueKind::Number;
        const tesser::Node* node = findNode(dev_.api(), m.path);
        if (node && node->type() == tesser::NodeType::Value) kind = static_cast<const tesser::ValueNode*>(node)->kind();
        std::string v = mapped(m, frame_, frameLen_, kind);
        if (v.empty() || v == sent_[i]) continue;
        sent_[i] = v;
        std::string path = m.path;
        client_->set(path, v, [this, path](Status s, JsonVariantConst body) {
            if (s == Status::Ok) {
                // A path that works again clears its error.
                if (error_.compare(0, path.size() + 1, path + ":") == 0) failed("");
                return;
            }
            std::string e = path + ": " + tesser::toString(s);
            if (const char* msg = body["message"]) e = e + ": " + msg;
            failed(e);
        });
    }
}

void ArtNet::failed(const std::string& error) {
    if (error == error_) return;
    if (!error.empty()) tesser::logWarning("tesserkit: Art-Net: %s", error.c_str());
    error_ = error;
    status_->changed();
    if (!error.empty()) refused_->emit(error);
}

void ArtNet::pollReply(uint32_t ip) {
    uint32_t me = 0;
    if (Wifi* w = dev_.platform().wifi) {
        StaInfo sta;
        me = w->staInfo(sta) ? sta.ip : w->apIp();
    }
    uint8_t r[kPollReplySize] = {};
    memcpy(r, kId, sizeof(kId));
    r[8] = kOpPollReply & 0xFF;
    r[9] = kOpPollReply >> 8;
    for (int i = 0; i < 4; i++) r[10 + i] = r[207 + i] = static_cast<uint8_t>(me >> (24 - 8 * i));  // IP, BindIp
    r[14] = kPort & 0xFF;  // low byte first
    r[15] = kPort >> 8;
    unsigned major = 0, minor = 0;
    sscanf(dev_.version().c_str(), "%u.%u", &major, &minor);
    r[16] = static_cast<uint8_t>(major);
    r[17] = static_cast<uint8_t>(minor);
    r[18] = static_cast<uint8_t>(universe_ >> 8 & 0x7F);  // NetSwitch
    r[19] = static_cast<uint8_t>(universe_ >> 4 & 0x0F);  // SubSwitch
    r[23] = 0xD0;  // Status1: indicators normal, port-address set locally
    copyName(r + 26, dev_.label(), 18);
    copyName(r + 44, dev_.hostname().c_str(), 64);
    char report[64];
    snprintf(report, sizeof(report), "#0001 [%04u] %s", static_cast<unsigned>(packets_ % 10000),
             error_.empty() ? "OK" : error_.c_str());
    copyName(r + 108, report, 64);
    r[173] = 1;     // NumPorts
    r[174] = 0x80;  // PortTypes[0]: outputs DMX512 from Art-Net
    bool good = everFrame_ && nowMs_ - frameAtMs_ < kGoodOutputMs;
    r[182] = good ? 0x80 : 0;  // GoodOutputA[0]: data being output
    r[190] = static_cast<uint8_t>(universe_ & 0x0F);  // SwOut[0]
    r[200] = 0x00;  // Style: a node
    memcpy(r + 201, dev_.info().mac, 6);
    r[211] = 1;     // BindIndex
    r[212] = 0x08;  // Status2: 15-bit port-addresses
    // Unicast to the controller, on the Art-Net port (Art-Net 4).
    if (ip) socket_->send(ip, kPort, r, sizeof(r));
}

}  // namespace tesserkit
