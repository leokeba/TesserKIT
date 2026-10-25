#include "tesserkit/dmx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "tesserkit/limits.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr uint32_t kReopenMs = 5000;
constexpr size_t kMaxDatagram = 640;  // E1.31's largest data packet is 638 bytes
constexpr int kDrain = 64;            // datagrams a period, without the receiving task

}  // namespace

DmxService::DmxService(Device& dev, const char* name, DmxProtocol protocol, uint16_t port, uint16_t universe,
                       uint16_t first, uint16_t last)
    : Service(dev, name),
      lock_(tesser::kRankLeaf, "dmx"),
      protocol_(protocol),
      port_(port),
      first_(first),
      last_(last),
      universe_(universe) {}

DmxService& DmxService::enable(bool on) {
    enabled_ = on;
    return *this;
}

DmxService& DmxService::universe(uint16_t universe) {
    universe_ = universe;
    return *this;
}

DmxService& DmxService::map(const DmxMapping& m) {
    if (mappings_.size() < kMaxMappings) mappings_.push_back(m);
    return *this;
}

DmxService& DmxService::sink(DmxSink* sink, const DmxTask& task) {
    sink_ = sink;
    task_ = task;
    return *this;
}

void DmxService::universes(const std::vector<uint16_t>& list) {
    std::vector<uint16_t> u;
    for (uint16_t x : list) {
        if (x >= first_ && x <= last_) u.push_back(x);
    }
    std::sort(u.begin(), u.end());
    u.erase(std::unique(u.begin(), u.end()), u.end());
    if (u.size() > kMaxUniverses) u.resize(kMaxUniverses);
    tesser::MutexGuard g(lock_);
    listened_ = std::move(u);
    rebuild();
}

std::vector<uint16_t> DmxService::universes() const {
    tesser::MutexGuard g(lock_);
    return listened_;
}

std::string DmxService::mapped(const DmxMapping& m, const uint8_t* dmx, size_t len, tesser::ValueKind kind) {
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

void DmxService::describeDmx(tesser::Object& o, const char* universeDoc) {
    o.writeAccess(Access::Admin);
    o.value("enabled", enabled_).persist();
    universeNode_ = &o.value("universe", universe_).range(first_, last_).doc(universeDoc).persist().ui("widget", "number");
    mappingsNode_ =
        &o.list("mappings", mappings_,
                [this](tesser::Object& e, DmxMapping& m) {
                    e.value("path", m.path)
                        .validate([this](const std::string& v) {
                            // Before the service starts the tree may be
                            // incomplete: a boot load keeps what it stored.
                            if (v.size() > limits::kPath) return limits::length<limits::kPath>(v);
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
    s.value("packets", static_cast<const uint32_t&>(shown_.packets)).label("DMX packets").watch();
    s.value("universe", static_cast<const uint16_t&>(shown_.universe)).label("Last universe").watch();
    s.value("received", static_cast<const bool&>(shown_.received))
        .doc("A frame of this universe arrived since the start")
        .watch()
        .ui("hidden");
    s.value("age",
            [this]() -> int32_t {
                if (!shown_.received) return -1;
                // The receiving task's clock can be ahead of the period's.
                int32_t ms = static_cast<int32_t>(nowMs_ - shown_.frameMs);
                return ms > 0 ? ms / 1000 : 0;
            })
        .label("Last frame")
        .unit("s")
        .doc("Seconds since a frame of this universe; -1: none yet")
        .watch()
        .ui("visibleWhen", "received=true");
    s.value("sequenceErrors", static_cast<const uint32_t&>(shown_.sequenceErrors))
        .label("Sequence errors")
        .doc("Frames whose sequence number didn't follow their sender's previous frame: lost or reordered on the way")
        .watch()
        .ui("advanced");
    s.value("error", static_cast<const std::string&>(error_)).label("Last error");
    refused_ = &o.event("refused").label("Write refused").ui("toast", "warn").ui("hidden");
}

void DmxService::start() {
    Platform& p = dev_.platform();
    if (!p.udp) p.udp = espUdp();
    udp_ = p.udp;
    if (!udp_) tesser::logError("tesserkit: %s has no UDP", name());
    client_.reset(new tesser::LocalClient(dev_.api()));  // Admin, as the mappings were set
    settingsGen_ = universeNode_->generation() + mappingsNode_->generation();
    sent_.assign(mappings_.size(), std::string());
    tesser::MutexGuard g(lock_);
    rebuild();
}

void DmxService::tick(uint32_t nowMs) {
    nowMs_ = nowMs;
    if (client_) client_->process();
    uint32_t gen = universeNode_->generation() + mappingsNode_->generation();
    if (gen != settingsGen_) {
        // New mappings or universe: everything is written again from the
        // next frame.
        settingsGen_ = gen;
        sent_.assign(mappings_.size(), std::string());
        tesser::MutexGuard g(lock_);
        frameLen_ = 0;
        fresh_ = false;
        rebuild();
    }
    if (!enabled_) {
        if (socket_) closing();
        socket_.reset();  // waits for the receiving task to end
        listening_ = false;
        opening_ = false;
        return;
    }
    if (!socket_) {
        if (!udp_ || (opening_ && nowMs - openedAtMs_ < kReopenMs)) return;
        opening_ = true;
        openedAtMs_ = nowMs;
        socket_ = udp_->open(port_);
        if (!socket_) {
            char e[40];
            snprintf(e, sizeof(e), "can't open UDP port %u", static_cast<unsigned>(port_));
            failed(e);
            return;
        }
        if (sink_) {
            listening_ = socket_->listen(name(), task_.priority, task_.core, task_.stackBytes,
                                         [this](const uint8_t* d, size_t n, uint32_t ip, uint64_t atUs) {
                                             received(d, n, ip, atUs);
                                         });
            if (!listening_) tesser::logWarning("tesserkit: %s: no receiving task, frames come every period", name());
        }
        opened();
    }
    if (!listening_) receive(nowMs);
    ticked(nowMs);
    uint8_t dmx[512];
    size_t len = 0;
    bool fresh;
    {
        tesser::MutexGuard g(lock_);
        // What the status shows, as of this period.
        shown_.packets = packets_;
        shown_.sequenceErrors = sequenceErrors_;
        shown_.universe = lastUniverse_;
        if (const Track* t = track(universe_)) {
            shown_.received = t->everFrame;
            shown_.frameMs = t->frameMs;
        }
        fresh = fresh_;
        if (fresh) {
            fresh_ = false;
            len = frameLen_;
            memcpy(dmx, frame_, len);
        }
    }
    if (fresh) apply(dmx, len);
}

void DmxService::receive(uint32_t nowMs) {
    uint8_t buf[kMaxDatagram];
    // Consoles send up to 44 frames a second for each universe.
    for (int i = 0; i < kDrain; i++) {
        uint32_t ip = 0;
        uint16_t port = 0;
        size_t n = socket_->receive(buf, sizeof(buf), ip, port);
        if (n == 0) break;
        received(buf, n, ip, uint64_t(nowMs) * 1000);
    }
}

void DmxService::rebuild() {
    std::vector<uint16_t> all = listened_;
    auto at = std::lower_bound(all.begin(), all.end(), universe_);
    if (at == all.end() || *at != universe_) all.insert(at, universe_);
    std::vector<Track> tracks;
    std::vector<Source> sources;
    tracks.reserve(all.size());
    if (protocol_ == DmxProtocol::Sacn) sources.reserve(all.size() * kMaxSources);
    for (uint16_t u : all) {
        const Track* old = track(u);
        Track t = old ? *old : Track();
        t.universe = u;
        t.sink = std::binary_search(listened_.begin(), listened_.end(), u);
        t.mapped = u == universe_;
        tracks.push_back(t);
        if (protocol_ != DmxProtocol::Sacn) continue;
        for (size_t i = 0; i < kMaxSources; i++) sources.push_back(old ? sources_[static_cast<size_t>(old - tracks_.data()) * kMaxSources + i] : Source());
    }
    tracks_.swap(tracks);
    sources_.swap(sources);
}

void DmxService::counted(uint16_t universe) {
    packets_++;
    lastUniverse_ = universe;
}

DmxService::Track* DmxService::track(uint16_t universe) {
    auto at = std::lower_bound(tracks_.begin(), tracks_.end(), universe,
                               [](const Track& t, uint16_t u) { return t.universe < u; });
    return at != tracks_.end() && at->universe == universe ? &*at : nullptr;
}

bool DmxService::take(Track* t, const DmxFrame& f) {
    t->everFrame = true;
    t->frameMs = static_cast<uint32_t>(f.atUs / 1000);
    if (t->mapped) {
        size_t len = f.length < sizeof(frame_) ? f.length : sizeof(frame_);
        memcpy(frame_, f.data, len);
        frameLen_ = len;
        fresh_ = true;
    }
    return t->sink && sink_;
}

void DmxService::sync(const DmxSync& s) {
    if (sink_) sink_->sync(s);
}

void DmxService::sequenced(uint8_t last, uint8_t now) {
    // Art-Net numbers 1 to 255, then 1 again; E1.31 0 to 255.
    uint8_t next = static_cast<uint8_t>(last + 1);
    if (protocol_ == DmxProtocol::ArtNet && next == 0) next = 1;
    if (now != next) sequenceErrors_++;
}

void DmxService::apply(const uint8_t* dmx, size_t len) {
    if (sent_.size() != mappings_.size()) sent_.assign(mappings_.size(), std::string());
    for (size_t i = 0; i < mappings_.size(); i++) {
        const DmxMapping& m = mappings_[i];
        if (m.path.empty()) continue;
        tesser::ValueKind kind = tesser::ValueKind::Number;
        const tesser::Node* node = findNode(dev_.api(), m.path);
        if (node && node->type() == tesser::NodeType::Value) kind = static_cast<const tesser::ValueNode*>(node)->kind();
        std::string v = mapped(m, dmx, len, kind);
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

void DmxService::failed(const std::string& error) {
    if (error == error_) return;
    if (!error.empty()) tesser::logWarning("tesserkit: %s: %s", name(), error.c_str());
    error_ = error;
    status_->changed();
    if (!error.empty()) refused_->emit(error);
}

}  // namespace tesserkit
