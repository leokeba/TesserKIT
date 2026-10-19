// ESP-NOW through NowTP, and TesserAPI's NowTpTransport serving the API to
// the nodes nearby. Built when the build has NowTP (TESSER_HAVE_NOWTP, set by
// TesserAPI's component, or <NowTP.h> on Arduino); espNowTp() is null
// otherwise.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"
#include "tesser/transports/nowtp_transport.h"

#if defined(TESSER_HAVE_NOWTP)

#include <stdio.h>

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
        c.enableDiscovery = true;
        c.discovery.name = config.name.substr(0, nowtp::Discovery::kMaxName);
        c.security.pairingCode = config.pairingCode;
        // Received frames waiting for NowTP's task: 4 of ESP-NOW v2's
        // largest. NowTP's default (8, about 12 KB) cost a classic ESP32
        // gateway 4 KB more heap than before NowTP 764a3d2, which a device
        // with every service can't spare (DESIGN.md section 5.11).
        c.eventBufferSize = 6 * 1024;
        // Network time: "this" offers this node's clock; NowTP picks the
        // reference announced in discovery otherwise, or the node set below.
        c.timeSync.reference = config.timeReference == "this";
        c.timeSync.intervalMs = config.timeIntervalMs;
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
        nowtp::Status st = radio_.begin(c);
        if (st != nowtp::Status::Ok) {
            tesser::logError("tesserkit: NowTP: %s", nowtp::toString(st));
            return false;
        }
        if (!transport_->begin(config.runTask)) return false;
        polled_ = !config.runTask;
        if (polled_) transport_->endpoint().onQueued(config.queued);
        if (peers && !mounted_) {
            transport_->mountPeers(*peers);
            mounted_ = true;
        }
        transport_->advertise();
        timeReference_ = "auto";
        if (config.timeReference == "this") {
            timeReference_ = "this";
        } else if (config.timeReference != "auto") {
            timeReference(config.timeReference);
        }
        return true;
    }

    tesser::DatagramEndpoint* endpoint() override { return transport_ ? &transport_->endpoint() : nullptr; }
    void poll() override {
        if (polled_ && transport_) transport_->poll();
    }

    uint64_t networkTimeUs() override { return radio_.networkTimeUs(); }
    int64_t localTimeUs(uint64_t networkUs) override { return radio_.localTimeUs(networkUs); }

    bool timeReference(const std::string& ref) override {
        if (ref == timeReference_) return true;
        nowtp::Mac mac;
        // Following another node is set at once; being the reference, or
        // back to the announced one, is fixed when the radio starts.
        if (timeReference_ == "this" || !parseMac(ref, mac)) return false;
        if (radio_.setTimeReference(mac) != nowtp::Status::Ok) return false;
        timeReference_ = ref;
        return true;
    }

    void end() override {
        if (transport_) transport_->end();
        radio_.end();
    }

    std::vector<NowTpPeer> peers() override {
        std::vector<NowTpPeer> out;
        for (const nowtp::PeerInfo& p : radio_.peers()) {
            NowTpPeer n;
            n.name = p.name;
            n.mac = macString(p.mac);
            n.rssi = p.rssi;
            n.channel = p.channel;
            uint8_t port;
            uint32_t hash;
            n.tesser = tesser::NowTpTransport::parseAdvertisement(p.metadata, port, hash);
            n.authenticated = p.authenticated;
            out.push_back(n);
        }
        return out;
    }

    NowTpStatus status() override {
        NowTpStatus s;
        s.started = radio_.started();
        if (!s.started) return s;
        s.channel = radio_.channel();
        s.secured = radio_.secured();
        nowtp::TimeSyncStatus t = radio_.timeSyncStatus();
        s.timeSynced = t.synced;
        s.timeReference = t.isReference;
        s.timeOffsetUs = t.offsetUs;
        if (t.isReference) {
            s.timeReferenceMac = macString(radio_.localMac());
        } else if (t.synced || t.rounds) {
            s.timeReferenceMac = macString(t.reference);
        }
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
    bool longRange(bool on) override { return radio_.setLongRange(on) == nowtp::Status::Ok; }
    bool pair(uint32_t timeoutMs) override { return spawnPairing(false, timeoutMs); }
    bool acceptPairing(uint32_t timeoutMs) override { return spawnPairing(true, timeoutMs); }

    bool forgetKey() override {
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

    // Pairing blocks for up to its timeout: in a task of its own.
    bool spawnPairing(bool accept, uint32_t timeoutMs) {
        auto* p = new Pairing{this, accept, timeoutMs};
        if (xTaskCreate(pairingTask, "tk_pair", 6144, p, 4, nullptr) == pdPASS) return true;
        delete p;
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
        p.reset();
        vTaskDelete(nullptr);
    }

    nowtp::EspNowTransport radio_;
    std::unique_ptr<tesser::NowTpTransport> transport_;
    bool polled_ = false;  // no worker task: poll() handles the messages
    bool mounted_ = false;
    std::string timeReference_ = "auto";  // what NowTP runs: "auto", "this" or a MAC
    EventSink sink_;
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
