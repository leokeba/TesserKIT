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
    // The form's patch is atomic: a bad password leaves the name as it was.
    CHECK_EQ(r.set("/sys/network/ap", "{\"ssid\":\"Other\",\"password\":\"short\"}").status, Status::InvalidValue);
    CHECK_EQ(r.value("/sys/network/ap/ssid"), "\"Setup\"");
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
    CHECK_EQ(r.fake.mdns.host, "helio-2");
    CHECK_EQ(r.set("/sys/network/mdns", "false").status, Status::Ok);
    r.settle();
    CHECK(!r.fake.mdns.on);
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
    CHECK_EQ(r.value("/sys/network/ethernet/status"),
             "{\"link\":false,\"connected\":false,\"ip\":\"\",\"speed\":0,\"mac\":\"e0:e1:e2:e3:e4:e5\"}");
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
    CHECK(!r.net.connected());  // Ethernet first

    // DHCP again, which nothing answers: the address goes at once, and the
    // station joins once it has waited for a lease.
    CHECK_EQ(r.set("/sys/network/ethernet/static/enabled", "false").status, Status::Ok);
    r.settle();
    CHECK(!r.net.ethernetConnected());
    CHECK_EQ(r.value("/sys/network/ethernet/status/ip"), "\"\"");
    CHECK(!r.net.connected());
    r.run(tesserkit::Network::kEthWaitMs);
    r.settle();
    CHECK(r.net.connected());
    CHECK_EQ(r.dev.address(), 0xC0A80132u);

    // Static again: back on the cable at once, the station leaves.
    CHECK_EQ(r.set("/sys/network/ethernet/static/enabled", "true").status, Status::Ok);
    r.settle();
    CHECK(r.net.ethernetConnected());
    CHECK(!r.net.connected());
    CHECK_EQ(r.dev.address(), 0xC0A80332u);
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

}  // namespace network_cases
