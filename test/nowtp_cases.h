// NowTp (docs/DESIGN.md section 5.11).
#pragma once

#include "rig.h"

namespace nowtp_cases {

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
    CHECK_EQ(r.fake.nowtp.forgets, 1);
}

TEST(nowtp_settings_need_admin) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.as(tesser::Access::User, rig::Op::Set, "/sys/nowtp/pairing/pair").status, Status::Unauthorized);
    CHECK_EQ(r.as(tesser::Access::User, rig::Op::Set, "/sys/nowtp/enabled", "false").status, Status::Unauthorized);
}

}  // namespace nowtp_cases
