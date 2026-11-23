// ESP-NOW through NowTP, and TesserAPI's NowTpTransport serving the API to
// the nodes nearby. Built when the build has NowTP (TESSER_HAVE_NOWTP, set by
// TesserAPI's component, or <NowTP.h> on Arduino); espNowTp() is null
// otherwise.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"
#include "tesser/transports/nowtp_transport.h"

#if defined(TESSER_HAVE_NOWTP)

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <functional>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "tesser/platform.h"

namespace tesserkit {

namespace {

std::string macString(const nowtp::Mac& m) {
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m.bytes[0], m.bytes[1], m.bytes[2], m.bytes[3], m.bytes[4],
             m.bytes[5]);
    return b;
}

bool parseMac(const std::string& s, nowtp::Mac& m) {
    unsigned b[6];
    char tail;
    if (sscanf(s.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) m.bytes[i] = static_cast<uint8_t>(b[i]);
    return true;
}

// The diagnostics' NowTP types, as plain structs the service can serialize.
LinkDiagnosis toTesserkit(const nowtp::LinkDiagnosis& d) {
    LinkDiagnosis out;
    for (int i = 0; i < 6; i++) out.peer[i] = d.peer.bytes[i];
    out.responds = d.responds;
    out.requestPowerDbm = d.requestPowerDbm;
    out.replyPowerDbm = d.replyPowerDbm;
    out.ours.limitDbm = d.ours.limitDbm;
    out.ours.dsssDbm = d.ours.dsssDbm;
    out.ours.ofdmDbm = d.ours.ofdmDbm;
    out.ours.powerResponse = d.ours.powerResponse;
    out.theirs.limitDbm = d.theirs.limitDbm;
    out.theirs.dsssDbm = d.theirs.dsssDbm;
    out.theirs.ofdmDbm = d.theirs.ofdmDbm;
    out.theirs.powerResponse = d.theirs.powerResponse;
    for (const nowtp::ProbeResult& r : d.forward) {
        ProbeResult p;
        p.rate = static_cast<Rate>(r.step.rate);
        p.txPowerDbm = r.step.txPowerDbm;
        p.sent = r.sent;
        p.received = r.received;
        p.rssi = r.rssi;
        out.forward.push_back(p);
    }
    for (const nowtp::ProbeResult& r : d.reverse) {
        ProbeResult p;
        p.rate = static_cast<Rate>(r.step.rate);
        p.txPowerDbm = r.step.txPowerDbm;
        p.sent = r.sent;
        p.received = r.received;
        p.rssi = r.rssi;
        out.reverse.push_back(p);
    }
    out.forwardRssi = d.forwardRssi;
    out.reverseRssi = d.reverseRssi;
    for (const nowtp::Issue& i : d.issues) {
        LinkIssue issue;
        issue.kind = static_cast<LinkIssue::Kind>(static_cast<uint8_t>(i.kind));
        issue.local = i.local;
        issue.value = i.value;
        issue.rate = static_cast<Rate>(i.rate);
        issue.detail = i.detail;
        out.issues.push_back(issue);
    }
    return out;
}

LinkProfile toTesserkit(const nowtp::LinkProfile& p) {
    LinkProfile out;
    for (int i = 0; i < 6; i++) out.peer[i] = p.peer.bytes[i];
    out.rateToPeer = static_cast<Rate>(p.rateToPeer);
    out.rateFromPeer = static_cast<Rate>(p.rateFromPeer);
    out.txPowerDbm = p.txPowerDbm;
    out.peerTxPowerDbm = p.peerTxPowerDbm;
    out.deliveryToPeer = p.deliveryToPeer;
    out.deliveryFromPeer = p.deliveryFromPeer;
    out.applied = p.applied;
    out.verified = p.verified;
    return out;
}

// NowTP's diagnostics' failures, as the API's statuses: the radio off (it
// stopped while the diagnostic ran), the peer busy with its own measurements
// (retry later), or no answer.
tesser::Status toTesserkit(nowtp::Status s) {
    switch (s) {
        case nowtp::Status::Ok: return tesser::Status::Ok;
        case nowtp::Status::InvalidState: return tesser::Status::NotAllowed;
        case nowtp::Status::QueueFull: return tesser::Status::Busy;
        case nowtp::Status::Timeout: return tesser::Status::Timeout;
        case nowtp::Status::AuthFailed: return tesser::Status::Timeout;  // another key: no answer
        default: return tesser::Status::Internal;
    }
}

class EspNowTp : public NowTpRadio {
public:
    bool begin(tesser::Api& api, tesser::Object* peers, const NowTpConfig& config, EventSink sink) override {
        if (radio_.started()) return true;
        sink_ = std::move(sink);
        nowtp::EspNowConfig c;
        c.radio.initWifi = true;  // only if nobody started Wi-Fi (Network does)
        c.radio.channel = config.channel;
        c.radio.longRange = config.longRange;
        c.radio.txPowerDbm = config.txPower;
        // The same table, in the same order: Rate indexes NowTP's kRateNames.
        c.radio.broadcastRate = static_cast<nowtp::PhyRate>(config.broadcastRate);
        c.radio.unicastRate = static_cast<nowtp::PhyRate>(config.unicastRate);
        c.radio.countryCode[0] = config.countryCode.empty() ? 0 : config.countryCode[0];
        c.radio.countryCode[1] = config.countryCode.size() > 1 ? config.countryCode[1] : 0;
        c.radio.countryCode[2] = 0;
        c.radio.autoPowerFallback = config.autoPowerFallback;
        // Network applies /sys/network/wifi/powerSave, whose default is none
        // with NowTp (NowTp::needsStationAwake()): NowTP leaves it.
        c.radio.powerSave = nowtp::PowerSave::Leave;
        c.radio.disablePowerSave = false;
        c.enableDiscovery = true;
        c.discovery.name = config.name.substr(0, nowtp::Discovery::kMaxName);
        c.security.pairingCode = config.pairingCode;
        // Received frames waiting for NowTP's task: 4 of ESP-NOW v2's
        // largest. NowTP's default (8, about 12 KB) cost a classic ESP32
        // gateway 4 KB more heap than before NowTP 764a3d2, which a device
        // with every service can't spare (DESIGN.md section 5.11).
        c.eventBufferSize = 6 * 1024;
        c.timeSync.role = role(config.timeReference);
        c.timeSync.intervalMs = config.timeIntervalMs;
        // Without worker tasks, NowTP's own goes too (about 4 KB of stack):
        // its radio events and timers run in poll(), in the device's task,
        // which its wake hook wakes when there is new work. The device
        // polls every 20 ms, within the 250 ms NowTP may wait.
        polled_ = !config.runTask;
        c.runTask = config.runTask;
        if (polled_) {
            queued_ = config.queued;
            c.wake = [](void* self) {
                auto* radio = static_cast<EspNowTp*>(self);
                if (radio->queued_) radio->queued_();
            };
            c.wakeContext = this;
        }
        // The transport outlives a stop: nodes mounted under `peers` keep
        // pointing at its endpoint.
        if (!transport_) {
            tesser::DatagramEndpoint::Options o;
            o.maxCalls = config.maxCalls;
            transport_.reset(new tesser::NowTpTransport(&api, radio_, tesser::NowTpTransport::kDefaultPort, o));
            // Nodes holding the installation key: with a key, NowTP drops
            // every message not authenticated with it.
            transport_->endpoint().trust([this](const tesser::PeerAddress&) { return radio_.secured(); });
        }
        radio_.onPeerEvent([this](nowtp::PeerEvent e, const nowtp::PeerInfo& p) {
            transport_->peerEvent(e, p);
            if (e != nowtp::PeerEvent::Updated) post(Event::Type::NowTpPeers);
        });
        radio_.onChannelChange([this](uint8_t ch) { post(Event::Type::NowTpChannel, ch); });
        // A network clock's core (NetworkClock): ESP-NOW becomes one of its
        // carriers, once, before NowTP first starts; NowTP's own TimeSync
        // and the time settings are left out then.
        if (config.timeCore && !served_) {
            nowtp::Status served = radio_.serveTimeSync(config.timeCore, config.timeWake);
            if (served == nowtp::Status::Ok) {
                served_ = true;
            } else {
                tesser::logError("tesserkit: NowTP can't serve the network clock: %s", nowtp::toString(served));
            }
        }
        nowtp::Status st = radio_.begin(c);
        if (st != nowtp::Status::Ok) {
            tesser::logError("tesserkit: NowTP: %s", nowtp::toString(st));
            return false;
        }
        if (!transport_->begin(config.runTask)) return false;
        if (polled_) transport_->endpoint().onQueued(config.queued);
        if (peers && !mounted_) {
            transport_->mountPeers(*peers);
            mounted_ = true;
        }
        transport_->advertise();
        nowtp::Mac mac;
        if (!served_ && parseMac(config.timeReference, mac)) radio_.setTimeReference(mac);
        return true;
    }

    tesser::DatagramEndpoint* endpoint() override { return transport_ ? &transport_->endpoint() : nullptr; }
    void poll() override {
        if (!polled_) return;
        if (radio_.started()) radio_.poll();  // first: what it received reaches the transport's queue
        if (transport_) transport_->poll();
    }

    uint64_t networkTimeUs() override { return radio_.networkTimeUs(); }
    int64_t localTimeUs(uint64_t networkUs) override { return radio_.localTimeUs(networkUs); }

    // "auto": a candidate, which NowTP's election may make the reference;
    // "this": the reference; a MAC: a follower of that node, which never
    // takes over.
    static nowtp::TimeRole role(const std::string& ref) {
        if (ref == "auto") return nowtp::TimeRole::Candidate;
        if (ref == "this") return nowtp::TimeRole::Reference;
        return nowtp::TimeRole::Follow;
    }

    bool timeReference(const std::string& ref) override {
        nowtp::Mac mac;
        bool follow = parseMac(ref, mac);
        if (radio_.setTimeRole(role(ref)) != nowtp::Status::Ok) return false;
        nowtp::Status st = follow ? radio_.setTimeReference(mac) : radio_.clearTimeReference();
        return st == nowtp::Status::Ok;
    }

    bool timeInterval(uint32_t ms) override { return radio_.setTimeSyncInterval(ms) == nowtp::Status::Ok; }

    void end() override {
        if (transport_) transport_->end();
        radio_.end();
    }

    void peers(const std::function<void(const NowTpPeer&)>& each) override {
        NowTpPeer n;  // one, refilled: its name keeps its capacity
        for (const nowtp::PeerInfo& p : radio_.peers()) {
            n.name = p.name;
            memcpy(n.mac, p.mac.bytes, sizeof(n.mac));
            n.rssi = p.rssi;
            n.channel = p.channel;
            n.otherChannel = p.otherChannel;
            uint8_t port;
            uint32_t hash;
            n.tesser = tesser::NowTpTransport::parseAdvertisement(p.metadata, port, hash);
            n.authenticated = p.authenticated;
            each(n);
        }
    }

    NowTpStatus status() override {
        NowTpStatus s;
        s.started = radio_.started();
        if (!s.started) return s;
        s.channel = radio_.channel();
        nowtp::RadioInfo ri = radio_.radioInfo();
        s.txPowerDbm = ri.txPowerDbm;
        s.txPowerLimitDbm = ri.txPowerLimitDbm;
        s.learnedTxCeilingDbm = ri.learnedTxCeilingDbm;
        s.stationConnected = ri.stationConnected;
        s.softApActive = ri.softApActive;
        s.wifiStartedByNowTP = ri.wifiStartedByNowTP;
        s.interface = ri.interface == WIFI_IF_STA ? "station" : "soft-AP";
        s.broadcastRate = static_cast<Rate>(radio_.broadcastRate());
        s.secured = radio_.secured();
        nowtp::TimeSyncStatus t = radio_.timeSyncStatus();
        s.timeSynced = t.synced;
        s.timeReference = t.isReference;
        s.timeOffsetUs = t.offsetUs;
        // The reference is this node's MAC while it acts as one.
        if (t.isReference || t.synced || t.rounds) s.timeReferenceMac = macString(t.reference);
        s.timeRoundTripUs = t.roundTripUs;
        s.timeDriftPpm = t.driftPpm;
        s.timeRounds = t.rounds;
        s.timeAgeMs = t.ageMs;
        nowtp::EspNowTransport::TransportStats st = radio_.stats();
        s.sent = st.messagesSent;
        s.received = st.messagesReceived;
        s.failed = st.messagesFailed;
        return s;
    }

    bool channel(uint8_t ch) override { return radio_.moveToChannel(ch) == nowtp::Status::Ok; }
    bool announceChannelChange(uint8_t ch) override {
        return radio_.announceChannelChange(ch) == nowtp::Status::Ok;
    }
    bool longRange(bool on) override { return radio_.setLongRange(on) == nowtp::Status::Ok; }
    bool txPower(float dbm) override { return radio_.setTxPower(dbm) == nowtp::Status::Ok; }
    bool broadcastRate(Rate r) override {
        return radio_.setBroadcastRate(static_cast<nowtp::PhyRate>(r)) == nowtp::Status::Ok;
    }
    bool unicastRate(Rate r) override {
        return radio_.setUnicastRate(static_cast<nowtp::PhyRate>(r)) == nowtp::Status::Ok;
    }
    bool pair(uint32_t timeoutMs) override { return spawnPairing(false, timeoutMs); }
    bool acceptPairing(uint32_t timeoutMs) override { return spawnPairing(true, timeoutMs); }
    bool pairing() override { return pairing_.load(); }
    bool diagnosing() override { return diagnosing_.load(); }

    bool diagnose(const std::array<uint8_t, 6>& mac,
                  const std::function<void(tesser::Status, const LinkDiagnosis&)>& done) override {
        return spawnDiagnostic(mac, done, nullptr);
    }
    bool optimize(const std::array<uint8_t, 6>& mac,
                  const std::function<void(tesser::Status, const LinkProfile&)>& done) override {
        return spawnDiagnostic(mac, nullptr, done);
    }

    bool forgetKey() override {
        if (pairing_.load()) return false;  // the pairing task uses the key
        if (radio_.started()) return radio_.forgetSecurityKey() == nowtp::Status::Ok;
        // Stopped: NowTP keeps the key in its own NVS namespace.
        nvs_handle_t h;
        if (nvs_open("nowtp", NVS_READWRITE, &h) != ESP_OK) return true;  // nothing stored
        bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
        return ok;
    }

private:
    struct Pairing {
        EspNowTp* self;
        bool accept;
        uint32_t timeoutMs;
    };

    void post(Event::Type type, int32_t value = 0, std::string text = std::string()) {
        Event e;
        e.type = type;
        e.value = value;
        e.text = std::move(text);
        if (sink_) sink_(std::move(e));
    }

    // Pairing blocks for up to its timeout: in a task of its own, one at a
    // time.
    bool spawnPairing(bool accept, uint32_t timeoutMs) {
        if (pairing_.exchange(true)) return false;
        auto* p = new Pairing{this, accept, timeoutMs};
        if (xTaskCreate(pairingTask, "tk_pair", 6144, p, 4, nullptr) == pdPASS) return true;
        delete p;
        pairing_.store(false);
        return false;
    }
    static void pairingTask(void* arg) {
        std::unique_ptr<Pairing> p(static_cast<Pairing*>(arg));
        nowtp::Mac other;
        nowtp::Status st = p->accept ? p->self->radio_.acceptPairing(p->timeoutMs, &other)
                                     : p->self->radio_.pair(p->timeoutMs, &other);
        if (st == nowtp::Status::Ok) {
            p->self->post(Event::Type::NowTpPairing, 0, macString(other));
        } else {
            p->self->post(Event::Type::NowTpPairing, 1, nowtp::toString(st));
        }
        // Last: the radio, and the sink begin() sets, are no longer used.
        p->self->pairing_.store(false);
        p.reset();
        vTaskDelete(nullptr);
    }

    // A diagnosis or an optimization blocks for seconds: in a task of its own,
    // one at a time, like a pairing.
    struct Diagnostic {
        EspNowTp* self;
        nowtp::Mac mac;
        std::function<void(tesser::Status, const LinkDiagnosis&)> diagnosed;
        std::function<void(tesser::Status, const LinkProfile&)> optimized;
    };

    bool spawnDiagnostic(const std::array<uint8_t, 6>& mac,
                         const std::function<void(tesser::Status, const LinkDiagnosis&)>& done,
                         const std::function<void(tesser::Status, const LinkProfile&)>& optimized) {
        if (!radio_.started()) return false;
        if (diagnosing_.exchange(true)) return false;
        auto* d = new Diagnostic{this, nowtp::Mac(), done, optimized};
        for (int i = 0; i < 6; i++) d->mac.bytes[i] = mac[i];
        // Its rate tables (up to a dozen probes each way), the issues' texts
        // and NowTP's procedures take a few KB of stack.
        if (xTaskCreate(diagnosticTask, "tk_diag", 8192, d, 4, nullptr) == pdPASS) return true;
        delete d;
        diagnosing_.store(false);
        return false;
    }
    static void diagnosticTask(void* arg) {
        std::unique_ptr<Diagnostic> d(static_cast<Diagnostic*>(arg));
        if (d->optimized) {
            nowtp::LinkProfile p;
            nowtp::Status st = d->self->radio_.optimizeLink(d->mac, p);
            if (st == nowtp::Status::Ok) d->optimized(tesser::Status::Ok, toTesserkit(p));
            else d->optimized(toTesserkit(st), LinkProfile());
        } else {
            nowtp::LinkDiagnosis diag;
            nowtp::Status st = d->self->radio_.diagnose(d->mac, diag);
            // A peer that never answers still fills the diagnosis: that is
            // the result (an issue plus responds false), not a failure.
            if (st == nowtp::Status::Timeout) st = nowtp::Status::Ok;
            if (st == nowtp::Status::Ok) d->diagnosed(tesser::Status::Ok, toTesserkit(diag));
            else d->diagnosed(toTesserkit(st), LinkDiagnosis());
        }
        // Last: the radio, and the sink begin() sets, are no longer used.
        d->self->diagnosing_.store(false);
        d.reset();
        vTaskDelete(nullptr);
    }

    nowtp::EspNowTransport radio_;
    std::unique_ptr<tesser::NowTpTransport> transport_;
    bool polled_ = false;  // no worker tasks, NowTP's included: poll() does their work
    std::function<void()> queued_;  // wakes the polling task (NowTP's wake hook)
    bool mounted_ = false;
    bool served_ = false;  // serves a NetworkClock's TimeSync
    EventSink sink_;
    std::atomic<bool> pairing_{false};  // a pairing task runs
    std::atomic<bool> diagnosing_{false};  // a diagnostic task runs
};

}  // namespace

NowTpRadio* espNowTp() {
    static EspNowTp nowtp;
    return &nowtp;
}

}  // namespace tesserkit

#else

namespace tesserkit {
NowTpRadio* espNowTp() { return nullptr; }
}  // namespace tesserkit

#endif
#endif
