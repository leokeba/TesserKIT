#include "tesserkit/nowtp.h"

#include "tesserkit/network_clock.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

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

std::string macText(const uint8_t* m) {
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

// The rates' names, in Rate's order: a mirror of NowTP's kRateNames
// (radio.cpp), whose PhyRate indexes the same table. The nodes offer them
// as their options.
const char* const kRateNames[] = {
    "default (1 Mbps)", "1 Mbps", "2 Mbps", "5.5 Mbps", "11 Mbps", "6 Mbps", "9 Mbps", "12 Mbps",
    "18 Mbps", "24 Mbps", "36 Mbps", "48 Mbps", "54 Mbps", "MCS0 (6.5 Mbps)", "MCS1 (13 Mbps)",
    "MCS2 (19.5 Mbps)", "MCS3 (26 Mbps)", "MCS4 (39 Mbps)", "MCS5 (52 Mbps)", "MCS6 (58.5 Mbps)",
    "MCS7 (65 Mbps)", "LR 250 kbps", "LR 500 kbps",
};

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

NowTp& NowTp::txPower(float dBm) {
    txPower_ = dBm;
    return *this;
}

NowTp& NowTp::broadcastRate(Rate r) {
    broadcastRate_ = r;
    return *this;
}

NowTp& NowTp::unicastRate(Rate r) {
    unicastRate_ = r;
    return *this;
}

NowTp& NowTp::countryCode(const char* code) {
    countryCode_ = code ? code : "";
    return *this;
}

NowTp& NowTp::autoPowerFallback(bool on) {
    autoPowerFallback_ = on;
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
    if (NetworkClock* clock = dev_.networkClock()) return clock->networkTimeUs();
    if (radio_) return radio_->networkTimeUs();
    return dev_.core().uptimeMs() * 1000;
}

bool NowTp::timeSynced() const {
    if (NetworkClock* clock = dev_.networkClock()) return clock->timeSynced();
    return status_.timeSynced;
}

int64_t NowTp::localTimeUs(uint64_t networkUs) const {
    if (NetworkClock* clock = dev_.networkClock()) return clock->localTimeUs(networkUs);
    if (radio_) return radio_->localTimeUs(networkUs);
    return static_cast<int64_t>(networkUs);
}

NowTp& NowTp::pairingCode(const char* code) {
    pairingCode_ = code ? code : "";
    return *this;
}

void NowTp::describeTime(tesser::Object& o) {
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
    longRangeNode_ = &o.value("longRange", [this] { return longRange_; },
                             [this](bool on) -> Check {
                                 // Off with a Long Range rate set would persist a state
                                 // NowTP refuses at boot.
                                 if (!on && (isLongRange(broadcastRate_) || isLongRange(unicastRate_))) {
                                     return Check::fail(Status::InvalidValue, "change the rates away from Long Range first");
                                 }
                                 longRange_ = on;
                                 return Check::ok();
                             })
                          .persist()
                          .label("Long range")
                          .doc("Espressif's Long Range mode, besides 802.11b/g/n: farther, slower")
                          .ui("advanced");
    txPowerNode_ = &o.value("txPower", [this] { return txPower_; },
                            [this](float dbm) -> Check {
                                // The driver's units are 0.25 dBm (NowTP's quarterDbm): a value it
                                // would round is not the one that gets set.
                                if (dbm > 0 && (dbm < 2 || dbm * 4 != static_cast<float>(static_cast<int>(dbm * 4)))) {
                                    return Check::fail(Status::InvalidValue, "the driver's (0), or 2–21 dBm in 0.25 steps");
                                }
                                txPower_ = dbm;
                                return Check::ok();
                            })
                     .range(0, 21)
                     .step(0.25)
                     .unit("dBm")
                     .persist()
                     .label("Transmit power")
                     .doc("How hard the radio sends; lower reaches less far")
                     .ui("advanced");
    broadcastRateNode_ = &o.value("broadcastRate", [this] { return broadcastRate_; },
                                 [this](Rate r) -> Check {
                                     if (isLongRange(r) && !longRange_) {
                                         return Check::fail(Status::InvalidValue, "Long Range rates need long range on");
                                     }
                                     broadcastRate_ = r;
                                     return Check::ok();
                                 },
                                 kRateNames)
                            .persist()
                            .label("Broadcast rate")
                            .doc("The PHY rate of frames sent to all the nodes nearby; Default: the driver's (1 Mbps)")
                            .ui("advanced");
    unicastRateNode_ = &o.value("unicastRate", [this] { return unicastRate_; },
                                [this](Rate r) -> Check {
                                    if (isLongRange(r) && !longRange_) {
                                        return Check::fail(Status::InvalidValue, "Long Range rates need long range on");
                                    }
                                    unicastRate_ = r;
                                    return Check::ok();
                                },
                                kRateNames)
                           .persist()
                           .label("Unicast rate")
                           .doc("The PHY rate of frames sent to each node individually; Default: the driver's (1 Mbps)")
                           .ui("advanced");
    countryCodeNode_ = &o.value("countryCode", countryCode_)
                            .validate([](const std::string& v) {
                                if (v.empty()) return Check::ok();
                                if (v.size() != 2) return Check::fail(Status::InvalidValue, "two letters, or empty");
                                for (char c : v) {
                                    if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')) {
                                        return Check::fail(Status::InvalidValue, "two letters, or empty");
                                    }
                                }
                                return Check::ok();
                            })
                            .persist()
                            .label("Country code")
                            .doc("The two-letter code ('US', 'FR', 'JP', '01' for world-safe) selecting the allowed channels and power limits; empty: the driver's own")
                            .ui("advanced");
    restartSettings_.push_back(countryCodeNode_);
    autoPowerFallbackNode_ = &o.value("autoPowerFallback", autoPowerFallback_)
                                  .persist()
                                  .label("Automatic power fallback")
                                  .doc("Lower the transmit power when a peer doesn't acknowledge, so a weak link works")
                                  .ui("advanced");
    restartSettings_.push_back(autoPowerFallbackNode_);
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
                             e.value("mac", [&n] { return macText(n.mac); }).label("MAC");
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

    // With a NetworkClock, network time is its (/sys/clock): NowTP serves
    // its core instead of running its own.
    if (!dev_.networkClock()) describeTime(o);

    auto& s = o.object("status").label("Status");
    statusNode_ = &s;
    s.value("started", [this] { return status_.started; }).label("Running");
    s.value("channel", [this] { return status_.channel; });
    // The radio's settings as they run: the driver's power (0: its own), the
    // highest it accepts, and the broadcast rate.
    s.value("txPower", [this] { return status_.txPowerDbm; }).unit("dBm").label("Transmit power");
    s.value("txPowerLimit", [this] { return status_.txPowerLimitDbm; })
        .unit("dBm")
        .label("Maximum transmit power")
        .ui("advanced");
    s.value("broadcastRate", [this] { return status_.broadcastRate; }, kRateNames)
        .label("Broadcast rate")
        .ui("advanced");
    // The radio as it runs: what the link supports, and the Wi-Fi state
    // ESP-NOW is bound to.
    s.value("learnedTxCeilingDbm", [this] { return status_.learnedTxCeilingDbm; })
        .unit("dBm")
        .label("Power found to work")
        .doc("The power found to work by the fallback or optimize() (0: none)")
        .ui("advanced");
    s.value("interface", [this] { return status_.interface; })
        .label("Interface")
        .doc("The Wi-Fi interface ESP-NOW is bound to: the station's while it runs, else the access point's")
        .ui("advanced");
    s.value("stationConnected", [this] { return status_.stationConnected; })
        .label("Connected to an access point")
        .ui("advanced");
    s.value("softApActive", [this] { return status_.softApActive; })
        .label("Running a soft-AP")
        .ui("advanced");
    s.value("wifiStartedByNowTP", [this] { return status_.wifiStartedByNowTP; })
        .label("Wi-Fi started by NowTP")
        .doc("NowTP initialized and started Wi-Fi and will stop it")
        .ui("advanced");
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
    txPowerGen_ = txPowerNode_->generation();
    broadcastRateGen_ = broadcastRateNode_->generation();
    unicastRateGen_ = unicastRateNode_->generation();
    if (timeReferenceNode_) {
        timeReferenceGen_ = timeReferenceNode_->generation();
        timeIntervalGen_ = timeIntervalNode_->generation();
    }
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
    c.broadcastRate = broadcastRate_;
    c.unicastRate = unicastRate_;
    c.countryCode = countryCode_;
    c.autoPowerFallback = autoPowerFallback_;
    c.pairingCode = pairingCode_;
    c.maxCalls = maxCalls_;
    c.runTask = ownTask_;
    c.timeReference = timeReference_;
    c.timeIntervalMs = timeInterval_ * 1000;
    if (!ownTask_) c.queued = [&dev = dev_] { dev.wake(); };
    if (NetworkClock* clock = dev_.networkClock()) {
        c.timeCore = clock->core();
        c.timeWake = [&dev = dev_] { dev.wake(); };
    }
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
                   st.txPowerDbm != status_.txPowerDbm || st.txPowerLimitDbm != status_.txPowerLimitDbm ||
                   st.broadcastRate != status_.broadcastRate || st.sent != status_.sent ||
                   st.received != status_.received || st.failed != status_.failed ||
                   st.learnedTxCeilingDbm != status_.learnedTxCeilingDbm || st.stationConnected != status_.stationConnected ||
                   st.softApActive != status_.softApActive || st.wifiStartedByNowTP != status_.wifiStartedByNowTP ||
                   st.interface != status_.interface;
    // The offset, drift and age move all the time: read when asked, announced
    // with a round.
    bool timeChanged = st.timeSynced != status_.timeSynced || st.timeReference != status_.timeReference ||
                       st.timeReferenceMac != status_.timeReferenceMac || st.timeRounds != status_.timeRounds;
    status_ = std::move(st);
    if (changed) statusNode_->changed();
    if (timeChanged && timeNode_) timeNode_->changed();
    // The list in place: no copy of it each second, and the signal, which
    // moves most seconds, notifies only past kRssiStepDb.
    for (Node& n : nodes_) n.seen = false;
    bool listChanged = false;
    if (started_) {
        radio_->peers([this, &listChanged](const NowTpPeer& p) {
            Node* n = nullptr;
            for (Node& m : nodes_) {
                if (memcmp(m.mac, p.mac, sizeof(m.mac)) == 0) n = &m;
            }
            if (!n) {
                if (nodes_.size() >= kMaxNodes) return;
                nodes_.emplace_back();
                n = &nodes_.back();
                memcpy(n->mac, p.mac, sizeof(n->mac));
                n->rssiShown = p.rssi;
                listChanged = true;
            }
            n->seen = true;
            if (n->name != p.name || n->channel != p.channel || n->otherChannel != p.otherChannel ||
                n->tesser != p.tesser || n->authenticated != p.authenticated) {
                n->name = p.name;
                n->channel = p.channel;
                n->otherChannel = p.otherChannel;
                n->tesser = p.tesser;
                n->authenticated = p.authenticated;
                listChanged = true;
            }
            n->rssi = p.rssi;
            int32_t moved = n->rssi - n->rssiShown;
            if (moved >= kRssiStepDb || -moved >= kRssiStepDb) listChanged = true;
        });
    }
    size_t before = nodes_.size();
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [](const Node& n) { return !n.seen; }), nodes_.end());
    if (nodes_.size() != before) listChanged = true;
    if (listChanged) {
        for (Node& n : nodes_) n.rssiShown = n.rssi;
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
    if (!started_ || !timeReferenceNode_) return;
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
    // The power and the rates run live: NowTP applies them to the driver.
    if ((gen = txPowerNode_->generation()) != txPowerGen_) {
        txPowerGen_ = gen;
        if (started_) radio_->txPower(txPower_);
    }
    if ((gen = broadcastRateNode_->generation()) != broadcastRateGen_) {
        broadcastRateGen_ = gen;
        if (started_) radio_->broadcastRate(broadcastRate_);
    }
    if ((gen = unicastRateNode_->generation()) != unicastRateGen_) {
        unicastRateGen_ = gen;
        if (started_) radio_->unicastRate(unicastRate_);
    }
    if (timeReferenceNode_ && (gen = timeReferenceNode_->generation()) != timeReferenceGen_) {
        timeReferenceGen_ = gen;
        applyTimeReference();
    }
    if (timeIntervalNode_ && (gen = timeIntervalNode_->generation()) != timeIntervalGen_) {
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
