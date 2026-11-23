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

TEST(nowtp_announces_the_channel_a_network_joins) {
    NowTpRig r;
    fake::Air home;
    home.ssid = "Home";
    home.password = "password1";
    home.channel = 6;
    fake::Air work;
    work.ssid = "Work";
    work.password = "password2";
    work.channel = 11;
    r.fake.wifi.air.push_back(home);
    r.fake.wifi.air.push_back(work);
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.run(5000);
    CHECK(r.net.connected());
    CHECK_EQ(r.fake.nowtp.begins, 1);  // started once the network settled
    // The first join: the radio wasn't started yet, so nothing was announced.
    CHECK_EQ(r.fake.nowtp.announced.size(), 0u);
    // Switching to a network on another channel tells the nodes nearby,
    // before the connection is attempted.
    CHECK_EQ(r.set("/sys/network/wifi/networks", "[{\"ssid\":\"Work\",\"password\":\"password2\"}]").status, Status::Ok);
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.fake.wifi.last.ssid, "Work");
    CHECK_EQ(r.fake.nowtp.announced.size(), 1u);
    CHECK_EQ(r.fake.nowtp.announced[0], 11);
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
    CHECK_EQ(r.value("/sys/nowtp/nodes/aa:bb:cc:dd:ee:01/otherChannel"), "false");
    // Held on another channel by its access point: seen, out of reach.
    r.fake.nowtp.nearby.back().otherChannel = true;
    r.run(1100);
    CHECK_EQ(r.value("/sys/nowtp/nodes/aa:bb:cc:dd:ee:01/otherChannel"), "true");
    r.fake.nowtp.nearby.erase(r.fake.nowtp.nearby.begin());
    r.run(1100);  // polled too
    CHECK_EQ(r.get("/sys/nowtp/nodes/10:52:1c:75:f3:7c").status, Status::NotFound);
}

// The list is updated in place each second, and notified (whole, to every
// subscriber) when a node comes, goes or changes, or its signal moves by
// 10 dB: not every second for the signal's jitter.
TEST(nowtp_nodes_notify_on_change_not_every_second) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    r.fake.nowtp.found("lamp-f37c", "10:52:1c:75:f3:7c");
    r.settle();
    const tesser::Node* list = tesserkit::findNode(r.dev.api(), "/sys/nowtp/nodes");
    CHECK(list != nullptr);
    if (!list) return;
    CHECK_EQ(at(r.get("/sys/nowtp/nodes").body, "0/mac"), "\"10:52:1c:75:f3:7c\"");
    CHECK_EQ(r.value("/sys/nowtp/nodes/10:52:1c:75:f3:7c/rssi"), "-50");
    uint32_t gen = list->generation();
    // A few dB each second: read as it is, not notified.
    for (int8_t rssi : {-54, -47, -58, -52}) {
        r.fake.nowtp.nearby[0].rssi = rssi;
        r.run(1100);
        CHECK_EQ(list->generation(), gen);
    }
    CHECK_EQ(r.value("/sys/nowtp/nodes/10:52:1c:75:f3:7c/rssi"), "-52");
    // 10 dB from what was last notified (-50): notified.
    r.fake.nowtp.nearby[0].rssi = -61;
    r.run(1100);
    CHECK(list->generation() != gen);
    gen = list->generation();
    r.fake.nowtp.nearby[0].rssi = -55;  // 6 dB from -61
    r.run(1100);
    CHECK_EQ(list->generation(), gen);
    // Paired: a change of what it is, notified at once.
    r.fake.nowtp.nearby[0].authenticated = true;
    r.run(1100);
    CHECK(list->generation() != gen);
    CHECK_EQ(r.value("/sys/nowtp/nodes/10:52:1c:75:f3:7c/authenticated"), "true");
    // Another node comes, and keeps its place after the first.
    gen = list->generation();
    r.fake.nowtp.found("sensor", "aa:bb:cc:dd:ee:01", false);
    r.run(1100);
    CHECK(list->generation() != gen);
    CHECK_EQ(at(r.get("/sys/nowtp/nodes").body, "1/name"), "\"sensor\"");
    // Nothing moves: nothing notified.
    gen = list->generation();
    r.run(3300);
    CHECK_EQ(list->generation(), gen);
}

TEST(nowtp_keeps_the_station_awake_by_default) {
    // ESP-NOW loses frames while the modem sleeps: beside NowTp, Network's
    // power save defaults to none; the user's setting and the
    // application's still win.
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.value("/sys/network/wifi/powerSave"), "\"none\"");
    CHECK(r.fake.wifi.ps == tesserkit::PowerSave::None);
    CHECK_EQ(r.set("/sys/network/wifi/powerSave", "\"min\"").status, Status::Ok);
    r.settle();
    CHECK(r.fake.wifi.ps == tesserkit::PowerSave::Min);

    NowTpRig d;
    d.net.powerSave(tesserkit::PowerSave::Max);  // the application's factory default
    CHECK(d.dev.begin());
    d.settle();
    CHECK_EQ(d.value("/sys/network/wifi/powerSave"), "\"max\"");
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

TEST(nowtp_pairing_holds_the_radio) {
    // The pairing task uses NowTP and its key: neither stopping nor
    // forgetting while it runs.
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/pairing/accept").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/enabled", "false").status, Status::Busy);
    CHECK_EQ(r.set("/sys/nowtp/pairing/forget").status, Status::Busy);
    r.settle();
    CHECK_EQ(r.fake.nowtp.ends, 0);
    CHECK_EQ(r.fake.nowtp.forgets, 0);
    r.fake.nowtp.paired(false, "timeout");
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/pairing/forget").status, Status::Ok);
    // A pairing whose event the full queue lost: the radio's word ends it.
    CHECK_EQ(r.set("/sys/nowtp/pairing/pair").status, Status::Ok);
    r.fake.nowtp.pairingNow = false;
    CHECK_EQ(r.set("/sys/nowtp/pairing/accept").status, Status::Ok);
    r.fake.nowtp.paired(false, "timeout");
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.nowtp.ends, 1);
    CHECK_EQ(r.set("/sys/nowtp/enabled", "true").status, Status::Ok);  // nothing to wait for while off
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

// --- The radio's settings: transmit power and the PHY rates -------------------
// NowTP applies them to the driver at once (its runtime controls); the status
// tree reads the driver back.

TEST(nowtp_radio_settings_reach_the_driver_at_boot) {
    NowTpRig r;
    r.nowtp.txPower(15.25f).broadcastRate(tesserkit::Rate::G6M).unicastRate(tesserkit::Rate::B2M);
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.fake.nowtp.config.txPower, 15.25f);
    CHECK_EQ(r.fake.nowtp.config.broadcastRate, tesserkit::Rate::G6M);
    CHECK_EQ(r.fake.nowtp.config.unicastRate, tesserkit::Rate::B2M);
    // The settings as the radio runs them: the status tree.
    CHECK_EQ(r.value("/sys/nowtp/status/txPower"), "15.25");
    CHECK_EQ(r.value("/sys/nowtp/status/txPowerLimit"), "21");
    CHECK_EQ(r.value("/sys/nowtp/status/broadcastRate"), "\"6 Mbps\"");
}

TEST(nowtp_radio_settings_default_and_schema) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    // The defaults are the driver's: 0 its own power, Default its rates.
    CHECK_EQ(r.value("/sys/nowtp/txPower"), "0");
    CHECK_EQ(r.value("/sys/nowtp/broadcastRate"), "\"default (1 Mbps)\"");
    CHECK_EQ(r.value("/sys/nowtp/unicastRate"), "\"default (1 Mbps)\"");
    // The driver's units: 2–21 dBm in 0.25 steps, the chip's limit on top.
    rig::Reply s = r.schema("/sys/nowtp/txPower");
    CHECK_EQ(at(s.body, "min"), "0");
    CHECK_EQ(at(s.body, "max"), "21");
    CHECK_EQ(at(s.body, "step"), "0.25");
    CHECK_EQ(at(s.body, "unit"), "\"dBm\"");
    // The rates offer NowTP's table; the Long Range ones are last.
    rig::Reply e = r.schema("/sys/nowtp/broadcastRate");
    CHECK_EQ(at(e.body, "enum/0"), "\"default (1 Mbps)\"");
    CHECK_EQ(at(e.body, "enum/21"), "\"LR 250 kbps\"");
    CHECK_EQ(at(e.body, "enum/22"), "\"LR 500 kbps\"");
}

TEST(nowtp_radio_settings_reject_what_the_driver_cant_take) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    // The power is 0 (the driver's own) or 2–21 in 0.25 steps.
    CHECK_EQ(r.set("/sys/nowtp/txPower", "1.75").status, Status::InvalidValue);   // below 2, not its own
    CHECK_EQ(r.set("/sys/nowtp/txPower", "2.1").status, Status::InvalidValue);    // not a quarter step
    CHECK_EQ(r.set("/sys/nowtp/txPower", "-0.5").status, Status::InvalidValue);   // below the range
    CHECK_EQ(r.set("/sys/nowtp/txPower", "21.25").status, Status::InvalidValue);  // above it
    CHECK_EQ(r.set("/sys/nowtp/txPower", "15.25").status, Status::Ok);
    CHECK_EQ(r.value("/sys/nowtp/txPower"), "15.25");
    // A Long Range rate needs long range on; a rate the driver doesn't have is no option.
    CHECK_EQ(r.set("/sys/nowtp/broadcastRate", "\"LR 500 kbps\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/nowtp/unicastRate", "\"LR 250 kbps\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/nowtp/broadcastRate", "\"3 Mbps\"").status, Status::InvalidValue);
    // The refusals changed nothing.
    CHECK_EQ(r.value("/sys/nowtp/broadcastRate"), "\"default (1 Mbps)\"");
    CHECK_EQ(r.value("/sys/nowtp/unicastRate"), "\"default (1 Mbps)\"");
}

TEST(nowtp_long_range_and_the_rates_guard_each_other) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    // A Long Range rate needs long range on...
    CHECK_EQ(r.set("/sys/nowtp/broadcastRate", "\"LR 500 kbps\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/nowtp/longRange", "true").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/broadcastRate", "\"LR 500 kbps\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/unicastRate", "\"LR 250 kbps\"").status, Status::Ok);
    // ...and long range can't go off while one is set: it would persist a
    // state NowTP refuses at boot.
    CHECK_EQ(r.set("/sys/nowtp/longRange", "false").status, Status::InvalidValue);
    CHECK_EQ(r.value("/sys/nowtp/longRange"), "true");
    // Off once the rates leave Long Range.
    CHECK_EQ(r.set("/sys/nowtp/broadcastRate", "\"11 Mbps\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/unicastRate", "\"54 Mbps\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/longRange", "false").status, Status::Ok);
}

TEST(nowtp_radio_settings_apply_at_once) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/txPower", "15.25").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/broadcastRate", "\"6 Mbps\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/unicastRate", "\"2 Mbps\"").status, Status::Ok);
    r.run(1100);  // the tick applies them to the driver
    CHECK_EQ(r.fake.nowtp.config.txPower, 15.25f);
    CHECK_EQ(r.fake.nowtp.config.broadcastRate, tesserkit::Rate::G6M);
    CHECK_EQ(r.fake.nowtp.config.unicastRate, tesserkit::Rate::B2M);
    // The status reads the driver back: the power and the broadcast rate run.
    CHECK_EQ(r.value("/sys/nowtp/status/txPower"), "15.25");
    CHECK_EQ(r.value("/sys/nowtp/status/broadcastRate"), "\"6 Mbps\"");
    CHECK_EQ(r.value("/sys/nowtp/restartNeeded"), "false");  // all at once
    CHECK_EQ(r.fake.nowtp.begins, 1);
}

TEST(nowtp_radio_settings_persist) {
    std::map<std::string, std::string> saved;
    {
        NowTpRig r;
        CHECK(r.dev.begin());
        r.settle();
        CHECK_EQ(r.set("/sys/nowtp/longRange", "true").status, Status::Ok);
        CHECK_EQ(r.set("/sys/nowtp/txPower", "15.25").status, Status::Ok);
        CHECK_EQ(r.set("/sys/nowtp/broadcastRate", "\"LR 500 kbps\"").status, Status::Ok);
        CHECK_EQ(r.set("/sys/nowtp/unicastRate", "\"2 Mbps\"").status, Status::Ok);
        r.run(2500);  // the save is debounced
        saved = r.storage.records;
    }
    NowTpRig r;
    r.storage.records = saved;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.fake.nowtp.config.longRange);
    CHECK_EQ(r.fake.nowtp.config.txPower, 15.25f);
    CHECK_EQ(r.fake.nowtp.config.broadcastRate, tesserkit::Rate::LR500K);
    CHECK_EQ(r.fake.nowtp.config.unicastRate, tesserkit::Rate::B2M);
    CHECK_EQ(r.value("/sys/nowtp/longRange"), "true");
    CHECK_EQ(r.value("/sys/nowtp/txPower"), "15.25");
    CHECK_EQ(r.value("/sys/nowtp/broadcastRate"), "\"LR 500 kbps\"");
}

TEST(nowtp_restore_skips_a_rate_long_range_would_refuse) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    // A backup with a Long Range rate and long range off would not start: the
    // consistent half applies, the rate is left as it was.
    std::vector<std::string> skipped;
    CHECK_EQ(r.dev.api().restore(
                 "{\"sys\":{\"nowtp\":{\"longRange\":false,\"broadcastRate\":\"LR 500 kbps\"}}}", &skipped),
             Status::Ok);
    r.settle();
    CHECK_EQ(skipped.size(), 1u);
    if (!skipped.empty()) CHECK_EQ(skipped[0], "/sys/nowtp/broadcastRate");
    CHECK_EQ(r.value("/sys/nowtp/longRange"), "false");
    CHECK_EQ(r.value("/sys/nowtp/broadcastRate"), "\"default (1 Mbps)\"");
    // And the consistent pair comes back whole.
    std::vector<std::string> kept;
    CHECK_EQ(
        r.dev.api().restore("{\"sys\":{\"nowtp\":{\"longRange\":true,\"broadcastRate\":\"LR 500 kbps\"}}}", &kept),
        Status::Ok);
    r.settle();
    CHECK_EQ(kept.size(), 0u);
    CHECK_EQ(r.value("/sys/nowtp/broadcastRate"), "\"LR 500 kbps\"");
}


// --- The country code and the automatic power fallback ----------------------
// Both are NowTP's boot-time settings: no runtime setter, so they apply at
// the next boot (the restart-needed banner).

TEST(nowtp_country_code_and_auto_power_fallback_reach_the_driver) {
    NowTpRig r;
    r.nowtp.countryCode("US").autoPowerFallback(false);
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.fake.nowtp.config.countryCode, "US");
    CHECK_EQ(r.fake.nowtp.config.autoPowerFallback, false);
}

TEST(nowtp_country_code_defaults_and_schema) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    // Empty: the driver's own; the fallback on.
    CHECK_EQ(r.value("/sys/nowtp/countryCode"), "\"\"");
    CHECK_EQ(r.value("/sys/nowtp/autoPowerFallback"), "true");
    rig::Reply s = r.schema("/sys/nowtp/countryCode");
    CHECK_EQ(at(s.body, "persist"), "true");
}

TEST(nowtp_country_code_rejects_what_the_driver_cant_take) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    // Empty is the driver's; two letters or digits are the driver's code.
    CHECK_EQ(r.set("/sys/nowtp/countryCode", "\"US\"").status, Status::Ok);
    CHECK_EQ(r.value("/sys/nowtp/countryCode"), "\"US\"");
    CHECK_EQ(r.set("/sys/nowtp/countryCode", "\"01\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/nowtp/countryCode", "\"USA\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/nowtp/countryCode", "\"u\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/nowtp/countryCode", "\"U\"").status, Status::InvalidValue);
    // The refusals changed nothing.
    CHECK_EQ(r.value("/sys/nowtp/countryCode"), "\"01\"");
}

TEST(nowtp_country_code_and_auto_power_fallback_restart_the_radio) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/restartNeeded"), "false");
    CHECK_EQ(r.set("/sys/nowtp/countryCode", "\"FR\"").status, Status::Ok);
    r.settle();  // the tick sees the restart-needed settings changed
    CHECK_EQ(r.value("/sys/nowtp/restartNeeded"), "true");
    CHECK_EQ(r.set("/sys/nowtp/autoPowerFallback", "false").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.value("/sys/nowtp/restartNeeded"), "true");
    CHECK_EQ(r.fake.nowtp.begins, 1);  // not applied at once
}

// --- The status: the radio as it runs ----------------------------------------

TEST(nowtp_status_shows_the_radio_as_it_runs) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    // The fake's defaults: no ceiling learned, no AP, no soft-AP, the station's
    // interface, Wi-Fi started by Network.
    CHECK_EQ(r.value("/sys/nowtp/status/learnedTxCeilingDbm"), "0");
    CHECK_EQ(r.value("/sys/nowtp/status/stationConnected"), "false");
    CHECK_EQ(r.value("/sys/nowtp/status/softApActive"), "false");
    CHECK_EQ(r.value("/sys/nowtp/status/wifiStartedByNowTP"), "false");
    CHECK_EQ(r.value("/sys/nowtp/status/interface"), "\"\"");
    // What the driver reports, read back.
    r.fake.nowtp.state.learnedTxCeilingDbm = 17.5f;
    r.fake.nowtp.state.stationConnected = true;
    r.fake.nowtp.state.softApActive = false;
    r.fake.nowtp.state.wifiStartedByNowTP = true;
    r.fake.nowtp.state.interface = "station";
    r.run(1100);  // the radio's status is polled every second
    CHECK_EQ(r.value("/sys/nowtp/status/learnedTxCeilingDbm"), "17.5");
    CHECK_EQ(r.value("/sys/nowtp/status/stationConnected"), "true");
    CHECK_EQ(r.value("/sys/nowtp/status/softApActive"), "false");
    CHECK_EQ(r.value("/sys/nowtp/status/wifiStartedByNowTP"), "true");
    CHECK_EQ(r.value("/sys/nowtp/status/interface"), "\"station\"");
}

// --- Diagnostics: a link measured, and tuned --------------------------------

// A deferred request: answered when the client gets its reply.
std::shared_ptr<rig::Reply> callAsync(NowTpRig& r, const char* path, const std::string& body) {
    auto out = std::make_shared<rig::Reply>();
    r.admin.request(rig::Op::Set, path, body,
                    [out](Status s, JsonVariantConst b) {
                        out->status = s;
                        serializeJson(b, out->body);
                        out->done = true;
                    });
    r.admin.process();
    return out;
}

TEST(nowtp_diagnoses_a_link) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    // The MAC, either case: the diagnosis runs in the radio's task meanwhile.
    auto out = callAsync(r, "/sys/nowtp/diagnostics/diagnose", "\"AA:BB:CC:DD:EE:01\"");
    CHECK(!out->done);
    CHECK_EQ(r.fake.nowtp.diagnoses, 1);
    CHECK_EQ(int(r.fake.nowtp.diagPeer[0]), 0xaa);
    CHECK_EQ(int(r.fake.nowtp.diagPeer[5]), 0x01);
    CHECK(r.fake.nowtp.diagBusy);
    tesserkit::LinkDiagnosis d;
    d.responds = true;
    d.requestPowerDbm = -3.5f;
    d.ours.limitDbm = 21;
    d.ours.dsssDbm = 17;
    d.theirs.limitDbm = 19.5f;
    d.forward.push_back({tesserkit::Rate::G24M, 0, 20, 19, -50});
    d.reverse.push_back({tesserkit::Rate::B1M, 0, 20, 20, -52});
    d.forwardRssi = -50;
    d.reverseRssi = -52;
    d.issues.push_back({tesserkit::LinkIssue::Kind::WeakSignal, false, -90, tesserkit::Rate::B1M,
                        "raise the power or move closer"});
    r.fake.nowtp.diagnosed(Status::Ok, d);
    r.admin.process();
    CHECK(out->done);
    CHECK_EQ(out->status, Status::Ok);
    CHECK_EQ(at(out->body, "peer"), "\"aa:bb:cc:dd:ee:01\"");
    CHECK_EQ(at(out->body, "responds"), "true");
    CHECK_EQ(at(out->body, "requestPowerDbm"), "-3.5");
    CHECK_EQ(at(out->body, "ours/dsssDbm"), "17");
    CHECK_EQ(at(out->body, "theirs/limitDbm"), "19.5");
    CHECK_EQ(at(out->body, "forward/0/rate"), "\"24 Mbps\"");
    CHECK_EQ(at(out->body, "forward/0/delivery"), "0.95");
    CHECK_EQ(at(out->body, "reverse/0/received"), "20");
    CHECK_EQ(at(out->body, "forwardRssi"), "-50");
    CHECK_EQ(at(out->body, "issues/0/kind"), "\"WeakSignal\"");
    CHECK_EQ(at(out->body, "issues/0/local"), "false");
    CHECK_EQ(at(out->body, "issues/0/detail"), "\"raise the power or move closer\"");
    CHECK(!r.fake.nowtp.diagBusy);
}

// A peer that never answers is a result (responds false, an issue), and a
// failure is the reply's status instead.
TEST(nowtp_diagnosis_reports_a_failure) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    auto out = callAsync(r, "/sys/nowtp/diagnostics/diagnose", "\"aa:bb:cc:dd:ee:02\"");
    CHECK(!out->done);
    r.fake.nowtp.diagnosed(Status::Timeout, tesserkit::LinkDiagnosis());
    r.admin.process();
    CHECK(out->done);
    CHECK_EQ(out->status, Status::Timeout);
    CHECK_EQ(at(out->body, "message"), "\"no answer from the node\"");
}

TEST(nowtp_tunes_a_link) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    auto out = callAsync(r, "/sys/nowtp/diagnostics/optimize", "\"aa:bb:cc:dd:ee:03\"");
    CHECK(!out->done);
    CHECK_EQ(r.fake.nowtp.optimizes, 1);
    tesserkit::LinkProfile p;
    p.rateToPeer = tesserkit::Rate::G24M;
    p.rateFromPeer = tesserkit::Rate::G12M;
    p.txPowerDbm = 11;
    p.peerTxPowerDbm = 13.5f;
    p.deliveryToPeer = 1;
    p.deliveryFromPeer = 0.99f;
    p.applied = true;
    p.verified = true;
    r.fake.nowtp.optimized(Status::Ok, p);
    r.admin.process();
    CHECK(out->done);
    CHECK_EQ(out->status, Status::Ok);
    CHECK_EQ(at(out->body, "peer"), "\"aa:bb:cc:dd:ee:03\"");
    CHECK_EQ(at(out->body, "rateToPeer"), "\"24 Mbps\"");
    CHECK_EQ(at(out->body, "rateFromPeer"), "\"12 Mbps\"");
    CHECK_EQ(at(out->body, "txPowerDbm"), "11");
    CHECK_EQ(at(out->body, "peerTxPowerDbm"), "13.5");
    CHECK_EQ(at(out->body, "deliveryFromPeer"), "0.99");
    CHECK_EQ(at(out->body, "applied"), "true");
    CHECK_EQ(at(out->body, "verified"), "true");
}

TEST(nowtp_diagnostics_need_a_mac_and_admin) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/diagnostics/diagnose", "\"nope\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/nowtp/diagnostics/diagnose", "\"aa:bb:cc:dd:ee\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/nowtp/diagnostics/optimize", "\"\"").status, Status::InvalidValue);
    CHECK_EQ(r.fake.nowtp.diagnoses + r.fake.nowtp.optimizes, 0);
    CHECK_EQ(r.as(tesser::Access::User, rig::Op::Set, "/sys/nowtp/diagnostics/diagnose",
                  "\"aa:bb:cc:dd:ee:04\"")
                 .status,
             Status::Unauthorized);
    CHECK_EQ(r.fake.nowtp.diagnoses, 0);
}

TEST(nowtp_diagnostics_need_nowtp_running) {
    NowTpRig r;
    r.nowtp.enable(false);
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.set("/sys/nowtp/diagnostics/diagnose", "\"aa:bb:cc:dd:ee:05\"").status, Status::NotAllowed);
    CHECK_EQ(r.set("/sys/nowtp/diagnostics/optimize", "\"aa:bb:cc:dd:ee:05\"").status, Status::NotAllowed);
}

// One at a time: the radio runs one in its own task, and a pairing would
// pull it from under the diagnosis too.
TEST(nowtp_one_diagnosis_at_a_time) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    auto first = callAsync(r, "/sys/nowtp/diagnostics/diagnose", "\"aa:bb:cc:dd:ee:06\"");
    CHECK_EQ(r.fake.nowtp.diagnoses, 1);
    CHECK_EQ(r.set("/sys/nowtp/diagnostics/diagnose", "\"aa:bb:cc:dd:ee:07\"").status, Status::Busy);
    CHECK_EQ(r.set("/sys/nowtp/diagnostics/optimize", "\"aa:bb:cc:dd:ee:07\"").status, Status::Busy);
    CHECK_EQ(r.set("/sys/nowtp/pairing/pair").status, Status::Busy);
    CHECK_EQ(r.fake.nowtp.diagnoses, 1);
    CHECK_EQ(r.fake.nowtp.optimizes, 0);
    CHECK_EQ(r.fake.nowtp.pairings, 0);
    r.fake.nowtp.diagnosed(Status::Ok, tesserkit::LinkDiagnosis());
    r.admin.process();
    CHECK(first->done);
}

// The radio isn't stopped under a diagnosis: the next tick does it.
TEST(nowtp_a_diagnosis_delays_stopping_the_radio) {
    NowTpRig r;
    CHECK(r.dev.begin());
    r.settle();
    auto out = callAsync(r, "/sys/nowtp/diagnostics/diagnose", "\"aa:bb:cc:dd:ee:08\"");
    CHECK_EQ(r.set("/sys/nowtp/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.nowtp.ends, 0);
    r.fake.nowtp.diagnosed(Status::Ok, tesserkit::LinkDiagnosis());
    r.admin.process();
    CHECK(out->done);
    r.settle();
    CHECK_EQ(r.fake.nowtp.ends, 1);
    CHECK_EQ(r.value("/sys/nowtp/status/started"), "false");
}

}  // namespace nowtp_cases
