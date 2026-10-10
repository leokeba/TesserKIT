// Network (docs/DESIGN.md section 5.2): the station state machine, the
// access point and its captive portal, mDNS.
#pragma once

#include "rig.h"

namespace network_cases {

using rig::at;
using rig::Status;
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
    CHECK(r.fake.wifi.mdnsOn);
    CHECK_EQ(r.fake.wifi.mdnsHost, "test-device-a67c");
    CHECK_EQ(r.fake.wifi.mdnsVersion, "1.0.0");
    CHECK_EQ(r.fake.wifi.mdnsPort, 80);
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
}

TEST(network_scan) {
    NetRig r;
    r.air("Home", "pw", -55, 3);
    r.air("Cafe", "", -75, 11);
    CHECK(r.dev.begin());
    rig::Reply s = r.request(r.admin, rig::Op::Set, "/sys/network/wifi/scan", {}, tesser::Query(), 200);
    CHECK_EQ(s.status, Status::Ok);
    CHECK_EQ(s.body, "[{\"ssid\":\"Home\",\"rssi\":-55,\"channel\":3,\"secure\":true},"
                     "{\"ssid\":\"Cafe\",\"rssi\":-75,\"channel\":11,\"secure\":false}]");
    CHECK_EQ(r.request(r.anon, rig::Op::Set, "/sys/network/wifi/scan").status, Status::Unauthorized);
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
    rig::Reply s = r.schema("/sys/network/ap");
    CHECK_EQ(at(s.body, "ui/form"), "true");
    CHECK_EQ(at(s.body, "children/password/secret"), "true");
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
    CHECK_EQ(r.fake.wifi.mdnsHost, "helio-2");
    CHECK_EQ(r.set("/sys/network/mdns", "false").status, Status::Ok);
    r.settle();
    CHECK(!r.fake.wifi.mdnsOn);
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

}  // namespace network_cases
