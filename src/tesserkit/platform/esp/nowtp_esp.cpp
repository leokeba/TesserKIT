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
        if (!transport_->begin()) return false;
        if (peers && !mounted_) {
            transport_->mountPeers(*peers);
            mounted_ = true;
        }
        transport_->advertise();
        return true;
    }

    tesser::DatagramEndpoint* endpoint() override { return transport_ ? &transport_->endpoint() : nullptr; }

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
    bool mounted_ = false;
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
