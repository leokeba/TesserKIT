// Network (docs/DESIGN.md section 5.2): the station state machine, the
// access point and its captive portal, mDNS.
#pragma once

#include "rig.h"

namespace network_cases {

using rig::at;
using rig::Status;
using tesserkit::Event;
using tesserkit::State;

struct NetRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Network net{dev};
    void air(const char* ssid, const char* password, int8_t rssi = -60, uint8_t channel = 6) {
        fake::Air a;
        a.ssid = ssid;
        a.password = password;
        a.rssi = rssi;
        a.channel = channel;
        fake.wifi.air.push_back(a);
    }
};

TEST(network_fresh_device_provisions) {
    NetRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.fake.wifi.apUp);  // recovery: no network saved
    CHECK_EQ(r.fake.wifi.ap.ssid, "test-device-a67c");
    CHECK_EQ(r.fake.wifi.ap.password, "");
    CHECK(r.fake.wifi.captive);
    CHECK_EQ(r.dev.state(), State::Provisioning);
    CHECK_EQ(r.fake.wifi.scans, 0);
    CHECK_EQ(r.value("/sys/network/ap/status"), "{\"active\":true,\"ip\":\"192.168.4.1\",\"clients\":0}");
    CHECK(r.fake.mdns.on);
    CHECK_EQ(r.fake.mdns.host, "test-device-a67c");
    CHECK_EQ(r.fake.mdns.version, "1.0.0");
    CHECK_EQ(r.fake.mdns.port, 80);
}

TEST(network_joins_the_strongest_saved_network) {
    NetRig r;
    r.air("Weak", "password1", -80);
    r.air("Strong", "password2", -40);
    r.air("Other", "", -30);
    r.net.defaultNetwork("Weak", "password1").defaultNetwork("Strong", "password2");
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.fake.wifi.last.ssid, "Strong");
    CHECK_EQ(r.dev.state(), State::Online);
    CHECK(r.net.connected());
    CHECK(!r.fake.wifi.apUp);
    CHECK(!r.fake.wifi.captive);
    std::string st = r.value("/sys/network/wifi/status");
    CHECK_EQ(at(st, "ssid"), "\"Strong\"");
    CHECK_EQ(at(st, "ip"), "\"192.168.1.50\"");
    CHECK_EQ(at(st, "rssi"), "-40");
    CHECK_EQ(at(st, "bssid"), "\"10:11:12:13:14:15\"");
}

TEST(network_falls_back_to_the_access_point) {
    NetRig r;
    r.net.defaultNetwork("Home", "password1");  // not in range
    std::vector<uint8_t> channels;
    r.dev.onChannel([&channels](uint8_t c) { channels.push_back(c); });
    CHECK(r.dev.begin());
    r.run(14000);
    CHECK(!r.fake.wifi.apUp);
    CHECK_EQ(r.dev.state(), State::Connecting);
    int tries = r.fake.wifi.connects;
    CHECK(tries >= 3);  // backoff: 1, 2, 5 s...
    CHECK(tries <= 5);
    r.run(1200);
    CHECK(r.fake.wifi.apUp);
    CHECK(r.fake.wifi.captive);
    CHECK_EQ(r.dev.state(), State::Connecting);  // a network is saved: not provisioning

    // The router comes back: joined at the next attempt (at most 30 s later).
    r.air("Home", "password1", -50, 11);
    r.run(31000);
    CHECK(r.net.connected());
    CHECK(!r.fake.wifi.captive);
    CHECK(r.fake.wifi.apUp);  // lingers for phones on it
    CHECK_EQ(channels.size(), 1u);
    if (!channels.empty()) CHECK_EQ(channels[0], 11);
    r.run(31000);
    CHECK(!r.fake.wifi.apUp);
}

TEST(network_backoff_caps_at_30_seconds) {
    NetRig r;
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.run(120000);
    // 1 + 2 + 5 + 10 + 30 + 30 + 30 = 108 s, plus the first attempt.
    CHECK_EQ(r.fake.wifi.connects, 8);
}

TEST(network_reconnects_after_a_drop) {
    NetRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());
    r.fake.wifi.drop();
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.fake.wifi.connects, 2);
    CHECK_EQ(r.fake.wifi.scans, 2);
}

TEST(network_wrong_password_tries_the_next) {
    NetRig r;
    r.air("A", "right", -40);
    r.air("B", "right", -70);
    r.net.defaultNetwork("A", "wrong").defaultNetwork("B", "right");
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.fake.wifi.last.ssid, "B");
    CHECK_EQ(r.fake.wifi.connects, 2);
}

TEST(network_silent_connection_times_out) {
    NetRig r;
    r.air("A", "pw", -40);
    r.air("B", "pw", -70);
    r.net.defaultNetwork("A", "pw").defaultNetwork("B", "pw");
    r.fake.wifi.connectSilent = true;
    CHECK(r.dev.begin());
    r.run(9000);
    CHECK_EQ(r.fake.wifi.connects, 1);
    r.fake.wifi.connectSilent = false;
    r.run(1500);
    CHECK(r.net.connected());
    CHECK_EQ(r.fake.wifi.last.ssid, "B");
}

TEST(network_hidden_network_is_tried) {
    NetRig r;
    r.air("Hidden", "pw");
    r.fake.wifi.air.back().hidden = true;
    r.net.defaultNetwork("Hidden", "pw");
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());
}

TEST(network_list_writes) {
    NetRig r;
    r.air("Home", "password1");
    r.air("Office", "password2");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());

    // Secrets never read back.
    CHECK_EQ(r.value("/sys/network/wifi/networks/Home/password"), "null");
    // Adding a network doesn't disturb the joined one.
    rig::Reply w = r.set("/sys/network/wifi/networks", "[{\"ssid\":\"Home\"},{\"ssid\":\"Office\",\"password\":\"password2\"}]");
    CHECK_EQ(w.status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.wifi.connects, 1);
    // The keyed list kept Home's password, which the write didn't carry.
    r.fake.wifi.drop();
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.fake.wifi.last.password, "password1");

    // Removing the joined network moves to the other one.
    int before = r.fake.wifi.connects;
    CHECK_EQ(r.set("/sys/network/wifi/networks", "[{\"ssid\":\"Office\"}]").status, Status::Ok);
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.fake.wifi.last.ssid, "Office");
    CHECK_EQ(r.fake.wifi.connects, before + 1);

    // A new password for the joined network reconnects with it.
    CHECK_EQ(r.set("/sys/network/wifi/networks/Office/password", "\"changed!\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.wifi.last.password, "changed!");
    CHECK(!r.net.connected());

    // Removing every network brings the access point up.
    CHECK_EQ(r.set("/sys/network/wifi/networks", "[]").status, Status::Ok);
    r.settle();
    CHECK(r.fake.wifi.apUp);
    CHECK_EQ(r.dev.state(), State::Provisioning);
    CHECK_EQ(r.set("/sys/network/wifi/networks", "[{},{},{},{},{},{}]").status, Status::InvalidValue);
}

TEST(network_static_address) {
    NetRig r;
    r.air("Home", "pw");
    r.net.defaultNetwork("Home", "pw");
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/static/ip", "\"10.0.0\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/static",
                   "{\"enabled\":true,\"ip\":\"10.0.0.7\",\"gateway\":\"10.0.0.1\"}")
                 .status,
             Status::Ok);
    r.settle();
    r.settle();
    CHECK(r.fake.wifi.last.staticIp);
    CHECK_EQ(r.fake.wifi.last.ip, 0x0A000007u);
    CHECK_EQ(r.fake.wifi.last.mask, 0xFFFFFF00u);
    CHECK_EQ(r.fake.wifi.last.dns, 0x0A000001u);  // the gateway when unset
    CHECK_EQ(r.value("/sys/network/wifi/status/ip"), "\"10.0.0.7\"");
    rig::Reply s = r.schema("/sys/network/wifi/networks");
    CHECK_EQ(at(s.body, "key"), "\"ssid\"");
    CHECK_EQ(at(s.body, "items/children/static/children/ip/ui/visibleWhen"), "\"enabled=true\"");
    // As deep through a client, which parses the reply (TesserAPI b1d3603).
    tesser::Query q;
    q.view = tesser::View::Schema;
    rig::Reply c = r.request(r.admin, rig::Op::Get, "/", {}, q);
    CHECK_EQ(at(c.body, "children/sys/children/network/children/wifi/children/networks/items/children/static/children/dns/ui/visibleWhen"),
             "\"enabled=true\"");
}

// A network without a router: an address and a mask, no gateway.
TEST(network_static_address_without_gateway) {
    NetRig r;
    r.air("Home", "pw");
    r.net.defaultNetwork("Home", "pw");
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/static", "{\"enabled\":true,\"ip\":\"10.0.0.7\"}").status,
             Status::Ok);
    r.settle();
    r.settle();
    CHECK(r.fake.wifi.last.staticIp);
    CHECK_EQ(r.fake.wifi.last.ip, 0x0A000007u);
    CHECK_EQ(r.fake.wifi.last.gateway, 0u);
    CHECK_EQ(r.fake.wifi.last.dns, 0u);  // no gateway, no DNS of its own
    CHECK_EQ(r.value("/sys/network/wifi/status/ip"), "\"10.0.0.7\"");

    // A DNS server of its own still applies.
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/static/dns", "\"10.0.0.53\"").status, Status::Ok);
    r.settle();
    r.settle();
    CHECK_EQ(r.fake.wifi.last.dns, 0x0A000035u);
    CHECK_EQ(r.fake.wifi.last.gateway, 0u);
}

// What a static address can't apply without is refused, and the refused
// patch changes nothing.
TEST(network_static_address_refusals) {
    NetRig r;
    r.air("Home", "pw");
    r.net.defaultNetwork("Home", "pw");
    CHECK(r.dev.begin());
    const char* bad[] = {
        "{\"enabled\":true,\"ip\":\"10.0.0.7\",\"mask\":\"\"}",
        "{\"enabled\":true,\"ip\":\"10.0.0.7\",\"mask\":\"255.0.255.0\"}",
        "{\"enabled\":true,\"ip\":\"10.0.0.7\",\"mask\":\"0.0.0.0\"}",
        "{\"enabled\":true,\"ip\":\"0.0.0.0\"}",
        "{\"enabled\":true,\"ip\":\"255.255.255.255\"}",
        "{\"enabled\":true,\"ip\":\"224.0.0.1\"}",
    };
    for (const char* b : bad) {
        CHECK_EQ(r.set("/sys/network/wifi/networks/Home/static", b).status, Status::InvalidValue);
    }
    CHECK_EQ(r.value("/sys/network/wifi/networks/Home/static"),
             "{\"enabled\":false,\"ip\":\"\",\"mask\":\"255.255.255.0\",\"gateway\":\"\",\"dns\":\"\"}");
    r.settle();
    CHECK(!r.fake.wifi.last.staticIp);
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/static/mask", "\"255.255.0.0\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/static/mask", "\"255.255.255.255\"").status, Status::Ok);
}

TEST(network_slow_dhcp_still_joins) {
    // A Freebox: associated in 3 s, then DHCP with address conflict
    // detection binds 7.3 s later, past 10 s from the attempt's start.
    NetRig r;
    r.air("Home", "pw");
    r.net.defaultNetwork("Home", "pw");
    r.fake.wifi.dhcpSilent = true;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"getting an address from \\\"Home\\\"\"");
    r.run(tesserkit::Network::kConnectTimeoutMs + 2000);
    CHECK_EQ(r.fake.wifi.connects, 1);  // not abandoned
    r.fake.wifi.giveAddress();
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.fake.wifi.connects, 1);
}

TEST(network_no_address_is_abandoned) {
    NetRig r;
    r.air("Home", "pw");
    r.net.defaultNetwork("Home", "pw");
    r.fake.wifi.dhcpSilent = true;
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/network/wifi/failed");
    r.settle();
    r.run(tesserkit::Network::kAddressTimeoutMs - 500);
    CHECK_EQ(r.fake.wifi.connects, 1);
    r.run(1000);
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"Couldn't join \\\"Home\\\": no address (DHCP)\"");
    r.run(2000);
    CHECK_EQ(r.fake.wifi.connects, 2);  // the next round, after its backoff
}

TEST(network_scan_doesnt_wait_for_dhcp) {
    NetRig r;
    r.air("Home", "pw");
    r.net.defaultNetwork("Home", "pw");
    r.fake.wifi.dhcpSilent = true;
    CHECK(r.dev.begin());
    r.settle();  // associated, waiting for an address
    rig::Reply s = r.request(r.admin, rig::Op::Set, "/sys/network/wifi/scan", {}, tesser::Query(), 500);
    CHECK(s.done);
    CHECK_EQ(s.status, Status::Ok);
}

TEST(network_scan) {
    NetRig r;
    r.air("Home", "pw", -55, 3);
    r.air("Cafe", "", -75, 11);
    CHECK(r.dev.begin());
    rig::Reply s = r.request(r.admin, rig::Op::Set, "/sys/network/wifi/scan", {}, tesser::Query(), 200);
    CHECK_EQ(s.status, Status::Ok);
    CHECK_EQ(s.body,
             "[{\"ssid\":\"Home\",\"rssi\":-55,\"channel\":3,\"secure\":true,"
             "\"accessPoints\":[{\"bssid\":\"10:11:12:13:14:15\",\"rssi\":-55,\"channel\":3}]},"
             "{\"ssid\":\"Cafe\",\"rssi\":-75,\"channel\":11,\"secure\":false,"
             "\"accessPoints\":[{\"bssid\":\"10:11:12:13:14:15\",\"rssi\":-75,\"channel\":11}]}]");
    CHECK_EQ(r.request(r.anon, rig::Op::Set, "/sys/network/wifi/scan").status, Status::Unauthorized);
}

// Two access points of one network: a router and its repeater.
struct MeshRig : NetRig {
    MeshRig() {
        air("Home", "password1", -50, 6);  // the repeater, nearer
        fake.wifi.air.back().bssid[5] = 0x40;
        air("Home", "password1", -70, 6);  // the router
        fake.wifi.air.back().bssid[5] = 0x24;
        net.defaultNetwork("Home", "password1");
    }
};

TEST(network_scan_lists_access_points) {
    MeshRig r;
    r.air("Cafe", "", -60, 11);
    CHECK(r.dev.begin());
    rig::Reply s = r.request(r.admin, rig::Op::Set, "/sys/network/wifi/scan", {}, tesser::Query(), 200);
    CHECK_EQ(s.status, Status::Ok);
    // One entry per network, strongest first, its access points strongest first.
    CHECK_EQ(at(s.body, "0/ssid"), "\"Home\"");
    CHECK_EQ(at(s.body, "0/rssi"), "-50");
    CHECK_EQ(at(s.body, "0/accessPoints/0/bssid"), "\"10:11:12:13:14:40\"");
    CHECK_EQ(at(s.body, "0/accessPoints/1/bssid"), "\"10:11:12:13:14:24\"");
    CHECK_EQ(at(s.body, "0/accessPoints/1/rssi"), "-70");
    CHECK_EQ(at(s.body, "1/ssid"), "\"Cafe\"");
}

TEST(network_joins_the_access_point_set) {
    MeshRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.value("/sys/network/wifi/status/bssid"), "\"10:11:12:13:14:40\"");  // the strongest
    CHECK(!r.fake.wifi.last.bssidSet);
    // Set to the one it is on: no reconnection (the reply would be lost).
    int connects = r.fake.wifi.connects;
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/bssid", "\"10:11:12:13:14:40\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.wifi.connects, connects);
    // Set to the router's: it reconnects there.
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/bssid", "\"10:11:12:13:14:24\"").status, Status::Ok);
    r.settle();
    CHECK(r.net.connected());
    CHECK(r.fake.wifi.last.bssidSet);
    CHECK_EQ(r.value("/sys/network/wifi/status/bssid"), "\"10:11:12:13:14:24\"");
    // Checked, and readable by a user like the rest of the list.
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/bssid", "\"10:11:12\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/bssid", "\"10-11-12-13-14-24\"").status, Status::InvalidValue);
    // The router goes away: any access point of the network rather than none.
    r.fake.wifi.air.pop_back();
    r.fake.wifi.drop();
    r.run(40000);
    CHECK(r.net.connected());
    CHECK(!r.fake.wifi.last.bssidSet);
    CHECK_EQ(r.value("/sys/network/wifi/status/bssid"), "\"10:11:12:13:14:40\"");
}

TEST(network_scan_waits_for_a_connection_attempt) {
    NetRig r;
    r.air("Home", "pw");
    r.net.defaultNetwork("Home", "pw");
    r.fake.wifi.connectSilent = true;
    CHECK(r.dev.begin());
    r.settle();  // joining
    uint64_t start = r.fake.core.now;
    rig::Reply s = r.request(r.admin, rig::Op::Set, "/sys/network/wifi/scan", {}, tesser::Query(), 12000);
    CHECK(s.done);
    CHECK_EQ(s.status, Status::Ok);
    CHECK(r.fake.core.now - start >= 9000);  // after the attempt timed out
    CHECK_EQ(r.fake.wifi.scans, 2);
}

TEST(network_access_point_settings) {
    NetRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.fake.wifi.apStarts, 1);
    CHECK_EQ(r.set("/sys/network/ap/password", "\"short\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/network/ap", "{\"ssid\":\"Setup\",\"password\":\"longenough\",\"channel\":11}").status,
             Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.wifi.apStarts, 2);  // restarted with the new settings
    CHECK_EQ(r.fake.wifi.ap.ssid, "Setup");
    CHECK_EQ(r.fake.wifi.ap.password, "longenough");
    CHECK_EQ(r.fake.wifi.ap.channel, 11);
    CHECK_EQ(r.value("/sys/network/ap/password"), "null");
    CHECK_EQ(r.set("/sys/network/ap/channel", "14").status, Status::InvalidValue);
    // The form's patch is atomic: a bad password leaves the name as it was.
    CHECK_EQ(r.set("/sys/network/ap", "{\"ssid\":\"Other\",\"password\":\"short\"}").status, Status::InvalidValue);
    CHECK_EQ(r.value("/sys/network/ap/ssid"), "\"Setup\"");
    rig::Reply s = r.schema("/sys/network/ap");
    CHECK_EQ(at(s.body, "ui/form"), "true");
    CHECK_EQ(at(s.body, "children/password/secret"), "true");
}

TEST(network_access_point_password_in_secure_mode) {
    // With an admin account and no password set, the access point gets one
    // of its own: made once, readable by admins, kept across restarts of
    // the access point; a password set wins; open again without the admin.
    NetRig r;
    tesserkit::Auth auth{r.dev};
    auth.defaultAdmin("admin", "secret-pass");
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.dev.secure());
    CHECK(r.fake.wifi.apUp);  // recovery: no network yet
    std::string made = r.fake.wifi.ap.password;
    CHECK_EQ(made.size(), 12u);
    CHECK_EQ(r.value("/sys/network/ap/securePassword"), "\"" + made + "\"");
    CHECK_EQ(r.value("/sys/network/ap/password"), "null");  // the setting is still empty
    CHECK_EQ(r.set("/sys/network/ap/password", "\"chosen-one\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.wifi.ap.password, "chosen-one");
    CHECK_EQ(r.set("/sys/network/ap/password", "\"\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.wifi.ap.password, made);  // the same one again
    CHECK_EQ(r.set("/sys/auth/users", "[]").status, Status::Ok);  // no admin: open mode
    r.settle();
    CHECK(!r.dev.secure());
    CHECK(r.fake.wifi.apUp);
    CHECK_EQ(r.fake.wifi.ap.password, "");
    CHECK_EQ(r.set("/sys/auth/users", R"([{"name":"ada","password":"secret-pass","role":"admin"}])").status,
             Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.wifi.ap.password, made);  // kept for this device
    CHECK_EQ(r.get("/sys/network/ap/securePassword", tesser::Query(), false).status,
             Status::Unauthorized);  // admins only
}

TEST(network_access_point_modes) {
    NetRig r;
    r.air("Home", "pw");
    r.net.defaultNetwork("Home", "pw").apMode(tesserkit::ApMode::Always);
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());
    CHECK(r.fake.wifi.apUp);
    CHECK(!r.fake.wifi.captive);  // the station is joined: no portal
    r.run(60000);
    CHECK(r.fake.wifi.apUp);
    CHECK_EQ(r.set("/sys/network/ap/mode", "\"never\"").status, Status::Ok);
    r.settle();
    CHECK(!r.fake.wifi.apUp);
    r.fake.wifi.air.clear();
    r.fake.wifi.drop();
    r.run(60000);
    CHECK(!r.fake.wifi.apUp);  // never, even offline
    CHECK_EQ(r.set("/sys/network/wifi/networks", "[]").status, Status::Ok);
    r.settle();
    CHECK(r.fake.wifi.apUp);  // ...except in recovery
}

TEST(network_hostname_and_mdns) {
    NetRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/network/hostname", "\"-bad\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/network/hostname", "\"helio-2\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.dev.hostname(), "helio-2");
    CHECK_EQ(r.fake.wifi.hostnameSet, "helio-2");
    CHECK_EQ(r.fake.mdns.host, "helio-2");
    CHECK_EQ(r.set("/sys/network/mdns", "false").status, Status::Ok);
    r.settle();
    CHECK(!r.fake.mdns.on);
}

TEST(network_hostname_follows_the_name) {
    NetRig r;
    CHECK(r.dev.begin());
    r.settle();
    CHECK_EQ(r.value("/sys/network/hostname"), "\"\"");
    CHECK_EQ(at(r.schema("/sys/network/hostname").body, "ui/perNode"), "true");  // not applied to all
    CHECK_EQ(r.fake.mdns.host, "test-device-a67c");
    CHECK_EQ(r.value("/sys/network/host"), "\"test-device-a67c\"");  // the one in use
    CHECK_EQ(r.set("/sys/network/host", "\"x\"").status, Status::ReadOnly);
    CHECK_EQ(r.set("/sys/name", "\"Stage left\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.wifi.hostnameSet, "stage-left");
    CHECK_EQ(r.fake.mdns.host, "stage-left");
    // A hostname set wins over the name, until it is emptied.
    CHECK_EQ(r.set("/sys/network/hostname", "\"rig-7\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.mdns.host, "rig-7");
    CHECK_EQ(r.value("/sys/network/host"), "\"rig-7\"");
    CHECK_EQ(r.set("/sys/name", "\"Stage right\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.mdns.host, "rig-7");
    CHECK_EQ(r.set("/sys/network/hostname", "\"\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.wifi.hostnameSet, "stage-right");
    CHECK_EQ(r.fake.mdns.host, "stage-right");
}

TEST(network_hostname_migrates) {
    // Settings version 1 stored the hostname itself: one equal to the
    // default is dropped, so it follows the name; another is the user's.
    for (const char* stored : {"test-device-a67c", "rig-7"}) {
        NetRig r;
        r.storage.records["@version"] = "1";
        r.storage.records["sys"] = std::string("{\"network\":{\"hostname\":\"") + stored + "\"}}";
        CHECK(r.dev.begin());
        bool user = std::string(stored) == "rig-7";
        CHECK_EQ(r.value("/sys/network/hostname"), user ? "\"rig-7\"" : "\"\"");
        CHECK_EQ(r.set("/sys/name", "\"Porch\"").status, Status::Ok);
        CHECK_EQ(r.dev.hostname(), user ? "rig-7" : "porch");
    }
}

TEST(network_access_levels) {
    NetRig r;
    r.net.defaultNetwork("Home", "pw");
    CHECK(r.dev.begin());
    CHECK_EQ(r.request(r.anon, rig::Op::Get, "/sys/network/wifi/networks").status, Status::Unauthorized);
    rig::Reply w = r.request(r.anon, rig::Op::Get, "/sys/network/wifi");
    CHECK_EQ(w.status, Status::Ok);
    CHECK_EQ(at(w.body, "networks"), "<missing>");
    CHECK_EQ(r.request(r.anon, rig::Op::Set, "/sys/network/mdns", "false").status, Status::Unauthorized);
    rig::Reply s = r.schema("/sys/network");
    CHECK_EQ(at(s.body, "access"), "\"admin\"");
}

TEST(network_settings_persist) {
    std::map<std::string, std::string> saved;
    {
        NetRig r;
        CHECK(r.dev.begin());
        r.set("/sys/network/wifi/networks", "[{\"ssid\":\"Home\",\"password\":\"secret-pw\"}]");
        r.set("/sys/network/hostname", "\"kept\"");
        r.run(2500);
        saved = r.storage.records;
    }
    NetRig r;
    r.air("Home", "secret-pw");
    r.storage.records = saved;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.dev.hostname(), "kept");
    CHECK_EQ(r.fake.wifi.hostnameSet, "kept");
}

TEST(network_ipv4_parsing) {
    uint32_t ip = 0;
    CHECK(tesserkit::parseIpv4("192.168.1.255", ip));
    CHECK_EQ(ip, 0xC0A801FFu);
    CHECK(!tesserkit::parseIpv4("192.168.1", ip));
    CHECK(!tesserkit::parseIpv4("192.168.1.256", ip));
    CHECK(!tesserkit::parseIpv4("1.2.3.4.5", ip));
    CHECK(!tesserkit::parseIpv4("1.2.3.0004", ip));
    CHECK(!tesserkit::parseIpv4("", ip));
    CHECK_EQ(tesserkit::formatIpv4(0x0A000001), "10.0.0.1");
}

TEST(network_joining_is_an_event) {
    NetRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/network/wifi/joined");
    CHECK_EQ(at(r.schema("/sys/network/wifi/joined").body, "ui/toast"), "\"ok\"");
    r.settle();
    CHECK(r.net.connected());
    // With the address clients on the access point must follow.
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"192.168.1.50 on Home\"");
    r.fake.wifi.drop();
    r.settle();
    CHECK_EQ(ev.take().size(), size_t(1));
}

// Ethernet (a board declared with Network::ethernet()).

struct EthRig : NetRig {
    EthRig() { net.ethernet(tesserkit::boards::gledopto()); }
};

TEST(network_ethernet_board_and_tree) {
    EthRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.fake.ethernet.begins, 1);
    CHECK(r.fake.ethernet.board.phy == tesserkit::EthernetBoard::Phy::Lan8720);
    CHECK_EQ(r.fake.ethernet.board.address, 1);
    CHECK_EQ(r.fake.ethernet.board.mdio, 33);
    CHECK_EQ(r.fake.ethernet.board.power, 5);
    CHECK_EQ(r.fake.ethernet.hostnameSet, "test-device-a67c");
    CHECK(!r.fake.ethernet.ip.staticIp);
    r.step();
    CHECK_EQ(r.value("/sys/network/ethernet/status"),
             "{\"state\":\"no cable\",\"link\":false,\"connected\":false,\"ip\":\"\",\"speed\":0,"
             "\"mac\":\"e0:e1:e2:e3:e4:e5\"}");
    CHECK_EQ(at(r.schema("/sys/network/ethernet/joined").body, "ui/toast"), "\"ok\"");
    // Before Wi-Fi, which it is preferred to.
    std::string net = r.schema("/sys/network").body;
    CHECK(net.find("\"ethernet\":{") < net.find("\"wifi\":{"));
    r.set("/sys/network/hostname", "\"stage-left\"");
    r.settle();
    CHECK_EQ(r.fake.ethernet.hostnameSet, "stage-left");
}

TEST(network_without_ethernet_has_no_tree) {
    NetRig r;  // no board declared
    CHECK(r.dev.begin());
    CHECK_EQ(r.get("/sys/network/ethernet").status, Status::NotFound);
    CHECK_EQ(r.fake.ethernet.begins, 0);

    NetRig chip;  // declared, on a chip without the MAC
    chip.dev.platform().ethernet = nullptr;
    chip.net.ethernet(tesserkit::boards::gledopto());
    CHECK(chip.dev.begin());
    CHECK_EQ(chip.get("/sys/network/ethernet").status, Status::NotFound);
    chip.settle();
    CHECK(chip.fake.wifi.apUp);  // recovery as without Ethernet
}

// A W5500 module on SPI: the same tree, its wiring handed to the platform.
TEST(network_ethernet_w5500_board) {
    NetRig r;
    r.net.ethernet(tesserkit::boards::w5500(7, 8, 9, 44, 43));
    CHECK(r.dev.begin());
    const tesserkit::EthernetBoard& b = r.fake.ethernet.board;
    CHECK(b.spi());
    CHECK(b.phy == tesserkit::EthernetBoard::Phy::W5500);
    CHECK_EQ(b.sck, 7);
    CHECK_EQ(b.miso, 8);
    CHECK_EQ(b.mosi, 9);
    CHECK_EQ(b.cs, 44);
    CHECK_EQ(b.irq, 43);
    CHECK_EQ(b.reset, -1);
    CHECK_EQ(b.spiMhz, 20);
    CHECK(!tesserkit::boards::gledopto().spi());
    CHECK_EQ(tesserkit::boards::w5500(7, 8, 9, 2).irq, -1);  // polled
    r.step();
    CHECK_EQ(at(r.value("/sys/network/ethernet/status"), "state"), "\"no cable\"");
    r.fake.ethernet.plug();
    r.run(10000);
    CHECK(r.net.ethernetConnected());
}

TEST(network_ethernet_is_preferred) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/network/ethernet/joined");
    r.fake.ethernet.plug();
    r.run(10000);
    CHECK(r.net.ethernetConnected());
    CHECK_EQ(r.fake.wifi.connects, 0);  // the station waited for the cable, then kept away
    CHECK_EQ(r.dev.state(), State::Online);
    CHECK_EQ(r.dev.address(), 0xC0A8013Cu);
    CHECK(memcmp(r.dev.addressMac(), r.fake.ethernet.state.mac, 6) == 0);
    CHECK(!r.fake.wifi.apUp);
    std::string st = r.value("/sys/network/ethernet/status");
    CHECK_EQ(at(st, "link"), "true");
    CHECK_EQ(at(st, "connected"), "true");
    CHECK_EQ(at(st, "ip"), "\"192.168.1.60\"");
    CHECK_EQ(at(st, "speed"), "100");
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"192.168.1.60 on Ethernet\"");

    // Unplugged: Wi-Fi takes over at once.
    r.fake.ethernet.unplug();
    r.settle();
    CHECK(!r.net.ethernetConnected());
    CHECK(r.net.connected());
    CHECK_EQ(r.dev.state(), State::Online);
    CHECK_EQ(r.dev.address(), 0xC0A80132u);
    CHECK(memcmp(r.dev.addressMac(), r.dev.info().mac, 6) == 0);
    CHECK_EQ(r.value("/sys/network/ethernet/status/ip"), "\"\"");

    // Plugged again: the station leaves.
    r.fake.ethernet.plug();
    r.settle();
    CHECK(r.net.ethernetConnected());
    CHECK(!r.net.connected());
    CHECK(!r.fake.wifi.linked);
    CHECK_EQ(r.value("/sys/network/wifi/status/connected"), "false");
    CHECK_EQ(r.dev.address(), 0xC0A8013Cu);
    r.run(60000);
    CHECK(!r.fake.wifi.linked);
    CHECK_EQ(r.fake.wifi.connects, 1);
}

TEST(network_ethernet_without_cable_waits_then_wifi) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.run(tesserkit::Network::kEthWaitMs - 200);
    CHECK_EQ(r.fake.wifi.scans, 0);
    CHECK_EQ(r.dev.state(), State::Connecting);
    r.run(400);
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.dev.state(), State::Online);
}

TEST(network_ethernet_link_without_dhcp_lets_wifi_in) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    r.fake.ethernet.dhcp = false;
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.run(tesserkit::Network::kEthWaitMs + 500);
    CHECK(r.net.connected());
    CHECK(!r.net.ethernetConnected());
    CHECK_EQ(r.value("/sys/network/ethernet/status/link"), "true");
    // The lease comes late: Ethernet takes over.
    r.fake.ethernet.dhcp = true;
    r.fake.ethernet.address(r.fake.ethernet.ip);
    r.settle();
    CHECK(r.net.ethernetConnected());
    CHECK(!r.net.connected());
}

TEST(network_ethernet_unplugged_before_its_address) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    r.fake.ethernet.dhcp = false;
    CHECK(r.dev.begin());
    r.run(3000);
    r.fake.ethernet.plug();  // the station's wait starts again
    r.run(1000);
    CHECK_EQ(r.fake.wifi.scans, 0);
    r.fake.ethernet.unplug();  // nothing to wait for
    r.settle();
    CHECK(r.net.connected());
}

TEST(network_ethernet_recovery_access_point) {
    EthRig r;  // no Wi-Fi network saved, no cable
    CHECK(r.dev.begin());
    r.settle();
    CHECK(!r.fake.wifi.apUp);  // the cable gets the fallback's time first
    CHECK_EQ(r.dev.state(), State::Provisioning);
    CHECK_EQ(r.dev.address(), 0u);
    r.run(tesserkit::Network::kApFallbackMs);
    CHECK(r.fake.wifi.apUp);
    CHECK(r.fake.wifi.captive);
    CHECK_EQ(r.dev.address(), 0xC0A80401u);
    CHECK_EQ(r.fake.wifi.scans, 0);

    r.fake.ethernet.plug();
    r.settle();
    CHECK_EQ(r.dev.state(), State::Online);
    CHECK(!r.fake.wifi.captive);
    CHECK(r.fake.wifi.apUp);  // for clients to follow the new address
    r.run(tesserkit::Network::kApLingerMs);
    CHECK(!r.fake.wifi.apUp);

    // Unplugged: offline, and the access point back after the fallback's time.
    r.fake.ethernet.unplug();
    r.settle();
    CHECK_EQ(r.dev.state(), State::Provisioning);
    r.run(tesserkit::Network::kApFallbackMs);
    CHECK(r.fake.wifi.apUp);
}

TEST(network_ethernet_static_address) {
    EthRig r;
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.settle();
    CHECK_EQ(r.value("/sys/network/ethernet/status/ip"), "\"192.168.1.60\"");
    CHECK_EQ(r.set("/sys/network/ethernet/static/gateway", "\"10.0.0\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/network/ethernet/static", "{\"enabled\":true,\"ip\":\"10.0.0.7\",\"gateway\":\"10.0.0.1\"}")
                 .status,
             Status::Ok);
    r.settle();
    CHECK(r.fake.ethernet.ip.staticIp);
    CHECK_EQ(r.fake.ethernet.ip.ip, 0x0A000007u);
    CHECK_EQ(r.fake.ethernet.ip.mask, 0xFFFFFF00u);
    CHECK_EQ(r.fake.ethernet.ip.dns, 0x0A000001u);
    CHECK_EQ(r.value("/sys/network/ethernet/status/ip"), "\"10.0.0.7\"");
    CHECK_EQ(r.dev.address(), 0x0A000007u);
    CHECK_EQ(at(r.schema("/sys/network/ethernet/static").body, "ui/form"), "true");
    CHECK_EQ(r.request(r.anon, rig::Op::Set, "/sys/network/ethernet/static/enabled", "false").status,
             Status::Unauthorized);

    // Persisted, and applied from the start.
    r.run(2500);
    EthRig again;
    again.storage.records = r.storage.records;
    CHECK(again.dev.begin());
    CHECK(again.fake.ethernet.ip.staticIp);
    CHECK_EQ(again.fake.ethernet.ip.ip, 0x0A000007u);
    CHECK_EQ(again.fake.ethernet.addressCalls, 0);  // given to begin()
}

// A new address waits a moment: the reply to the write that set it goes out
// first (TesserAPI sends replies after the API lock), and quick patches make
// one change.
TEST(network_settings_apply_after_the_reply) {
    EthRig r;
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.settle();
    CHECK_EQ(r.set("/sys/network/ethernet/static", "{\"enabled\":true,\"ip\":\"10.0.0.7\",\"gateway\":\"10.0.0.1\"}")
                 .status,
             Status::Ok);
    r.run(80);
    CHECK(!r.fake.ethernet.ip.staticIp);  // not yet
    CHECK_EQ(r.set("/sys/network/ethernet/static/ip", "\"10.0.0.8\"").status, Status::Ok);
    r.run(100);
    CHECK(!r.fake.ethernet.ip.staticIp);  // a further patch waits with it
    int calls = r.fake.ethernet.addressCalls;
    r.run(200);
    CHECK(r.fake.ethernet.ip.staticIp);
    CHECK_EQ(r.fake.ethernet.ip.ip, 0x0A000008u);
    CHECK_EQ(r.fake.ethernet.addressCalls, calls + 1);  // one change for both
}

TEST(network_ethernet_static_turned_off_releases_the_address) {
    // A cable straight to a computer: no DHCP, a static address.
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    r.fake.ethernet.dhcp = false;
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    CHECK_EQ(r.set("/sys/network/ethernet/static",
                   "{\"enabled\":true,\"ip\":\"192.168.3.50\",\"gateway\":\"192.168.3.1\"}")
                 .status,
             Status::Ok);
    r.run(10000);
    CHECK(r.net.ethernetConnected());
    CHECK(r.net.connected());  // a static address isn't a network beyond the cable
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"connected, static address\"");
    CHECK_EQ(r.dev.address(), 0xC0A80332u);

    // DHCP again, which nothing answers: the address goes at once.
    CHECK_EQ(r.set("/sys/network/ethernet/static/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK(!r.net.ethernetConnected());
    CHECK_EQ(r.value("/sys/network/ethernet/status/ip"), "\"\"");
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"waiting for DHCP\"");
    CHECK(r.net.connected());
    CHECK_EQ(r.dev.address(), 0xC0A80132u);

    // Static again: back on the cable at once.
    CHECK_EQ(r.set("/sys/network/ethernet/static/enabled", "true").status, Status::Ok);
    r.settle();
    CHECK(r.net.ethernetConnected());
    CHECK_EQ(r.dev.address(), 0xC0A80332u);
}

// A cable to a network without a router: Ethernet takes the address with no
// gateway (it keeps the default route, so traffic beyond the subnet fails).
TEST(network_ethernet_static_address_without_gateway) {
    EthRig r;
    r.fake.ethernet.dhcp = false;
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    CHECK_EQ(r.set("/sys/network/ethernet/static", "{\"enabled\":true,\"ip\":\"192.168.3.50\",\"mask\":\"255.255.0.0\"}")
                 .status,
             Status::Ok);
    r.settle();
    CHECK(r.fake.ethernet.ip.staticIp);
    CHECK_EQ(r.fake.ethernet.ip.ip, 0xC0A80332u);
    CHECK_EQ(r.fake.ethernet.ip.mask, 0xFFFF0000u);
    CHECK_EQ(r.fake.ethernet.ip.gateway, 0u);
    CHECK_EQ(r.fake.ethernet.ip.dns, 0u);
    CHECK(r.net.ethernetConnected());
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"connected, static address\"");
    CHECK_EQ(r.set("/sys/network/ethernet/static/mask", "\"255.255.255.1\"").status, Status::InvalidValue);
    CHECK_EQ(r.fake.ethernet.ip.mask, 0xFFFF0000u);
}

TEST(network_ethernet_that_doesnt_start) {
    EthRig r;
    r.fake.ethernet.starts = false;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());  // no wait for a cable that can't come
}

TEST(network_station_modes_with_ethernet) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    rig::Reply m = r.schema("/sys/network/wifi/mode");
    CHECK_EQ(at(m.body, "ui/labels"), "\"Always,Never,As a fallback to Ethernet\"");
    CHECK_EQ(r.value("/sys/network/wifi/mode"), "\"fallback\"");
    r.fake.ethernet.plug();
    r.run(6000);
    CHECK(r.net.ethernetConnected());
    CHECK(!r.net.connected());
    CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"not needed: Ethernet has a DHCP address\"");

    // Both: the station joins beside the lease.
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"always\"").status, Status::Ok);
    r.settle();
    CHECK(r.net.connected());
    CHECK(r.net.ethernetConnected());
    CHECK_EQ(r.dev.address(), 0xC0A8013Cu);  // Ethernet's
    r.fake.ethernet.unplug();
    r.fake.ethernet.plug();
    r.settle();
    CHECK(r.net.connected());  // a new lease doesn't push it away either

    // Off: the station leaves, and stays away without Ethernet.
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"never\"").status, Status::Ok);
    r.settle();
    CHECK(!r.net.connected());
    CHECK(!r.fake.wifi.linked);
    CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"off\"");
    r.fake.ethernet.unplug();
    r.run(60000);
    CHECK(!r.net.connected());
    CHECK_EQ(r.dev.state(), State::Connecting);

    // Back to the fallback: joined at once, Ethernet being down.
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"fallback\"").status, Status::Ok);
    r.settle();
    CHECK(r.net.connected());
}

TEST(network_station_mode_without_ethernet) {
    NetRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/network/wifi/mode"), "\"always\"");
    CHECK_EQ(at(r.schema("/sys/network/wifi/mode").body, "ui/labels"), "\"On,Off\"");
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"fallback\"").status, Status::InvalidValue);
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"never\"").status, Status::Ok);
    r.settle();
    CHECK(!r.net.connected());
    r.run(tesserkit::Network::kApFallbackMs + 100);
    CHECK(r.fake.wifi.apUp);  // its own network, offline
}

TEST(network_power_save) {
    NetRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/network/wifi/powerSave"), "\"min\"");
    CHECK(r.fake.wifi.ps == tesserkit::PowerSave::Min);  // set at start
    CHECK_EQ(r.set("/sys/network/wifi/powerSave", "\"none\"").status, Status::Ok);
    r.settle();
    CHECK(r.fake.wifi.ps == tesserkit::PowerSave::None);
    CHECK(r.net.connected());  // the station isn't disturbed
    int sets = r.fake.wifi.psSets;
    r.run(1000);
    CHECK_EQ(r.fake.wifi.psSets, sets);  // applied once
    CHECK_EQ(r.set("/sys/network/wifi/powerSave", "\"off\"").status, Status::InvalidValue);

    NetRig d;
    d.net.powerSave(tesserkit::PowerSave::None);  // factory default
    CHECK(d.dev.begin());
    CHECK_EQ(d.value("/sys/network/wifi/powerSave"), "\"none\"");
    CHECK(d.fake.wifi.ps == tesserkit::PowerSave::None);
}

TEST(network_ethernet_switched_off) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.run(6000);
    CHECK(r.net.ethernetConnected());
    CHECK(!r.net.connected());
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"connected\"");

    CHECK_EQ(r.set("/sys/network/ethernet/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK(!r.fake.ethernet.on);
    CHECK(!r.net.ethernetConnected());
    CHECK(r.net.connected());  // at once: no cable to wait for
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"off\"");
    CHECK_EQ(r.value("/sys/network/ethernet/status/link"), "false");
    CHECK_EQ(r.request(r.anon, rig::Op::Set, "/sys/network/ethernet/enabled", "true").status, Status::Unauthorized);

    // Off from the start after a restart, and the station doesn't wait for it.
    r.run(2500);
    EthRig again;
    again.air("Home", "password1");
    again.net.defaultNetwork("Home", "password1");  // the same firmware
    again.storage.records = r.storage.records;
    CHECK(again.dev.begin());
    CHECK(!again.fake.ethernet.on);
    again.settle();
    CHECK(again.net.connected());

    // On again: the lease takes over.
    CHECK_EQ(again.set("/sys/network/ethernet/enabled", "true").status, Status::Ok);
    again.settle();
    again.fake.ethernet.plug();
    again.settle();
    CHECK(again.net.ethernetConnected());
    CHECK(!again.net.connected());
}

TEST(network_ethernet_link_local_without_dhcp) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    r.fake.ethernet.dhcp = false;
    r.fake.ethernet.autoip = true;
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/network/ethernet/joined");
    r.fake.ethernet.plug();  // the address comes without an event
    r.run(tesserkit::Network::kPollMs + 100);
    CHECK(r.net.ethernetConnected());
    CHECK_EQ(r.value("/sys/network/ethernet/status/ip"), "\"169.254.10.11\"");
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"connected, link-local address (no DHCP server)\"");
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"169.254.10.11 on Ethernet (link-local)\"");
    CHECK_EQ(r.dev.state(), State::Online);
    // A link-local address isn't a network beyond the cable: the station joins.
    r.run(tesserkit::Network::kEthWaitMs);
    r.settle();
    CHECK(r.net.connected());

    // A DHCP server comes up late: its lease replaces the address, and the
    // station leaves it to Ethernet.
    r.fake.ethernet.dhcp = true;
    r.fake.ethernet.address(r.fake.ethernet.ip);
    r.settle();
    CHECK_EQ(r.value("/sys/network/ethernet/status/ip"), "\"192.168.1.60\"");
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"connected\"");
    CHECK(!r.net.connected());
}

TEST(network_station_says_why) {
    NetRig r;
    CHECK(r.dev.begin());
    r.step();
    CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"no network saved\"");
    rig::Events ev(r, "/sys/network/wifi/failed");
    CHECK_EQ(at(r.schema("/sys/network/wifi/failed").body, "ui/toast"), "\"warn\"");

    r.air("Home", "right-password");
    CHECK_EQ(r.set("/sys/network/wifi/networks", "[{\"ssid\":\"Home\",\"password\":\"wrong-password\"}]").status,
             Status::Ok);
    r.settle();
    CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"\\\"Home\\\": wrong password?; retrying\"");
    r.run(10000);  // more rounds, the same failure: told once
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"Couldn't join \\\"Home\\\": wrong password?\"");

    // Another failure is news.
    r.fake.wifi.air.clear();
    r.run(35000);
    CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"\\\"Home\\\": not found; retrying\"");
    got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"Couldn't join \\\"Home\\\": not found\"");

    // Fixed: joined, and a later failure is told again.
    r.air("Home", "right-password");
    CHECK_EQ(r.set("/sys/network/wifi/networks/Home/password", "\"right-password\"").status, Status::Ok);
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"connected\"");
}

TEST(network_station_waits_for_ethernet_in_words) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.step();
    CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"waiting for Ethernet\"");
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"no cable\"");
    r.fake.ethernet.dhcp = false;
    r.fake.ethernet.plug();
    r.settle();
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"waiting for DHCP\"");
}

// Events lost by the device's queue (full during a burst and a stall): the
// interfaces' state is read back from the platform.

// Loses the events `lose` matches, from now on.
inline void lossy(tesserkit::EventSink& sink, std::function<bool(const tesserkit::Event&)> lose) {
    tesserkit::EventSink real = sink;
    sink = [real, lose](tesserkit::Event e) {
        if (!lose(e)) real(std::move(e));
    };
}

TEST(network_lost_unplug_still_brings_the_station) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.run(10000);
    CHECK(r.net.ethernetConnected());
    CHECK(!r.net.connected());  // fallback: kept away by the lease
    lossy(r.fake.ethernet.sink, [](const tesserkit::Event& e) { return e.type == Event::Type::EthLink; });
    r.fake.ethernet.unplug();  // EthLink(0) lost
    r.run(tesserkit::Network::kPollMs + 500);
    CHECK(!r.net.ethernetConnected());
    CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"no cable\"");
    CHECK(r.net.connected());
    CHECK_EQ(r.dev.address(), 0xC0A80132u);
    // Plugged again, its events lost too: the lease is found, and the
    // station leaves.
    lossy(r.fake.ethernet.sink, [](const tesserkit::Event&) { return true; });
    r.fake.ethernet.plug();
    r.run(tesserkit::Network::kPollMs + 500);
    CHECK(r.net.ethernetConnected());
    CHECK(!r.net.connected());
    CHECK_EQ(r.dev.address(), 0xC0A8013Cu);
}

TEST(network_lost_disconnection_is_caught_up) {
    NetRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());
    int connects = r.fake.wifi.connects;
    tesserkit::EventSink sink = r.fake.wifi.sink;
    lossy(r.fake.wifi.sink, [](const tesserkit::Event& e) { return e.type == Event::Type::StaDisconnected; });
    r.fake.wifi.air.clear();  // the router goes
    r.fake.wifi.drop();       // its StaDisconnected lost
    r.fake.wifi.sink = sink;
    r.run(tesserkit::Network::kPollMs + 500);
    CHECK(!r.net.connected());
    CHECK_EQ(r.value("/sys/network/wifi/status/connected"), "false");
    CHECK_EQ(r.dev.state(), State::Connecting);
    CHECK(r.fake.wifi.connects > connects);  // trying again
    r.air("Home", "password1");  // back: joined at a retry
    r.run(10000);
    CHECK(r.net.connected());
    CHECK_EQ(r.dev.state(), State::Online);
}

TEST(network_lost_address_event_is_caught_up) {
    NetRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    r.fake.wifi.dhcpSilent = true;
    CHECK(r.dev.begin());
    r.settle();
    CHECK(!r.net.connected());
    lossy(r.fake.wifi.sink, [](const tesserkit::Event& e) { return e.type == Event::Type::StaGotIp; });
    r.fake.wifi.giveAddress();  // StaGotIp lost
    r.run(tesserkit::Network::kPollMs + 100);
    CHECK(r.net.connected());
    CHECK_EQ(r.value("/sys/network/wifi/status/ip"), "\"192.168.1.50\"");
}

// A disconnection of the link before arrives late, during the next attempt,
// which it makes Network give up while the radio goes on to join: a station
// Network doesn't count on. Found under chaos (chaos_cases.h): it stayed
// joined beside Ethernet's lease in fallback.
TEST(network_station_joined_behind_networks_back) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.run(10000);  // no cable: joined over Wi-Fi
    CHECK(r.fake.wifi.linked);
    // The router drops the station; its event is late.
    std::vector<tesserkit::Event> late;
    tesserkit::EventSink real = r.fake.wifi.sink;
    r.fake.wifi.sink = [&late, real](tesserkit::Event e) {
        if (e.type == Event::Type::StaDisconnected && e.text == "reason 200") {
            late.push_back(e);
        } else {
            real(std::move(e));
        }
    };
    r.fake.wifi.dhcpSilent = true;  // the next attempt waits for its address
    r.fake.wifi.drop();
    r.run(tesserkit::Network::kPollMs + 100);  // the loss read back, a new attempt
    CHECK_EQ(late.size(), size_t(1));
    r.fake.wifi.sink = real;
    for (tesserkit::Event& e : late) real(e);  // the old disconnection: the attempt is given up
    r.step();
    r.fake.wifi.giveAddress();  // but the radio joined
    r.run(tesserkit::Network::kPollMs + 100);
    CHECK(r.net.connected());  // taken as it is
    // Ethernet's lease: the station leaves, in fallback.
    r.fake.ethernet.plug();
    r.run(tesserkit::Network::kPollMs + 100);
    CHECK(r.net.ethernetConnected());
    CHECK(!r.fake.wifi.linked);
}

TEST(network_access_point_clients_are_read_back) {
    NetRig r;  // no network saved: the access point is up
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.fake.wifi.apUp);
    lossy(r.fake.wifi.sink, [](const tesserkit::Event& e) { return e.type == Event::Type::ApClients; });
    r.fake.wifi.apStations(1);
    r.run(tesserkit::Network::kPollMs + 100);
    CHECK_EQ(r.value("/sys/network/ap/status/clients"), "1");
    r.fake.wifi.apStations(0);
    r.run(tesserkit::Network::kPollMs + 100);
    CHECK_EQ(r.value("/sys/network/ap/status/clients"), "0");
}

// A burst of MQTT messages fills the queue while the task stalls: the
// station's disconnection still gets through at once, not at the next poll.
TEST(network_event_gets_through_an_mqtt_flood) {
    NetRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.settle();
    CHECK(r.net.connected());
    for (int i = 0; i < 64; i++) {
        tesserkit::Event m;
        m.type = Event::Type::MqttMessage;
        m.value = 1;
        m.text = "tesser/x/led/on/set";
        m.data = "true";
        r.dev.post(m);
    }
    // The platform still says joined (linked): only the event can tell.
    r.fake.wifi.disconnected(tesserkit::StaReason::Other, 200);
    r.step();
    CHECK(!r.net.connected());
    r.settle();
    CHECK(r.net.connected());  // joined again
}

// The IP stack's one list of DNS servers, refilled by Network when the
// stack empties it (DESIGN.md section 5.2). Seen on board B (2026-10-14): a
// station's attempt emptied it seconds after Ethernet's lease wrote it, and
// the device had no names until the lease's renewal, hours later.
TEST(network_dns_refilled_from_ethernet_while_the_station_tries) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.run(6000);
    CHECK(r.net.ethernetConnected());
    CHECK_EQ(r.fake.ipStack.list, 0xC0A80101u);  // the lease's
    int renews = r.fake.ipStack.renews;
    r.fake.wifi.dhcpSilent = true;  // the station associates, and no address comes
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"always\"").status, Status::Ok);
    r.run(3000);
    CHECK(r.fake.wifi.connects > 0);
    CHECK_EQ(r.fake.ipStack.list, 0xC0A80101u);  // emptied by the association, Ethernet's lease renewed
    CHECK(r.fake.ipStack.renews > renews);
}

TEST(network_dns_refilled_from_a_static_server_then_the_station) {
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"never\"").status, Status::Ok);
    r.fake.ethernet.plug();
    r.run(6000);
    CHECK_EQ(r.set("/sys/network/ethernet/static",
                   "{\"enabled\":true,\"ip\":\"10.0.0.7\",\"gateway\":\"10.0.0.1\",\"dns\":\"10.0.0.53\"}")
                 .status,
             Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.ipStack.list, 0x0A000035u);  // the static address emptied it: its own server
    // A static address with neither gateway nor server gives none: the
    // station's lease does (whose acknowledgement writes it first).
    CHECK_EQ(r.set("/sys/network/ethernet/static", "{\"gateway\":\"\",\"dns\":\"\"}").status, Status::Ok);
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"always\"").status, Status::Ok);
    r.settle();
    CHECK_EQ(r.fake.ipStack.list, 0xC0A801FEu);
    CHECK(r.net.connected());
    r.fake.ipStack.list = 0;  // emptied again, say by a renewal on the cable
    int renews = r.fake.ipStack.renews;
    r.fake.ipStack.changed();
    r.step();
    CHECK_EQ(r.fake.ipStack.list, 0xC0A801FEu);  // the station's lease renewed
    CHECK_EQ(r.fake.ipStack.renews, renews + 1);
}

TEST(network_dns_renewals_wait_for_a_silent_server) {
    EthRig r;
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.run(6000);
    r.fake.ipStack.answers = false;
    r.fake.ipStack.list = 0;
    int renews = r.fake.ipStack.renews;
    r.fake.ipStack.changed();
    r.run(20000);
    CHECK_EQ(r.fake.ipStack.list, 0u);
    int asked = r.fake.ipStack.renews - renews;
    CHECK(asked >= 3 && asked <= 5);  // every 5 s, not every poll
    r.fake.ipStack.answers = true;
    r.run(6000);
    CHECK_EQ(r.fake.ipStack.list, 0xC0A80101u);
}

TEST(network_dns_refilled_though_its_event_was_lost) {
    EthRig r;
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.run(6000);
    lossy(r.fake.ipStack.sink, [](const tesserkit::Event& e) { return e.type == Event::Type::DnsEmpty; });
    r.fake.ipStack.list = 0;
    r.fake.ipStack.changed();
    r.run(tesserkit::Network::kPollMs + 100);
    CHECK_EQ(r.fake.ipStack.list, 0xC0A80101u);
}

// The platform's reasons, in words; the rest by its own code.
TEST(network_station_failure_by_the_platforms_code) {
    NetRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    r.fake.wifi.connectSilent = true;
    CHECK(r.dev.begin());
    r.step();
    r.fake.wifi.disconnected(tesserkit::StaReason::Other, 3);
    r.step();
    CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"\\\"Home\\\": failed (reason 3); retrying\"");
}

// The default route: Ethernet while it has a gateway, else the station
// (DESIGN.md section 5.2).
TEST(network_route_follows_the_cables_gateway) {
    using tesserkit::NetIf;
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.run(6000);
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"always\"").status, Status::Ok);
    r.settle();
    CHECK(r.net.connected());
    CHECK(r.fake.ipStack.defaultSet);
    CHECK(r.fake.ipStack.defaultIf == NetIf::Ethernet);  // the lease's router
    // Static without a gateway: the internet stays on the station.
    CHECK_EQ(r.set("/sys/network/ethernet/static", "{\"enabled\":true,\"ip\":\"10.0.0.7\",\"gateway\":\"\"}").status,
             Status::Ok);
    r.settle();
    CHECK_EQ(r.value("/sys/network/ethernet/status/ip"), "\"10.0.0.7\"");
    CHECK(r.fake.ipStack.defaultIf == NetIf::Station);
    // With one: Ethernet's again.
    CHECK_EQ(r.set("/sys/network/ethernet/static/gateway", "\"10.0.0.1\"").status, Status::Ok);
    r.settle();
    CHECK(r.fake.ipStack.defaultIf == NetIf::Ethernet);
    // Unplugged: the station at once.
    r.fake.ethernet.unplug();
    r.step();
    CHECK(r.fake.ipStack.defaultIf == NetIf::Station);
    int sets = r.fake.ipStack.defaultSets;
    r.run(5000);
    CHECK_EQ(r.fake.ipStack.defaultSets, sets);  // set on changes only
}

// Board B (2026-10-21): a cable to a computer, link-local, and the station
// joined; with the cable as the default route every HTTPS request failed.
TEST(network_route_leaves_a_link_local_cable_to_the_station) {
    using tesserkit::NetIf;
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    r.fake.ethernet.dhcp = false;
    r.fake.ethernet.autoip = true;
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.run(15000);
    CHECK(r.net.ethernetConnected());
    CHECK(r.net.connected());  // no lease on the cable: the station joins
    CHECK(r.fake.ipStack.defaultIf == NetIf::Station);
    CHECK_EQ(r.fake.ipStack.list, 0xC0A801FEu);  // and names are the station's
    // The station gone: the cable is all there is.
    r.fake.wifi.air.clear();
    r.fake.wifi.drop();
    r.step();
    CHECK(r.fake.ipStack.defaultIf == NetIf::Ethernet);
}

TEST(network_route_follows_a_lost_unplug) {
    using tesserkit::NetIf;
    EthRig r;
    r.air("Home", "password1");
    r.net.defaultNetwork("Home", "password1");
    CHECK(r.dev.begin());
    r.fake.ethernet.plug();
    r.run(6000);
    CHECK_EQ(r.set("/sys/network/wifi/mode", "\"always\"").status, Status::Ok);
    r.settle();
    CHECK(r.net.connected());
    CHECK(r.fake.ipStack.defaultIf == NetIf::Ethernet);
    lossy(r.fake.ethernet.sink, [](const tesserkit::Event& e) { return e.type == Event::Type::EthLink; });
    lossy(r.fake.ipStack.sink, [](const tesserkit::Event& e) { return e.type == Event::Type::DnsEmpty; });
    r.fake.ethernet.unplug();
    r.step();
    CHECK(r.fake.ipStack.defaultIf == NetIf::Ethernet);  // nothing said so yet
    r.run(tesserkit::Network::kPollMs + 100);
    CHECK(r.fake.ipStack.defaultIf == NetIf::Station);
}

// Events are handled after the device's round read the clock, and a busy
// task (a gateway's) handles them a few milliseconds later: a time Network
// stamped from the clock then was ahead of the next tick's, `now - then`
// wrapped, and every attempt timed out "no address (DHCP)" as it
// associated (Gledopto ce48 as a peer, 2026-10-21).
TEST(network_event_times_are_the_rounds) {
    struct Slow : tesserkit::Service {
        fake::Platform& fake;
        Slow(tesserkit::Device& d, fake::Platform& f) : Service(d, "slow"), fake(f) {}
        void describe(tesser::Object&) override {}

    protected:
        void event(const tesserkit::Event&) override { fake.core.now += 3; }  // each event takes 3 ms
    };
    rig::Rig r;
    Slow slow{r.dev, r.fake};  // before Network: its events come later in the round
    tesserkit::Network net{r.dev};
    fake::Air a;
    a.ssid = "Home";
    a.password = "password1";
    r.fake.wifi.air.push_back(a);
    net.defaultNetwork("Home", "password1");
    r.fake.wifi.dhcpSilent = true;  // associated, the address late
    CHECK(r.dev.begin());
    r.run(3000);
    CHECK_EQ(r.fake.wifi.connects, 1);  // still waiting for its address, not failed and tried again
    r.fake.wifi.giveAddress();
    r.run(1000);
    CHECK(net.connected());
}

}  // namespace network_cases
