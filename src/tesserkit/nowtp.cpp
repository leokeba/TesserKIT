#include "tesserkit/nowtp.h"

#include "tesserkit/limits.h"
#include "tesserkit/peers.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr uint32_t kPollMs = 1000;    // the radio's status and the nodes nearby
constexpr uint32_t kSettleMs = 20000;  // at the latest: the access point is up by then

// "auto", "this", or a MAC ("aa:bb:cc:dd:ee:ff", either case).
Check checkTimeReference(const std::string& v) {
    if (v == "auto" || v == "this") return Check::ok();
    bool mac = v.size() == 17;
    for (size_t i = 0; mac && i < v.size(); i++) {
        char c = v[i];
        mac = i % 3 == 2 ? c == ':' : (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    return mac ? Check::ok() : Check::fail(Status::InvalidValue, "auto, this, or a node's MAC (aa:bb:cc:dd:ee:ff)");
}

}  // namespace

NowTp& NowTp::enable(bool on) {
    enabled_ = on;
    return *this;
}

NowTp& NowTp::ownTask(bool own) {
    ownTask_ = own;
    return *this;
}

NowTp& NowTp::maxCalls(size_t n) {
    maxCalls_ = n;
    return *this;
}

NowTp& NowTp::mountPeers(bool on) {
    mountPeers_ = on;
    return *this;
}

NowTp& NowTp::channel(uint8_t ch) {
    channel_ = ch;
    return *this;
}

NowTp& NowTp::longRange(bool on) {
    longRange_ = on;
    return *this;
}

NowTp& NowTp::txPower(int8_t dBm) {
    txPower_ = dBm;
    return *this;
}

NowTp& NowTp::timeReference(const char* ref) {
    timeReference_ = ref ? ref : "auto";
    return *this;
}

NowTp& NowTp::timeInterval(uint32_t seconds) {
    timeInterval_ = seconds;
    return *this;
}

uint64_t NowTp::networkTimeUs() const {
    if (radio_) return radio_->networkTimeUs();
    return dev_.core().uptimeMs() * 1000;
}

int64_t NowTp::localTimeUs(uint64_t networkUs) const {
    if (radio_) return radio_->localTimeUs(networkUs);
    return static_cast<int64_t>(networkUs);
}

NowTp& NowTp::pairingCode(const char* code) {
    pairingCode_ = code ? code : "";
    return *this;
}

void NowTp::describe(tesser::Object& o) {
    o.label("NowTP")
        .ui("group", "Nodes")
        .ui("summary", "enabled")
        .ui("icon", "target")
        .doc("Nodes nearby over ESP-NOW, with no network in common: their pages here, this device on theirs")
        .writeAccess(Access::Admin);
    enabledNode_ = &o.value("enabled", enabled_)
                        .validate([this](bool on) {
                            // Stopped under a pairing task, NowTP would be
                            // pulled from under it.
                            return on || !pairingRuns() ? Check::ok()
                                                 : Check::fail(Status::Busy, "a pairing is in progress (up to 60 s)");
                        })
                        .persist();
    restartSettings_.clear();
    restartSettings_.push_back(&o.value("mountPeers", mountPeers_)
                                    .persist()
                                    .label("Show nodes nearby")
                                    .doc("Mounts the TesserAPI nodes found nearby under /peers, each with its page"));
    channelNode_ = &o.value("channel", channel_)
                        .range(1, 13)
                        .persist()
                        .doc("When neither a Wi-Fi network nor the access point fixes it; nodes nearby follow")
                        .ui("advanced");
    longRangeNode_ = &o.value("longRange", longRange_)
                          .persist()
                          .label("Long range")
                          .doc("Espressif's Long Range mode, besides 802.11b/g/n: farther, slower")
                          .ui("advanced");
    restartSettings_.push_back(&o.value("pairingCode", pairingCode_)
                                    .validate(limits::atMost<limits::kSecret>)
                                    .secret()
                                    .persist()
                                    .label("Pairing code")
                                    .doc("Both sides of a pairing must know it, so a node can't join the wrong "
                                         "installation")
                                    .ui("advanced"));
    restartNode_ = &o.value("restartNeeded", [this] { return restart_; }).ui("hidden");
    o.value("restartWarning", "Some changes apply after a restart.")
        .label("Restart needed")
        .ui("style", "warning")
        .ui("visibleWhen", "restartNeeded=true");

    auto& sec = o.object("pairing").label("Pairing").doc(
        "Nodes holding the installation key trust each other (Admin); messages without it are dropped");
    sec.value("secured", [this] { return status_.secured; }).label("Paired");
    pairingNode_ = &sec.value("state", static_cast<const std::string&>(pairing_)).label("Pairing");
    sec.action("pair", [this] { return pairing(false); })
        .label("Join")
        .doc("Takes the installation key from a node whose pairing window is open (60 s)");
    sec.action("accept", [this] { return pairing(true); })
        .label("Let a node join")
        .doc("Opens this node's pairing window for 60 s");
    sec.action("forget", [this]() -> Check {
           if (!radio_ || !started_) return Check::fail(Status::NotAllowed, "NowTP is off");
           if (pairingRuns()) return Check::fail(Status::Busy, "a pairing is in progress (up to 60 s)");
           if (!radio_->forgetKey()) return Check::fail(Status::Internal, "the key couldn't be removed");
           setPairing("");
           refresh();
           return Check::ok();
       })
        .label("Forget the key")
        .ui("style", "danger")
        .ui("confirm", "Leave the installation? Its nodes won't trust this one any more.")
        .ui("advanced");

    nodesNode_ = &o.list("nodes", nodes_,
                         [](tesser::Object& e, Node& n) {
                             e.value("name", static_cast<const std::string&>(n.name));
                             e.value("mac", static_cast<const std::string&>(n.mac)).label("MAC");
                             e.value("rssi", static_cast<const int32_t&>(n.rssi)).unit("dBm").label("Signal");
                             e.value("channel", static_cast<const uint8_t&>(n.channel));
                             // Its access point holds it on another channel:
                             // seen, but no message reaches it.
                             e.value("otherChannel", static_cast<const bool&>(n.otherChannel))
                                 .label("On another channel")
                                 .ui("style", "warning")
                                 .ui("visibleWhen", "otherChannel=true");
                             e.value("tesser", static_cast<const bool&>(n.tesser)).label("TesserAPI");
                             e.value("authenticated", static_cast<const bool&>(n.authenticated)).label("Paired");
                         })
                      .key("mac")
                      .maxSize(kMaxNodes)
                      .readOnly()
                      .label("Nodes nearby");

    auto& t = o.object("time").label("Network time").doc(
        "A clock shared by the nodes nearby, the reference node's: actions started at the same instant on each");
    timeNode_ = &t;
    timeReferenceNode_ = &t.value("reference", timeReference_)
                              .validate(checkTimeReference)
                              .persist()
                              .label("Reference")
                              .doc("auto: the nodes elect one among themselves; this: this node's clock; or the "
                                   "MAC of the node to follow")
                              // "this" applied to all would make every node the reference.
                              .ui("perNode");
    timeIntervalNode_ = &t.value("interval", timeInterval_)
                             .range(1, 600)
                             .unit("s")
                             .persist()
                             .label("Interval")
                             .doc("Between synchronization rounds, once synchronized")
                             .ui("advanced");
    t.value("synced", [this] { return status_.timeSynced; }).label("Synchronized");
    t.value("isReference", [this] { return status_.timeReference; }).label("This node is the reference");
    t.value("source", [this] { return status_.timeReferenceMac; })
        .label("Reference node")
        .doc("The MAC of the node whose clock is the network time");
    t.value("offset", [this] { return status_.timeOffsetUs; })
        .unit("µs")
        .label("Offset")
        .doc("Network time minus this node's clock")
        .ui("advanced");
    t.value("roundTrip", [this] { return status_.timeRoundTripUs; })
        .unit("µs")
        .label("Round trip")
        .doc("The shortest of the last round's exchanges with the reference")
        .ui("advanced");
    t.value("drift", [this] { return status_.timeDriftPpm; })
        .unit("ppm")
        .label("Drift")
        .doc("How fast the offset grows: the two crystals' difference")
        .ui("advanced");
    t.value("rounds", [this] { return status_.timeRounds; })
        .label("Rounds")
        .doc("Synchronization rounds with the current reference")
        .ui("advanced");
    t.value("age", [this] { return status_.timeAgeMs; })
        .unit("ms")
        .label("Last round")
        .doc("Since the last successful round")
        .ui("advanced");

    auto& s = o.object("status").label("Status");
    statusNode_ = &s;
    s.value("started", [this] { return status_.started; }).label("Running");
    s.value("channel", [this] { return status_.channel; });
    s.value("sent", [this] { return status_.sent; }).label("Messages sent").ui("advanced");
    s.value("received", [this] { return status_.received; }).label("Messages received").ui("advanced");
    s.value("failed", [this] { return status_.failed; }).label("Messages failed").ui("advanced");
}

uint32_t NowTp::restartGen() const {
    // Generations only grow, so the sum changes whenever one of them does.
    uint32_t sum = 0;
    for (const tesser::Node* n : restartSettings_) sum += n->generation();
    return sum;
}

void NowTp::start() {
    Platform& p = dev_.platform();
    if (!p.nowtp) p.nowtp = espNowTp();
    radio_ = p.nowtp;
    if (!radio_) tesser::logError("tesserkit: NowTp has no NowTP in this build");
    if (radio_ && !ownTask_) {
        NowTpRadio* radio = radio_;
        dev_.poller([radio] { radio->poll(); });
    }
    enabledGen_ = enabledNode_->generation();
    channelGen_ = channelNode_->generation();
    longRangeGen_ = longRangeNode_->generation();
    timeReferenceGen_ = timeReferenceNode_->generation();
    timeIntervalGen_ = timeIntervalNode_->generation();
    restartGen_ = restartGen();
    // Nodes found are mounted at the root, beside the application's tree,
    // where TesserUI's menu shows them (TesserUI's DESIGN.md section 6.3).
    if (mountPeers_) peersObject_ = &dev_.api().object("peers").label("Nodes nearby").ui("icon", "target");
    // The radio starts in tick(), once the network has settled.
    startMs_ = static_cast<uint32_t>(dev_.core().uptimeMs());
}

bool NowTp::settled(uint32_t nowMs) const {
    // While the station scans and joins, ESP-NOW can't hold a channel (the
    // driver warns on every frame); joined, or on its own access point, the
    // channel is fixed and nodes nearby can follow it. A network that never
    // answers keeps the device connecting: the fallback access point is up
    // by kSettleMs.
    State s = dev_.state();
    return (s != State::Booting && s != State::Connecting) || nowMs - startMs_ >= kSettleMs;
}

void NowTp::begin() {
    if (!radio_ || started_) return;
    NowTpConfig c;
    c.name = dev_.hostname();
    c.channel = channel_;
    c.longRange = longRange_;
    c.txPower = txPower_;
    c.pairingCode = pairingCode_;
    c.maxCalls = maxCalls_;
    c.runTask = ownTask_;
    c.timeReference = timeReference_;
    c.timeIntervalMs = timeInterval_ * 1000;
    if (!ownTask_) c.queued = [&dev = dev_] { dev.wake(); };
    Device& dev = dev_;
    bool had = radio_->endpoint() != nullptr;
    started_ = radio_->begin(dev.api(), peersObject_, c, [&dev](Event e) { dev.post(std::move(e)); });
    if (!started_) tesser::logError("tesserkit: NowTP didn't start");
    if (!had && radio_->endpoint()) Peers::watch(dev, *radio_->endpoint());
    refresh();
}

void NowTp::refresh() {
    if (!radio_) return;
    NowTpStatus st = started_ ? radio_->status() : NowTpStatus();
    bool changed = st.started != status_.started || st.channel != status_.channel || st.secured != status_.secured ||
                   st.sent != status_.sent || st.received != status_.received || st.failed != status_.failed;
    // The offset, drift and age move all the time: read when asked, announced
    // with a round.
    bool timeChanged = st.timeSynced != status_.timeSynced || st.timeReference != status_.timeReference ||
                       st.timeReferenceMac != status_.timeReferenceMac || st.timeRounds != status_.timeRounds;
    status_ = std::move(st);
    if (changed) statusNode_->changed();
    if (timeChanged) timeNode_->changed();
    std::vector<Node> nodes;
    if (started_) {
        for (const NowTpPeer& p : radio_->peers()) {
            if (nodes.size() >= kMaxNodes) break;
            Node n;
            n.name = p.name;
            n.mac = p.mac;
            n.rssi = p.rssi;
            n.channel = p.channel;
            n.otherChannel = p.otherChannel;
            n.tesser = p.tesser;
            n.authenticated = p.authenticated;
            nodes.push_back(n);
        }
    }
    bool same = nodes.size() == nodes_.size();
    for (size_t i = 0; same && i < nodes.size(); i++) {
        const Node &a = nodes[i], &b = nodes_[i];
        same = a.name == b.name && a.mac == b.mac && a.rssi == b.rssi && a.channel == b.channel &&
               a.otherChannel == b.otherChannel && a.tesser == b.tesser && a.authenticated == b.authenticated;
    }
    if (!same) {
        nodes_ = std::move(nodes);
        nodesNode_->changed();
    }
}

bool NowTp::pairingRuns() const {
    // The radio's word, not a flag its NowTpPairing clears: that event can
    // be lost in a full queue, and the flag would stay set.
    return started_ && radio_ && radio_->pairing();
}

void NowTp::setPairing(const std::string& s) {
    if (s == pairing_) return;
    pairing_ = s;
    pairingNode_->changed();
}

Check NowTp::pairing(bool accept) {
    if (!radio_ || !started_) return Check::fail(Status::NotAllowed, "NowTP is off");
    if (pairingRuns()) return Check::fail(Status::Busy, "a pairing is in progress");
    if (!(accept ? radio_->acceptPairing(kPairingMs) : radio_->pair(kPairingMs))) {
        return Check::fail(Status::Internal, "the pairing couldn't start");
    }
    setPairing(accept ? "Waiting for a node to join" : "Looking for a node with an open pairing window");
    return Check::ok();
}

void NowTp::applyTimeReference() {
    if (!started_) return;
    if (!radio_->timeReference(timeReference_)) tesser::logError("tesserkit: NowTP refused the time reference");
    refresh();
}

void NowTp::tick(uint32_t nowMs) {
    if (enabled_ && !started_ && !begun_ && settled(nowMs)) {
        begun_ = true;  // once: a radio that won't start isn't retried every period
        begin();
    }
    uint32_t gen = enabledNode_->generation();
    if (gen != enabledGen_) {
        enabledGen_ = gen;
        if (enabled_ && !started_) {
            begin();
        } else if (!enabled_ && started_) {
            radio_->end();
            started_ = false;
            setPairing("");
            refresh();
        }
    }
    if ((gen = channelNode_->generation()) != channelGen_) {
        channelGen_ = gen;
        if (started_) radio_->channel(channel_);
    }
    if ((gen = longRangeNode_->generation()) != longRangeGen_) {
        longRangeGen_ = gen;
        if (started_) radio_->longRange(longRange_);
    }
    if ((gen = timeReferenceNode_->generation()) != timeReferenceGen_) {
        timeReferenceGen_ = gen;
        applyTimeReference();
    }
    if ((gen = timeIntervalNode_->generation()) != timeIntervalGen_) {
        timeIntervalGen_ = gen;
        if (started_) radio_->timeInterval(timeInterval_ * 1000);
    }
    uint32_t rg = restartGen();
    if (rg != restartGen_) {
        restartGen_ = rg;
        if (!restart_) {
            restart_ = true;
            restartNode_->changed();
        }
    }
    if (nowMs - polledAtMs_ >= kPollMs) {
        polledAtMs_ = nowMs;
        refresh();
    }
}

void NowTp::event(const Event& e) {
    switch (e.type) {
        case Event::Type::NowTpPeers:
        case Event::Type::NowTpChannel:
            refresh();
            return;
        case Event::Type::NowTpPairing:
            setPairing(e.value == 0 ? "Paired with " + e.text : "Not paired: " + e.text);
            refresh();
            return;
        default:
            return;
    }
}

// The key lives in NowTP's own storage, which the device's erase doesn't reach.
void NowTp::factoryReset() {
    if (radio_) radio_->forgetKey();
}

}  // namespace tesserkit
