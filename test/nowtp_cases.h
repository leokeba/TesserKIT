// NowTp (docs/DESIGN.md section 5.11).
#pragma once

#include "rig.h"

namespace nowtp_cases {

using rig::at;
using rig::Status;

struct NowTpRig : rig::Rig {
    tesserkit::Network net{dev};
    tesserkit::NowTp nowtp{dev};
};

TEST(nowtp_starts_with_its_settings) {
    NowTpRig r;
    r.nowtp.channel(6).pairingCode("1234");
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.fake.nowtp.begins, 1);
    CHECK_EQ(r.fake.nowtp.config.name, r.dev.hostname());
    CHECK_EQ(int(r.fake.nowtp.config.channel), 6);
    CHECK_EQ(r.fake.nowtp.config.pairingCode, "1234");
    CHECK_EQ(r.value("/sys/nowtp/status/started"), "true");
    CHECK_EQ(r.value("/sys/nowtp/status/channel"), "6");
    CHECK_EQ(r.value("/sys/nowtp/pairingCode"), "null");  // a secret
    // Nodes found are mounted under /peers.
    CHECK(r.fake.nowtp.mountedUnder != nullptr);
    CHECK_EQ(r.get("/peers").status, Status::Ok);
}

TEST(nowtp_waits_for_the_network_to_settle) {
    NowTpRig r;
    r.net.defaultNetwork("Workshop", "pw");  // saved, but never answers
    CHECK(r.dev.begin());
    r.run(5000);
    CHECK_EQ(r.fake.nowtp.begins, 0);  // still joining: no channel to hold
    r.run(16000);
    CHECK_EQ(r.fake.nowtp.begins, 1);  // the access point is up by now
}

TEST(nowtp_starts_once_joined) {
    NowTpRig r;
    fake::Air a;
    a.ssid = "Workshop";
    a.password = "pw";
    a.channel = 6;
    r.fake.wifi.air.push_back(a);
    r.net.defaultNetwork("Workshop", "pw");
    CHECK(r.dev.begin());
    r.run(5000);
    CHECK_EQ(r.dev.state(), tesserkit::State::Online);
    CHECK_EQ(r.fake.nowtp.begins, 1);
}

TEST(nowtp_without_mounting_has_no_peers_object) {
    NowTpRig r;
    r.nowtp.mountPeers(false);
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.fake.nowtp.mountedUnder == nullptr);
    CHECK_EQ(r.get("/peers").status, Status::NotFound);
}

TEST(nowtp_lists_the_nodes_nearby) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/nodes"), "[]");
    r.fake.nowtp.found("lamp-f37c", "10:52:1c:75:f3:7c");
    r.fake.nowtp.found("sensor", "aa:bb:cc:dd:ee:01", false);
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/nodes/10:52:1c:75:f3:7c/name"), "\"lamp-f37c\"");
    CHECK_EQ(r.value("/sys/nowtp/nodes/10:52:1c:75:f3:7c/tesser"), "true");
    CHECK_EQ(r.value("/sys/nowtp/nodes/aa:bb:cc:dd:ee:01/tesser"), "false");
    CHECK_EQ(r.set("/sys/nowtp/nodes", "[]").status, Status::ReadOnly);  // the radio's, not the client's
    r.fake.nowtp.nearby.erase(r.fake.nowtp.nearby.begin());
    r.run(1100);  // polled too
    CHECK_EQ(r.get("/sys/nowtp/nodes/10:52:1c:75:f3:7c").status, Status::NotFound);
}

TEST(nowtp_channel_and_long_range_apply_at_once) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/channel", "11").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/channel", "14").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/nowtp/longRange", "true").status, Status::Ok);
    r.run(1100);
    CHECK_EQ(r.value("/sys/nowtp/status/channel"), "11");
    CHECK(r.fake.nowtp.config.longRange);
    CHECK_EQ(r.value("/sys/nowtp/restartNeeded"), "false");
    CHECK_EQ(r.fake.nowtp.begins, 1);
}

TEST(nowtp_stops_and_starts_again) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.nowtp.ends, 1);
    CHECK_EQ(r.value("/sys/nowtp/status/started"), "false");
    CHECK_EQ(r.set("/sys/nowtp/pairing/pair").status, Status::NotAllowed);
    CHECK_EQ(r.set("/sys/nowtp/enabled", "true").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.nowtp.begins, 2);
    CHECK_EQ(r.value("/sys/nowtp/status/started"), "true");
}

TEST(nowtp_pairs_and_forgets) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/pairing/secured"), "false");
    CHECK_EQ(r.set("/sys/nowtp/pairing/pair").status, Status::Ok);
    CHECK_EQ(r.fake.nowtp.pairings, 1);
    CHECK_EQ(r.value("/sys/nowtp/pairing/state"), "\"Looking for a node with an open pairing window\"");
    CHECK_EQ(r.set("/sys/nowtp/pairing/accept").status, Status::Busy);
    r.fake.nowtp.paired(true, "10:52:1c:75:f3:7c");
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/pairing/state"), "\"Paired with 10:52:1c:75:f3:7c\"");
    CHECK_EQ(r.value("/sys/nowtp/pairing/secured"), "true");
    // Another pairing, which fails.
    CHECK_EQ(r.set("/sys/nowtp/pairing/accept").status, Status::Ok);
    CHECK_EQ(r.fake.nowtp.accepts, 1);
    r.fake.nowtp.paired(false, "timeout");
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/pairing/state"), "\"Not paired: timeout\"");
    CHECK_EQ(r.set("/sys/nowtp/pairing/forget").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/pairing/secured"), "false");
}

TEST(nowtp_settings_for_the_next_boot) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/restartNeeded"), "false");
    CHECK_EQ(r.set("/sys/nowtp/mountPeers", "false").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/restartNeeded"), "true");
}

TEST(nowtp_factory_reset_forgets_the_key) {
    NowTpRig r;
    tesserkit::System sys(r.dev);
    CHECK(r.dev.begin());
    r.settle();
    r.fake.nowtp.paired(true, "10:52:1c:75:f3:7c");
    r.settle();
    CHECK_EQ(r.set("/sys/factoryReset").status, Status::Ok);
    CHECK_EQ(r.fake.nowtp.forgets, 0);  // not in the request's task
    r.step();
    CHECK_EQ(r.fake.nowtp.forgets, 1);
}

TEST(nowtp_settings_need_admin) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.as(tesser::Access::User, rig::Op::Set, "/sys/nowtp/pairing/pair").status, Status::Unauthorized);
    CHECK_EQ(r.as(tesser::Access::User, rig::Op::Set, "/sys/nowtp/enabled", "false").status, Status::Unauthorized);
}

TEST(nowtp_without_its_own_task) {
    NowTpRig r;
    r.nowtp.ownTask(false);
    CHECK(r.dev.begin());
    r.settle();
    CHECK(!r.fake.nowtp.config.runTask);
    CHECK(r.fake.nowtp.config.queued != nullptr);
    tesser::DatagramEndpoint* ep = r.fake.nowtp.endpoint();
    CHECK(ep != nullptr);
    if (!ep) return;
    // A request from a node is answered by the device's own loop.
    const uint8_t mac[6] = {0x10, 0x52, 0x1c, 0x75, 0xf3, 0x7c};
    const char req[] = "{\"id\":7,\"op\":\"get\",\"path\":\"/sys/nowtp/status/started\"}";
    ep->receive(tesser::PeerAddress::fromBytes(mac, 6), req, sizeof(req) - 1, true);
    r.fake.nowtp.sent.clear();
    r.step(20);
    CHECK_EQ(r.fake.nowtp.sent.size(), 1u);
    if (!r.fake.nowtp.sent.empty()) CHECK(r.fake.nowtp.sent[0].text.find("\"id\":7") != std::string::npos);
    CHECK(r.fake.nowtp.polls > 0);
}

TEST(nowtp_network_time_settings_reach_the_radio) {
    NowTpRig r;
    r.nowtp.timeReference("this").timeInterval(2);
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.fake.nowtp.config.timeReference, "this");
    CHECK_EQ(r.fake.nowtp.config.timeIntervalMs, 2000u);
    CHECK_EQ(r.value("/sys/nowtp/time/reference"), "\"this\"");
    CHECK_EQ(r.value("/sys/nowtp/time/interval"), "2");
    // TesserUI's apply to all leaves it out: one reference, not every node.
    CHECK_EQ(at(r.schema("/sys/nowtp/time/reference").body, "ui/perNode"), "true");
}

TEST(nowtp_network_time_reference_changes_at_runtime) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.fake.nowtp.timeReferenceSet, "auto");
    CHECK_EQ(r.set("/sys/nowtp/time/reference", "\"10:52:1C:75:F3:7C\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/time/reference", "\"lamp\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/nowtp/time/reference", "\"10:52:1c:75:f3\"").status, Status::InvalidValue);
    r.settle();
    CHECK_EQ(r.fake.nowtp.timeReferenceSet, "10:52:1C:75:F3:7C");
    CHECK_EQ(r.set("/sys/nowtp/time/reference", "\"this\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/time/interval", "30").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/time/interval", "0").status, Status::InvalidValue);
    r.settle();
    CHECK_EQ(r.fake.nowtp.timeReferenceSet, "this");
    CHECK_EQ(r.fake.nowtp.config.timeIntervalMs, 30000u);
    CHECK_EQ(r.value("/sys/nowtp/restartNeeded"), "false");  // all at once
    CHECK_EQ(r.fake.nowtp.begins, 1);
    // Kept while NowTP is off, and given to it when it starts again.
    CHECK_EQ(r.set("/sys/nowtp/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/time/reference", "\"auto\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/enabled", "true").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.nowtp.config.timeReference, "auto");
    CHECK_EQ(r.fake.nowtp.timeReferenceSet, "auto");
}

TEST(nowtp_network_time_for_the_application) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(!r.nowtp.timeSynced());
    CHECK_EQ(r.value("/sys/nowtp/time/synced"), "false");
    auto& st = r.fake.nowtp.state;
    st.timeSynced = true;
    st.timeReferenceMac = "10:52:1c:75:f3:7c";
    st.timeOffsetUs = 1500;
    st.timeRoundTripUs = 420;
    st.timeRounds = 3;
    r.run(1100);
    CHECK(r.nowtp.timeSynced());
    CHECK_EQ(r.value("/sys/nowtp/time/synced"), "true");
    CHECK_EQ(r.value("/sys/nowtp/time/isReference"), "false");
    CHECK_EQ(r.value("/sys/nowtp/time/source"), "\"10:52:1c:75:f3:7c\"");
    CHECK_EQ(r.value("/sys/nowtp/time/offset"), "1500");
    CHECK_EQ(r.value("/sys/nowtp/time/roundTrip"), "420");
    CHECK_EQ(r.value("/sys/nowtp/time/rounds"), "3");
    uint64_t net = r.nowtp.networkTimeUs();
    CHECK_EQ(net, r.fake.core.now * 1000 + 1500);
    CHECK_EQ(r.nowtp.localTimeUs(net + 10000), static_cast<int64_t>(r.fake.core.now * 1000 + 10000));
}

}  // namespace nowtp_cases
