#include "tesserkit/network_clock.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "tesser/api.h"
#include "tesser/platform.h"
#include "tesser/udp_link.h"
#include "tesserkit/udp_api.h"

#if defined(ESP_PLATFORM)
#include "tesser/transports/nowtp_transport.h"  // TESSER_HAVE_NOWTP on Arduino
#endif
#if defined(TESSER_HAVE_NOWTP) || defined(TESSERKIT_HAVE_NOWTP)
#define TESSERKIT_NETWORK_CLOCK 1
#include "nowtp/timesync.h"
#endif

namespace tesserkit {

using tesser::Check;
using tesser::Status;

namespace {

constexpr uint32_t kRefreshMs = 1000;
// A timed write is applied when the alarm finds it this close: esp_timer's
// task wakes the alarm's a little late anyway.
constexpr uint64_t kEarlyUs = 100;

bool hexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
uint8_t hexValue(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return static_cast<uint8_t>(c - 'A' + 10);
}

// "aa:bb:cc:dd:ee:ff" or "aabbccddeeff", either case.
bool parseMac(const std::string& v, uint8_t* out) {
    size_t step = v.size() == 17 ? 3 : v.size() == 12 ? 2 : 0;
    if (!step) return false;
    for (size_t i = 0; i < 6; i++) {
        size_t at = i * step;
        if (!hexDigit(v[at]) || !hexDigit(v[at + 1])) return false;
        if (step == 3 && i < 5 && v[at + 2] != ':') return false;
        out[i] = static_cast<uint8_t>(hexValue(v[at]) << 4 | hexValue(v[at + 1]));
    }
    return true;
}

Check checkReference(const std::string& v) {
    uint8_t mac[6];
    if (v == "auto" || v == "this" || parseMac(v, mac)) return Check::ok();
    return Check::fail(Status::InvalidValue, "auto, this, or a device's MAC (aa:bb:cc:dd:ee:ff)");
}

// "udp, nowtp": names, comma or space separated.
std::vector<std::string> carrierNames(const std::string& v) {
    std::vector<std::string> out;
    std::string word;
    for (size_t i = 0; i <= v.size(); i++) {
        char c = i < v.size() ? v[i] : ',';
        if (c == ',' || c == ' ') {
            if (!word.empty()) out.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    return out;
}

Check checkCarriers(const std::string& v) {
    std::vector<std::string> names = carrierNames(v);
    if (names.empty()) return Check::fail(Status::InvalidValue, "at least one of udp, nowtp");
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] != "udp" && names[i] != "nowtp") return Check::fail(Status::InvalidValue, "udp or nowtp");
        for (size_t j = 0; j < i; j++) {
            if (names[j] == names[i]) return Check::fail(Status::InvalidValue, "a carrier named twice");
        }
    }
    return Check::ok();
}

std::string macText(const uint8_t* m) {
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

#if defined(TESSERKIT_NETWORK_CLOCK)
// NowTP's TimeSync over UDP: TesserAPI's side channel (its DESIGN.md section
// 10.7), peers named by their device id (station MAC) both ways. What
// arrives (in UDP's receive task) and when each datagram left wait here for
// the core's tick(), in the device's task.
class UdpCarrier : public nowtp::TimeCarrier {
public:
    static constexpr size_t kInbox = 16;

    explicit UdpCarrier(UdpApi& udp) : udp_(udp) {}

    const char* name() const override { return "udp"; }

    nowtp::Status send(const nowtp::Mac& device, const uint8_t* data, size_t len, uint32_t* token) override {
        tesser::UdpLink* link = udp_.link();
        if (!link || !udp_.running()) return nowtp::Status::InvalidState;
        tesser::PeerAddress to;  // empty: every local network
        if (!device.isBroadcast() && !link->addressOf(tesser::PeerAddress::fromBytes(device.bytes, 6), to)) {
            return nowtp::Status::LinkError;
        }
        uint64_t sentUs = 0;
        if (!link->sendSide(to, data, len, &sentUs)) return nowtp::Status::LinkError;
        uint32_t t = ++token_;
        if (token) *token = t;
        tesser::MutexGuard g(mutex_);
        if (sentUs && sent_.size() < kInbox) sent_.push_back(Sent{t, sentUs});
        count_.store(sent_.size() + inbox_.size());
        return nowtp::Status::Ok;
    }

    bool reaches(const nowtp::Mac& device) const override {
        tesser::UdpLink* link = udp_.link();
        return link && udp_.running() && link->endpoint().deviceOnline(tesser::PeerAddress::fromBytes(device.bytes, 6));
    }

    void poll(nowtp::TimeSync& core) override {
        std::vector<Sent> sent;
        std::vector<Datagram> inbox;
        {
            tesser::MutexGuard g(mutex_);
            sent.swap(sent_);
            inbox.swap(inbox_);
            count_.store(0);
        }
        // Outside the lock: the core answers from here, through send().
        for (const Sent& s : sent) core.sent(*this, s.token, s.us);
        for (const Datagram& d : inbox) core.received(*this, nowtp::Mac::from(d.device), d.data, d.len, d.rxUs);
    }

    bool pending() const override { return count_.load() > 0; }

    // UDP's receive task: quick.
    bool receive(const tesser::PeerAddress& device, const uint8_t* data, size_t len, uint64_t rxUs) {
        if (device.length != 6 || len > sizeof(Datagram::data)) return false;
        tesser::MutexGuard g(mutex_);
        if (inbox_.size() >= kInbox) return false;
        Datagram d;
        memcpy(d.device, device.bytes, 6);
        memcpy(d.data, data, len);
        d.len = len;
        d.rxUs = rxUs;
        inbox_.push_back(d);
        count_.store(sent_.size() + inbox_.size());
        return true;
    }

private:
    struct Sent {
        uint32_t token;
        uint64_t us;
    };
    struct Datagram {
        uint8_t device[6];
        uint8_t data[48];
        size_t len;
        uint64_t rxUs;
    };

    UdpApi& udp_;
    tesser::Mutex mutex_{tesser::kRankLeaf, "clock udp"};
    std::vector<Sent> sent_;
    std::vector<Datagram> inbox_;
    std::atomic<size_t> count_{0};
    uint32_t token_ = 0;
};
#endif

}  // namespace

struct NetworkClock::Impl {
    struct Entry {
        uint64_t localUs;
        size_t bytes;
        tesser::TimedWrite write;
    };

    Device& dev;
    Alarm* alarm = nullptr;
    std::atomic<uint32_t> lateUs{0};
#if defined(TESSERKIT_NETWORK_CLOCK)
    std::unique_ptr<nowtp::TimeSync> core;
    std::unique_ptr<UdpCarrier> udp;
#endif

    tesser::Mutex mutex{tesser::kRankLeaf, "clock queue"};
    std::vector<Entry> queue;  // by local time, then arrival
    size_t bytes = 0;
    Writes stats;

    explicit Impl(Device& d) : dev(d) {}

    uint64_t micros() const { return alarm ? alarm->nowUs() : dev.core().uptimeMs() * 1000; }

    bool synced() const {
#if defined(TESSERKIT_NETWORK_CLOCK)
        return core && core->synced();
#else
        return false;
#endif
    }

    uint64_t toLocal(uint64_t networkUs) const {
#if defined(TESSERKIT_NETWORK_CLOCK)
        if (core) return core->toLocal(networkUs);
#endif
        return networkUs;
    }

    uint64_t toNetwork(uint64_t localUs) const {
#if defined(TESSERKIT_NETWORK_CLOCK)
        if (core) return core->toNetwork(localUs);
#endif
        return localUs;
    }

    // TesserAPI's scheduler: under the API lock, in whichever task the
    // write came by.
    Status schedule(tesser::TimedWrite&& w) {
        if (!synced()) return Status::NotAllowed;
        uint64_t local = toLocal(w.at);
        uint64_t now = micros();
        if (now > local + lateUs.load()) {
            tesser::MutexGuard g(mutex);
            stats.late++;
            return Status::Timeout;  // reason "late"
        }
        size_t size = w.path.size() + w.body.size();
        tesser::MutexGuard g(mutex);
        if (queue.size() >= kMaxQueued || bytes + size > kMaxQueuedBytes) {
            stats.busy++;
            return Status::Busy;
        }
        auto at = std::upper_bound(queue.begin(), queue.end(), local,
                                   [](uint64_t t, const Entry& e) { return t < e.localUs; });
        bool first = at == queue.begin();
        queue.insert(at, Entry{local, size, std::move(w)});
        bytes += size;
        stats.queued = static_cast<uint32_t>(queue.size());
        // Under the queue's lock, so a fire() that is about to set a later
        // time can't overwrite this one.
        if (first && alarm) alarm->at(local);
        return Status::Ok;
    }

    // Applies the writes whose time has come, in the alarm's task (or the
    // device's, without one), and sets the alarm for the next.
    void fire() {
        for (;;) {
            Entry e{0, 0, tesser::TimedWrite()};
            {
                tesser::MutexGuard g(mutex);
                if (queue.empty()) return;
                if (queue.front().localUs > micros() + kEarlyUs) {
                    if (alarm) alarm->at(queue.front().localUs);
                    return;
                }
                e = std::move(queue.front());
                queue.erase(queue.begin());
                bytes -= e.bytes;
                stats.queued = static_cast<uint32_t>(queue.size());
            }
            uint64_t now = micros();
            uint64_t lateness = now > e.localUs ? now - e.localUs : 0;
            Status s = dev.api().applyTimed(e.write);  // takes the API lock
            tesser::MutexGuard g(mutex);
            if (s == Status::Ok) {
                stats.applied++;
            } else {
                stats.failed++;
            }
            uint32_t l = lateness > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(lateness);
            if (l > stats.lateMaxUs) stats.lateMaxUs = l;
        }
    }
};

NetworkClock::NetworkClock(Device& dev, UdpApi* udp) : Service(dev, "clock"), udp_(udp) {}

NetworkClock::~NetworkClock() {
    if (impl_) dev_.api().setScheduler(nullptr);
}

NetworkClock& NetworkClock::reference(const char* ref) {
    reference_ = ref ? ref : "auto";
    return *this;
}

NetworkClock& NetworkClock::carriers(const char* order) {
    carriers_ = order ? order : "udp, nowtp";
    return *this;
}

NetworkClock& NetworkClock::interval(uint32_t seconds) {
    interval_ = seconds;
    return *this;
}

NetworkClock& NetworkClock::late(uint32_t ms) {
    late_ = ms < kMaxLateMs ? ms : kMaxLateMs;
    return *this;
}

uint64_t NetworkClock::networkTimeUs() const {
    if (!impl_) return dev_.core().uptimeMs() * 1000;
    return impl_->toNetwork(impl_->micros());
}

int64_t NetworkClock::localTimeUs(uint64_t networkUs) const {
    if (!impl_) return static_cast<int64_t>(networkUs);
    return static_cast<int64_t>(impl_->toLocal(networkUs));
}

bool NetworkClock::timeSynced() const { return impl_ && impl_->synced(); }

NetworkClock::Writes NetworkClock::writes() const {
    if (!impl_) return Writes();
    tesser::MutexGuard g(impl_->mutex);
    return impl_->stats;
}

nowtp::TimeSync* NetworkClock::core() {
#if defined(TESSERKIT_NETWORK_CLOCK)
    return impl_ ? impl_->core.get() : nullptr;
#else
    return nullptr;
#endif
}

void NetworkClock::describe(tesser::Object& o) {
    o.label("Network time")
        .ui("group", "Nodes")
        .ui("icon", "clock")
        .doc("A clock shared by the nodes over UDP and NowTP, the reference node's: writes and timelines "
             "played at the same instant on each")
        .writeAccess(tesser::Access::Admin);
    referenceNode_ = &o.value("reference", reference_)
                          .validate(checkReference)
                          .persist()
                          .label("Reference")
                          .doc("auto: the nodes elect one among themselves; this: this node's clock (an "
                               "ensemble's master); or the MAC of the device to follow")
                          // "this" applied to all would make every node the reference.
                          .ui("perNode");
    carriersNode_ = &o.value("carriers", carriers_)
                         .validate(checkCarriers)
                         .persist()
                         .label("Carriers")
                         .doc("What may carry it, in order of preference: udp (Ethernet or Wi-Fi), nowtp. A "
                              "carrier left out isn't used")
                         .ui("advanced");
    intervalNode_ = &o.value("interval", interval_)
                         .range(1, 600)
                         .unit("s")
                         .persist()
                         .label("Interval")
                         .doc("Between synchronization rounds, once synchronized")
                         .ui("advanced");
    o.value("late", late_)
        .range(0, kMaxLateMs)
        .unit("ms")
        .persist()
        .label("Late timed writes")
        .doc("How late a timed write may come and still be applied; later, it is dropped (the next one "
             "repairs the state)")
        .ui("advanced");

    auto& s = o.object("status").label("Status");
    statusNode_ = &s;
    s.value("synced", [this] { return status_.synced; }).label("Synchronized");
    s.value("isReference", [this] { return status_.isReference; }).label("This node is the reference");
    s.value("source", [this] { return status_.source; })
        .label("Reference node")
        .doc("The MAC of the device whose clock is the network time");
    s.value("carrier", [this] { return status_.carrier; })
        .label("Over")
        .doc("The carrier its last round went by: udp or nowtp");
    s.value("offset", [this] { return status_.offsetUs; })
        .unit("µs")
        .label("Offset")
        .doc("Network time minus this node's clock")
        .ui("advanced");
    s.value("roundTrip", [this] { return status_.roundTripUs; })
        .unit("µs")
        .label("Round trip")
        .doc("The shortest of the last round's exchanges with the reference")
        .ui("advanced");
    s.value("drift", [this] { return status_.driftPpm; }).unit("ppm").label("Drift").ui("advanced");
    s.value("rounds", [this] { return status_.rounds; })
        .label("Rounds")
        .doc("Synchronization rounds with the current reference")
        .ui("advanced");
    s.value("age", [this] { return status_.ageMs; }).unit("ms").label("Last round").ui("advanced");
    s.value("error", [this] { return status_.error; }).ui("style", "warning").ui("visibleWhen", "error!=");

    auto& w = o.object("writes").label("Timed writes").ui("advanced");
    writesNode_ = &w;
    w.value("queued", [this] { return writes().queued; }).label("Waiting");
    w.value("applied", [this] { return writes().applied; }).label("Applied");
    w.value("late", [this] { return writes().late; }).label("Late").doc("Came after their time: dropped");
    w.value("busy", [this] { return writes().busy; }).label("Refused").doc("The queue was full");
    w.value("failed", [this] { return writes().failed; }).label("Failed").doc("Refused when applied");
    w.value("lateMax", [this] { return writes().lateMaxUs; })
        .unit("µs")
        .label("Latest")
        .doc("The most an applied write was late: the API lock held it, or the task woke late");
}

void NetworkClock::loaded() {
    impl_.reset(new Impl(dev_));
    Platform& p = dev_.platform();
    if (!p.alarm) p.alarm = espAlarm();
    impl_->alarm = p.alarm;
    impl_->lateUs.store(late_ * 1000);
#if defined(TESSERKIT_NETWORK_CLOCK)
    Impl* impl = impl_.get();
    Device& dev = dev_;
    nowtp::TimeSyncConfig c;
    c.intervalMs = interval_ * 1000;
    impl_->core.reset(new nowtp::TimeSync([impl] { return impl->micros(); }, c, nowtp::Mac::from(dev_.info().mac),
                                          [&dev] { return dev.loopMs(); }));
    if (udp_) {
        impl_->udp.reset(new UdpCarrier(*udp_));
        impl_->core->addCarrier(*impl_->udp);
        UdpCarrier* carrier = impl_->udp.get();
        udp_->onSide([carrier, &dev](const tesser::PeerAddress&, const tesser::PeerAddress& device,
                                     const uint8_t* data, size_t len, uint64_t rxUs) {
            if (carrier->receive(device, data, len, rxUs)) dev.wake();
        });
    }
    // NowTp serves the core when it starts (NowTpConfig::timeCore): ESP-NOW
    // becomes its "nowtp" carrier then.
#else
    (void)udp_;
    status_.error = "this build has no NowTP: no network clock";
#endif
}

void NetworkClock::start() {
    referenceGen_ = referenceNode_->generation();
    carriersGen_ = carriersNode_->generation();
    intervalGen_ = intervalNode_->generation();
    Impl* impl = impl_.get();
    if (impl->alarm && !impl->alarm->begin([impl] { impl->fire(); })) {
        tesser::logError("tesserkit: the network clock's task didn't start: timed writes applied every period");
        impl->alarm = nullptr;
    }
    dev_.api().setScheduler([impl](tesser::TimedWrite&& w) { return impl->schedule(std::move(w)); });
#if defined(TESSERKIT_NETWORK_CLOCK)
    impl_->core->start(dev_.loopMs());
    applyReference();
    applyCarriers();
#endif
    refresh();
}

void NetworkClock::applyReference() {
#if defined(TESSERKIT_NETWORK_CLOCK)
    nowtp::TimeSync& core = *impl_->core;
    uint32_t now = dev_.loopMs();
    uint8_t mac[6];
    if (reference_ == "this") {
        core.setRole(nowtp::TimeRole::Reference, now);
        core.clearReference(now);
    } else if (parseMac(reference_, mac)) {
        core.setRole(nowtp::TimeRole::Follow, now);
        core.setReference(nowtp::Mac::from(mac), now);
    } else {
        core.setRole(nowtp::TimeRole::Candidate, now);
        core.clearReference(now);
    }
#endif
}

void NetworkClock::applyCarriers() {
#if defined(TESSERKIT_NETWORK_CLOCK)
    std::vector<std::string> names = carrierNames(carriers_);
    std::vector<const char*> list;
    for (const std::string& n : names) list.push_back(n.c_str());
    // Again each refresh: a carrier added since (NowTP's, when it starts)
    // takes its place, or is left out.
    impl_->core->setCarrierOrder(list.data(), list.size(), dev_.loopMs());
#endif
}

void NetworkClock::refresh() {
    Status_ st;
#if defined(TESSERKIT_NETWORK_CLOCK)
    nowtp::TimeSyncStatus t = impl_->core->status();
    st.synced = t.synced;
    st.isReference = t.isReference;
    if (t.isReference || t.synced || t.rounds) st.source = macText(t.reference.bytes);
    st.carrier = t.carrier ? t.carrier : "";
    st.offsetUs = t.offsetUs;
    st.roundTripUs = t.roundTripUs;
    st.driftPpm = t.driftPpm;
    st.rounds = t.rounds;
    st.ageMs = t.ageMs;
#endif
    st.error = status_.error;
    // The offset, drift and age move all the time: read when asked, and
    // announced with a round.
    bool changed = st.synced != status_.synced || st.isReference != status_.isReference ||
                   st.source != status_.source || st.carrier != status_.carrier || st.rounds != status_.rounds;
    status_ = std::move(st);
    if (changed) statusNode_->changed();
    Writes w = writes();
    bool wrote = w.queued != writes_.queued || w.applied != writes_.applied || w.late != writes_.late ||
                 w.busy != writes_.busy || w.failed != writes_.failed || w.lateMaxUs != writes_.lateMaxUs;
    writes_ = w;
    if (wrote) writesNode_->changed();
}

void NetworkClock::tick(uint32_t nowMs) {
    uint32_t gen = referenceNode_->generation();
    if (gen != referenceGen_) {
        referenceGen_ = gen;
        applyReference();
    }
    if ((gen = carriersNode_->generation()) != carriersGen_) {
        carriersGen_ = gen;
        applyCarriers();
    }
    if ((gen = intervalNode_->generation()) != intervalGen_) {
        intervalGen_ = gen;
#if defined(TESSERKIT_NETWORK_CLOCK)
        impl_->core->setInterval(interval_ * 1000);
#endif
    }
    impl_->lateUs.store(late_ * 1000);
#if defined(TESSERKIT_NETWORK_CLOCK)
    impl_->core->tick(nowMs);
#endif
    if (!impl_->alarm) impl_->fire();
    if (nowMs - refreshedMs_ >= kRefreshMs) {
        refreshedMs_ = nowMs;
        applyCarriers();
        refresh();
    }
}

}  // namespace tesserkit
