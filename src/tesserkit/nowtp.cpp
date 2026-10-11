#include "tesserkit/nowtp.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr uint32_t kPollMs = 1000;    // the radio's status and the nodes nearby
constexpr uint32_t kSettleMs = 20000;  // at the latest: the access point is up by then

}  // namespace

NowTp& NowTp::enable(bool on) {
    enabled_ = on;
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

NowTp& NowTp::pairingCode(const char* code) {
    pairingCode_ = code ? code : "";
    return *this;
}

void NowTp::describe(tesser::Object& o) {
    o.label("NowTP")
        .ui("icon", "target")
        .doc("Nodes nearby over ESP-NOW, with no network in common: their pages here, this device on theirs")
        .writeAccess(Access::Admin);
    enabledNode_ = &o.value("enabled", enabled_).persist();
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
                             e.value("tesser", static_cast<const bool&>(n.tesser)).label("TesserAPI");
                             e.value("authenticated", static_cast<const bool&>(n.authenticated)).label("Paired");
                         })
                      .key("mac")
                      .maxSize(kMaxNodes)
                      .readOnly()
                      .label("Nodes nearby");

    auto& s = o.object("status").label("Status");
    statusNode_ = &s;
    s.value("started", [this] { return status_.started; }).label("Running");
    s.value("channel", [this] { return status_.channel; });
    s.value("timeSynced", [this] { return status_.timeSynced; })
        .label("Network time")
        .doc("Synchronized with the node whose clock is the network time (for actions started together)");
    s.value("timeReference", [this] { return status_.timeReference; }).label("Time reference").ui("advanced");
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
    enabledGen_ = enabledNode_->generation();
    channelGen_ = channelNode_->generation();
    longRangeGen_ = longRangeNode_->generation();
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
    Device& dev = dev_;
    started_ = radio_->begin(dev.api(), peersObject_, c, [&dev](Event e) { dev.post(std::move(e)); });
    if (!started_) tesser::logError("tesserkit: NowTP didn't start");
    refresh();
}

void NowTp::refresh() {
    if (!radio_) return;
    NowTpStatus st = started_ ? radio_->status() : NowTpStatus();
    bool changed = st.started != status_.started || st.channel != status_.channel || st.secured != status_.secured ||
                   st.timeSynced != status_.timeSynced || st.timeReference != status_.timeReference ||
                   st.sent != status_.sent || st.received != status_.received || st.failed != status_.failed;
    status_ = st;
    if (changed) statusNode_->changed();
    std::vector<Node> nodes;
    if (started_) {
        for (const NowTpPeer& p : radio_->peers()) {
            if (nodes.size() >= kMaxNodes) break;
            Node n;
            n.name = p.name;
            n.mac = p.mac;
            n.rssi = p.rssi;
            n.channel = p.channel;
            n.tesser = p.tesser;
            n.authenticated = p.authenticated;
            nodes.push_back(n);
        }
    }
    bool same = nodes.size() == nodes_.size();
    for (size_t i = 0; same && i < nodes.size(); i++) {
        const Node &a = nodes[i], &b = nodes_[i];
        same = a.name == b.name && a.mac == b.mac && a.rssi == b.rssi && a.channel == b.channel &&
               a.tesser == b.tesser && a.authenticated == b.authenticated;
    }
    if (!same) {
        nodes_ = std::move(nodes);
        nodesNode_->changed();
    }
}

void NowTp::setPairing(const std::string& s) {
    if (s == pairing_) return;
    pairing_ = s;
    pairingNode_->changed();
}

Check NowTp::pairing(bool accept) {
    if (!radio_ || !started_) return Check::fail(Status::NotAllowed, "NowTP is off");
    if (pairingBusy_) return Check::fail(Status::Busy, "a pairing is in progress");
    if (!(accept ? radio_->acceptPairing(kPairingMs) : radio_->pair(kPairingMs))) {
        return Check::fail(Status::Internal, "the pairing couldn't start");
    }
    pairingBusy_ = true;
    setPairing(accept ? "Waiting for a node to join" : "Looking for a node with an open pairing window");
    return Check::ok();
}

void NowTp::tick(uint32_t nowMs) {
    if (enabled_ && !started_ && !begun_ && settled(nowMs)) {
        begun_ = true;  // once: a radio that won't start isn't retried every period
        begin();
    }
    uint16_t gen = enabledNode_->generation();
    if (gen != enabledGen_) {
        enabledGen_ = gen;
        if (enabled_ && !started_) {
            begin();
        } else if (!enabled_ && started_) {
            radio_->end();
            started_ = false;
            pairingBusy_ = false;
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
            pairingBusy_ = false;
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
