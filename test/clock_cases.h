// NetworkClock (docs/DESIGN.md sections 5.25 and 5.26): network time over
// UDP from another node's TimeSync, and timed writes applied at their time.
#pragma once

#include <memory>
#include <vector>

#include "nowtp/timesync.h"
#include "rig.h"
#include "tesserkit/network_clock.h"
#include "udp_cases.h"

namespace clock_cases {

using rig::Op;
using rig::Status;
using tesser::PeerAddress;

const uint8_t kPeerMac[6] = {0x10, 0x52, 0x1c, 0x75, 0xf3, 0x7c};

// The other node of udp_cases, with a TimeSync of its own over its UDP link:
// its clock `skewUs` ahead of the device's.
struct PeerClock : nowtp::TimeCarrier {
    tesser::UdpLink& link;
    uint64_t skewUs;
    const uint64_t& nowMs;
    std::unique_ptr<nowtp::TimeSync> core;
    struct In {
        nowtp::Mac from;
        std::vector<uint8_t> data;
        uint64_t rxUs;
    };
    std::vector<In> inbox;
    std::vector<std::pair<uint32_t, uint64_t>> sent;
    uint32_t tokens = 0;

    PeerClock(tesser::UdpLink& l, uint64_t skew, const uint64_t& now, nowtp::TimeRole role)
        : link(l), skewUs(skew), nowMs(now) {
        link.setMicros([this] { return local(); });
        link.onSide([this](const PeerAddress&, const PeerAddress& device, const uint8_t* d, size_t n, uint64_t rx) {
            if (device.length == 6) inbox.push_back(In{nowtp::Mac::from(device.bytes), {d, d + n}, rx});
        });
        nowtp::TimeSyncConfig c;
        c.role = role;
        core.reset(new nowtp::TimeSync([this] { return local(); }, c, nowtp::Mac::from(kPeerMac),
                                       [this] { return static_cast<uint32_t>(nowMs); }));
        core->addCarrier(*this);
        core->start(static_cast<uint32_t>(nowMs));
    }
    uint64_t local() const { return nowMs * 1000 + skewUs; }
    void tick() { core->tick(static_cast<uint32_t>(nowMs)); }

    const char* name() const override { return "udp"; }
    nowtp::Status send(const nowtp::Mac& device, const uint8_t* data, size_t len, uint32_t* token) override {
        PeerAddress to;
        if (!device.isBroadcast() && !link.addressOf(PeerAddress::fromBytes(device.bytes, 6), to)) {
            return nowtp::Status::LinkError;
        }
        uint64_t us = 0;
        if (!link.sendSide(to, data, len, &us)) return nowtp::Status::LinkError;
        *token = ++tokens;
        sent.push_back({tokens, us});
        return nowtp::Status::Ok;
    }
    bool reaches(const nowtp::Mac& device) const override {
        return link.endpoint().deviceOnline(PeerAddress::fromBytes(device.bytes, 6));
    }
    void poll(nowtp::TimeSync& c) override {
        auto s = std::move(sent);
        sent.clear();
        auto in = std::move(inbox);
        inbox.clear();
        for (auto& x : s) c.sent(*this, x.first, x.second);
        for (auto& x : in) c.received(*this, x.from, x.data.data(), x.data.size(), x.rxUs);
    }
    bool pending() const override { return !sent.empty() || !inbox.empty(); }
};

struct ClockRig : udp_cases::UdpRig {
    tesserkit::NetworkClock clock{dev, &udp};
    // Runs both nodes for `ms`, the peer's clock ticking with the device's.
    void runBoth(PeerClock& p, uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 20) {
            pump(20);
            p.tick();
        }
    }
};

TEST(clock_follows_a_reference_over_udp) {
    ClockRig r;
    PeerClock peer(*r.peer, 3000000, r.fake.core.now, nowtp::TimeRole::Reference);
    r.online();
    CHECK_EQ(r.value("/sys/clock/status/synced"), "false");
    r.runBoth(peer, 15000);
    CHECK_EQ(r.value("/sys/clock/status/synced"), "true");
    CHECK_EQ(r.value("/sys/clock/status/isReference"), "false");
    CHECK_EQ(r.value("/sys/clock/status/source"), "\"10:52:1c:75:f3:7c\"");
    CHECK_EQ(r.value("/sys/clock/status/carrier"), "\"udp\"");
    CHECK(r.clock.timeSynced());
    // Within the fake network's 20 ms steps.
    int64_t error = static_cast<int64_t>(r.clock.networkTimeUs()) - static_cast<int64_t>(peer.local());
    CHECK(error > -25000 && error < 25000);
    CHECK(r.fake.udpApi.link()->stats().sideSent > 0);
}

TEST(clock_carriers_left_out_are_not_used) {
    ClockRig r;
    r.clock.carriers("nowtp");
    PeerClock peer(*r.peer, 3000000, r.fake.core.now, nowtp::TimeRole::Reference);
    r.online();
    r.runBoth(peer, 15000);
    // No NowTP here: alone, it took over as the reference.
    CHECK_EQ(r.value("/sys/clock/status/isReference"), "true");
    CHECK_EQ(r.fake.udpApi.link()->stats().sideSent, 0u);
    CHECK(r.set("/sys/clock/carriers", "\"udp, radio\"").status != Status::Ok);
    CHECK(r.set("/sys/clock/carriers", "\"\"").status != Status::Ok);
    CHECK_EQ(r.set("/sys/clock/carriers", "\"nowtp,udp\"").status, Status::Ok);
    r.runBoth(peer, 15000);
    CHECK_EQ(r.value("/sys/clock/status/source"), "\"10:52:1c:75:f3:7c\"");
}

// A device that is its own reference: synchronized at once, its network
// time its own clock.
struct TimedRig : rig::Rig {
    tesserkit::NetworkClock clock{dev, nullptr};
    bool lamp = false;
    std::string label;
    std::vector<bool> seen;  // every value the lamp was set to
    TimedRig() {
        clock.reference("this");
        dev.api().object("lamp").value("on", [this] { return lamp; }, [this](bool on) {
            lamp = on;
            seen.push_back(on);
        });
        dev.api().object("lamp").value("label", [this] { return label; },
                                   [this](const std::string& value) { label = value; });
    }
    void begin() {
        CHECK(dev.begin());
        run(100);
    }
    // A set at network time `at`, through the admin client.
    Status timedSet(const char* path, const char* body, uint64_t at) {
        auto st = std::make_shared<Status>(Status::Internal);
        tesser::Query q;
        q.at = at;
        admin.set(path, body, [st](Status s, JsonVariantConst) { *st = s; }, q);
        admin.process();
        return *st;
    }
};

TEST(clock_applies_timed_writes_at_their_time) {
    TimedRig r;
    r.begin();
    CHECK(r.clock.timeSynced());
    CHECK_EQ(r.value("/sys/clock/status/isReference"), "true");
    uint64_t now = r.clock.networkTimeUs();
    CHECK_EQ(r.timedSet("/lamp/on", "true", now + 200000), Status::Ok);
    CHECK(!r.lamp);  // queued, not applied
    CHECK_EQ(r.value("/sys/clock/writes/queued"), "1");
    r.run(140);
    CHECK(!r.lamp);
    r.run(100);
    CHECK(r.lamp);
    r.run(1000);
    CHECK_EQ(r.value("/sys/clock/writes/applied"), "1");
    CHECK_EQ(r.value("/sys/clock/writes/queued"), "0");
}

// Writes to one path at one time aren't merged, and keep their order: a
// note-on and its note-off both happen.
TEST(clock_timed_writes_keep_their_order) {
    TimedRig r;
    r.begin();
    uint64_t at = r.clock.networkTimeUs() + 100000;
    CHECK_EQ(r.timedSet("/lamp/on", "true", at + 1), Status::Ok);
    CHECK_EQ(r.timedSet("/lamp/on", "false", at + 2), Status::Ok);
    CHECK_EQ(r.timedSet("/lamp/on", "true", at - 50000), Status::Ok);  // earlier: goes first
    r.run(300);
    CHECK_EQ(r.seen.size(), 3u);
    if (r.seen.size() == 3) {
        CHECK(r.seen[0]);
        CHECK(r.seen[1]);
        CHECK(!r.seen[2]);
    }
    CHECK(!r.lamp);
}

TEST(clock_late_writes_are_dropped) {
    TimedRig r;
    r.begin();
    uint64_t now = r.clock.networkTimeUs();
    CHECK_EQ(r.timedSet("/lamp/on", "true", now - 1000), Status::Timeout);
    r.run(100);
    CHECK(!r.lamp);
    CHECK_EQ(r.value("/sys/clock/writes/late"), "1");
    // Within /sys/clock/late, applied.
    CHECK_EQ(r.set("/sys/clock/late", "5").status, Status::Ok);
    r.run(20);
    now = r.clock.networkTimeUs();
    CHECK_EQ(r.timedSet("/lamp/on", "true", now - 1000), Status::Ok);
    r.run(40);
    CHECK(r.lamp);
}

TEST(clock_refuses_timed_writes_unsynchronized_or_full) {
    TimedRig r;
    r.clock.reference("aa:bb:cc:dd:ee:ff");  // a reference that never answers
    r.begin();
    CHECK(!r.clock.timeSynced());
    CHECK_EQ(r.timedSet("/lamp/on", "true", r.clock.networkTimeUs() + 100000), Status::NotAllowed);

    TimedRig full;
    full.begin();
    uint64_t at = full.clock.networkTimeUs() + 60000000;
    for (size_t i = 0; i < tesserkit::NetworkClock::kMaxQueued; i++) {
        CHECK_EQ(full.timedSet("/lamp/on", "true", at), Status::Ok);
    }
    CHECK_EQ(full.timedSet("/lamp/on", "true", at), Status::Busy);
    full.run(1000);
    CHECK_EQ(full.value("/sys/clock/writes/busy"), "1");
}

TEST(clock_queue_bounds_payload_independently_of_entry_layout) {
    TimedRig r;
    r.begin();
    const uint64_t at = r.clock.networkTimeUs() + 60000000;
    const std::string body = "\"" + std::string(200, 'x') + "\"";
    size_t queued = 0;
    for (; queued < tesserkit::NetworkClock::kMaxQueued; queued++) {
        Status s = r.timedSet("/lamp/label", body.c_str(), at);
        if (s == Status::Busy) break;
        CHECK_EQ(s, Status::Ok);
    }
    CHECK(queued > 0);
    CHECK(queued < tesserkit::NetworkClock::kMaxQueued);
    CHECK_EQ(r.clock.writes().busy, 1u);
}

// With a NetworkClock, NowTp serves its core: /sys/nowtp/time goes.
TEST(clock_takes_nowtps_time_over) {
    struct R : rig::Rig {
        tesserkit::NowTp nowtp{dev};
        tesserkit::NetworkClock clock{dev, nullptr};
    } r;
    CHECK(r.dev.begin());
    r.run(100);
    CHECK_EQ(r.get("/sys/nowtp/time").status, Status::NotFound);
    CHECK_EQ(r.get("/sys/clock/reference").body, "\"auto\"");
}

}  // namespace clock_cases
