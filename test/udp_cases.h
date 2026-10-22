// UdpApi (docs/DESIGN.md section 5.18), with a second node on the network:
// its own tree and TesserAPI's UdpLink, the datagrams carried in memory.
#pragma once

#include <memory>

#include "rig.h"

namespace udp_cases {

using rig::at;
using rig::Op;
using rig::Status;
using tesser::PeerAddress;

inline PeerAddress ip(uint8_t last, uint16_t port = 8484) { return tesser::udpAddress(192, 168, 1, last, port); }

// Whether a datagram sent to `to` reaches the node at `self`: every node
// for the empty address, x.x.x.255 for the nodes on its port.
inline bool reaches(const PeerAddress& to, const PeerAddress& self) {
    if (to.length == 0) return true;
    if (to == self) return true;
    return to.length == 6 && to.bytes[3] == 255 && to.bytes[4] == self.bytes[4] && to.bytes[5] == self.bytes[5];
}

struct UdpRig : rig::Rig {
    tesserkit::UdpApi udp{dev};
    bool lamp = false;
    int plays = 0;
    // The other node: a lamp, and a play action.
    tesser::Api peerApi;
    bool peerLamp = false;
    int peerPlays = 0;
    std::string peerLast;
    std::vector<std::pair<PeerAddress, std::vector<uint8_t>>> peerOut;
    std::unique_ptr<tesser::UdpLink> peer;
    PeerAddress self = ip(59), other = ip(97);

    UdpRig() {
        auto& o = dev.api().object("lamp");
        o.value("on", lamp).writeAccess(tesser::Access::User);
        o.action("play", [this] { plays++; });
        auto& p = peerApi.object("lamp");
        p.value("on", peerLamp);
        p.action("play", [this](std::string step) {
            peerPlays++;
            peerLast = step;
        });
        peerApi.label("Peer");
        peerApi.authenticate([](std::string_view) { return tesser::Access::Admin; });
        peer.reset(new tesser::UdpLink(
            &peerApi,
            [this](const PeerAddress& to, const uint8_t* d, size_t n) {
                peerOut.push_back({to, std::vector<uint8_t>(d, d + n)});
                return true;
            },
            [this] { return static_cast<uint32_t>(fake.core.now); }));
        peer->setName("lamp-f37c");
        const uint8_t mac[6] = {0x10, 0x52, 0x1c, 0x75, 0xf3, 0x7c};
        peer->setDeviceId(mac);
        peer->start(0x9000);
        peer->advertise();
    }
    tesser::UdpLink* link() { return fake.udpApi.link(); }
    // Starts the device, with an address so the service starts at once.
    void online() {
        CHECK(dev.begin());
        const uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0xf4, 0xa6, 0x7c};
        dev.address(0xC0A8013B, mac);
        pump();
    }
    // Carries the datagrams both ways and runs both nodes until quiet,
    // `ms` of device periods at least.
    void pump(uint32_t ms = 100) {
        for (uint32_t t = 0; t < ms || !quiet(); t += 20) {
            if (t > 20000) break;
            // Through the chaos, when on: lost, late or out of order.
            auto mine = fake.chaos.carry(mineLine, std::move(fake.udpApi.out));
            fake.udpApi.out.clear();
            auto theirs = fake.chaos.carry(theirLine, std::move(peerOut));
            peerOut.clear();
            for (auto& d : mine) {
                if (reaches(d.to, other)) peer->packet(self, d.data.data(), d.data.size());
            }
            for (auto& d : theirs) {
                if (link() && link()->running() && reaches(d.first, self)) {
                    link()->packet(other, d.second.data(), d.second.size());
                }
            }
            peer->tick();
            if (link()) link()->tick();
            step(20);
        }
    }
    bool quiet() const {
        return fake.udpApi.out.empty() && peerOut.empty() && mineLine.held.empty() && theirLine.held.empty();
    }
    fake::Chaos::Line<fake::UdpApi::Datagram> mineLine;
    fake::Chaos::Line<std::pair<PeerAddress, std::vector<uint8_t>>> theirLine;
    // A request from the other node to the device, over UDP.
    rig::Reply fromPeer(Op op, const char* path, const char* body = "") {
        auto r = std::make_shared<rig::Reply>();
        peer->endpoint().request(self, op, path, body, [r](Status s, JsonVariantConst b) {
            r->status = s;
            serializeJson(b, r->body);
            r->done = true;
        });
        pump();
        return *r;
    }
    // An action of the device, completed over the network.
    rig::Reply call(const char* path, const char* body) {
        auto r = std::make_shared<rig::Reply>();
        admin.request(Op::Set, path, body, [r](Status s, JsonVariantConst b) {
            r->status = s;
            serializeJson(b, r->body);
            r->done = true;
        });
        for (int i = 0; i < 50 && !r->done; i++) {
            admin.process();
            pump(20);
        }
        admin.process();
        return *r;
    }
};

TEST(udp_starts_with_an_address_and_mounts_nodes) {
    UdpRig r;
    CHECK(r.dev.begin());
    r.step(100);
    CHECK_EQ(r.fake.udpApi.begins, 0);  // no network yet
    const uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0xf4, 0xa6, 0x7c};
    r.dev.address(0xC0A8013B, mac);
    r.pump();
    CHECK_EQ(r.fake.udpApi.begins, 1);
    CHECK_EQ(r.fake.udpApi.port, 8484);
    CHECK_EQ(r.fake.udpApi.name, r.dev.hostname());
    CHECK_EQ(r.value("/sys/udp/status/running"), "true");
    r.pump(1000);
    CHECK(tesserkit::findNode(r.dev.api(), "/peers/lamp-f37c") != nullptr);
    CHECK(r.value("/sys/udp/status/sent") != "0");
}

// A device with an admin account: no longer open to everyone (DESIGN.md
// section 5.7), so the nodes' level applies.
struct LockedRig : UdpRig {
    tesserkit::Auth auth{dev};
    LockedRig() { auth.defaultAdmin("admin", "secret-pass"); }
};

TEST(udp_open_device_serves_everyone) {
    UdpRig r;
    r.online();
    CHECK_EQ(r.fromPeer(Op::Set, "/sys/udp/mountPeers", "false").status, Status::Ok);
}

TEST(udp_nodes_go_public_when_an_admin_appears) {
    // Auto follows secure mode at run time, both ways.
    UdpRig r;
    tesserkit::Auth auth{r.dev};
    r.online();
    CHECK_EQ(r.fromPeer(Op::Get, "/sys/udp/peerAccess").status, Status::Ok);
    CHECK_EQ(r.set("/sys/auth/users", R"([{"name":"ada","password":"secret-pass","role":"admin"}])").status, Status::Ok);
    r.pump();
    CHECK(r.dev.secure());
    CHECK_EQ(r.fromPeer(Op::Get, "/lamp/on").status, Status::Ok);  // Public still reads what Public may
    CHECK_EQ(r.fromPeer(Op::Set, "/lamp/on", "true").status, Status::Unauthorized);
}

TEST(udp_serves_the_tree_at_the_nodes_level) {
    LockedRig r;
    r.online();
    rig::Reply got = r.fromPeer(Op::Get, "/lamp/on");
    CHECK_EQ(got.status, Status::Ok);
    CHECK_EQ(got.body, "false");
    // Secure mode (an admin account): auto gives the nodes Public, since
    // UDP carries no proof of who sends it.
    CHECK_EQ(r.value("/sys/udp/peerAccess"), "\"auto\"");
    CHECK_EQ(r.fromPeer(Op::Set, "/lamp/on", "true").status, Status::Unauthorized);
    CHECK_EQ(r.set("/sys/udp/peerAccess", "\"user\"").status, Status::Ok);
    r.pump();
    CHECK_EQ(r.fromPeer(Op::Set, "/lamp/on", "true").status, Status::Ok);
    CHECK(r.lamp);
    // User: /sys stays the admin's.
    CHECK_EQ(r.fromPeer(Op::Set, "/sys/udp/mountPeers", "false").status, Status::Unauthorized);
    CHECK_EQ(r.set("/sys/udp/peerAccess", "\"admin\"").status, Status::Ok);
    r.pump();
    CHECK_EQ(r.fromPeer(Op::Set, "/sys/udp/mountPeers", "false").status, Status::Ok);
    CHECK_EQ(r.set("/sys/udp/peerAccess", "\"public\"").status, Status::Ok);
    r.pump();
    CHECK_EQ(r.fromPeer(Op::Set, "/lamp/on", "false").status, Status::Unauthorized);
}

TEST(udp_send_broadcasts_to_every_node) {
    UdpRig r;
    r.online();
    rig::Reply sent = r.call("/sys/udp/send", R"({"path":"/lamp/play","value":"\"intro\""})");
    CHECK_EQ(sent.status, Status::Ok);
    r.pump();
    CHECK_EQ(r.peerPlays, 1);  // once, though broadcasts go twice
    CHECK_EQ(r.peerLast, "intro");
    CHECK_EQ(r.plays, 0);  // the sender doesn't apply its own
    CHECK(r.call("/sys/udp/send", R"({"path":"/lamp/on","value":"true"})").status == Status::Ok);
    r.pump();
    CHECK(r.peerLamp);
    CHECK(!r.lamp);
}

TEST(udp_send_to_one_node_replies) {
    UdpRig r;
    r.online();
    rig::Reply ok = r.call("/sys/udp/send", R"({"path":"/lamp/on","value":"true","to":"192.168.1.97"})");
    CHECK_EQ(ok.status, Status::Ok);
    CHECK_EQ(ok.body, "true");
    CHECK(r.peerLamp);
    rig::Reply missing = r.call("/sys/udp/send", R"({"path":"/nope","value":"1","to":"192.168.1.97:8484"})");
    CHECK_EQ(missing.status, Status::NotFound);
    CHECK_EQ(at(missing.body, "message") != "<missing>", true);
}

TEST(udp_send_is_checked) {
    UdpRig r;
    r.online();
    CHECK_EQ(r.call("/sys/udp/send", R"({"path":"lamp/on","value":"true"})").status, Status::InvalidValue);
    CHECK_EQ(r.call("/sys/udp/send", R"({"path":"/lamp/on","value":"tru"})").status, Status::InvalidValue);
    CHECK_EQ(r.call("/sys/udp/send", R"({"path":"/lamp/on","value":"true","to":"lamp.local"})").status,
             Status::InvalidValue);
    CHECK_EQ(r.as(tesser::Access::User, Op::Set, "/sys/udp/send", R"({"path":"/lamp/on","value":"true"})").status,
             Status::Unauthorized);
    CHECK_EQ(r.set("/sys/udp/enabled", "false").status, Status::Ok);
    r.pump();
    CHECK_EQ(r.fake.udpApi.ends, 1);
    CHECK_EQ(r.value("/sys/udp/status/running"), "false");
    CHECK_EQ(r.call("/sys/udp/send", R"({"path":"/lamp/on","value":"true"})").status, Status::NotAllowed);
}

TEST(udp_settings_apply) {
    UdpRig r;
    r.online();
    CHECK_EQ(r.set("/sys/udp/port", "9000").status, Status::Ok);
    r.pump();
    CHECK_EQ(r.fake.udpApi.ends, 1);
    CHECK_EQ(r.fake.udpApi.begins, 2);
    CHECK_EQ(r.fake.udpApi.port, 9000);
    CHECK_EQ(r.set("/sys/udp/port", "80").status, Status::InvalidValue);
    CHECK_EQ(r.value("/sys/udp/restartNeeded"), "false");
    CHECK_EQ(r.set("/sys/udp/mountPeers", "false").status, Status::Ok);
    r.pump();
    CHECK_EQ(r.value("/sys/udp/restartNeeded"), "true");
    CHECK_EQ(r.set("/sys/udp/enabled", "false").status, Status::Ok);
    r.pump();
    CHECK_EQ(r.set("/sys/udp/enabled", "true").status, Status::Ok);
    r.pump();
    CHECK_EQ(r.fake.udpApi.begins, 3);
    CHECK_EQ(r.value("/sys/udp/status/running"), "true");
}

TEST(udp_disabled_stays_off) {
    UdpRig r;
    r.udp.enable(false);
    r.online();
    r.pump(25000);
    CHECK_EQ(r.fake.udpApi.begins, 0);
}

TEST(udp_socket_that_wont_open) {
    UdpRig r;
    r.fake.udpApi.opens = false;
    r.online();
    r.pump(2000);
    CHECK_EQ(r.fake.udpApi.begins, 1);  // not every period
    CHECK_EQ(r.value("/sys/udp/status/running"), "false");
}

TEST(udp_without_its_own_task) {
    UdpRig r;
    r.udp.ownTask(false);
    r.online();
    CHECK(!r.fake.udpApi.ownTask);
    CHECK(r.fake.udpApi.queued != nullptr);
    // A request from the other node: only the device's loop ticks its link.
    auto done = std::make_shared<std::string>();
    r.peer->endpoint().request(r.self, Op::Get, "/lamp/on", "", [done](Status s, JsonVariantConst b) {
        *done = s == Status::Ok ? b.as<bool>() ? "true" : "false" : "error";
    });
    int polls = r.fake.udpApi.polls;
    for (int i = 0; i < 50 && done->empty(); i++) {
        auto mine = std::move(r.fake.udpApi.out);
        r.fake.udpApi.out.clear();
        auto theirs = std::move(r.peerOut);
        r.peerOut.clear();
        for (auto& d : mine) r.peer->packet(r.self, d.data.data(), d.data.size());
        for (auto& d : theirs) r.link()->packet(r.other, d.second.data(), d.second.size());
        r.peer->tick();
        r.step(20);
    }
    CHECK_EQ(*done, "false");
    CHECK(r.fake.udpApi.polls > polls);
}

TEST(udp_interfaces) {
    UdpRig r;
    r.online();
    CHECK_EQ(int(r.fake.udpApi.interfaces), 7);  // all of them by default
    CHECK_EQ(r.value("/sys/udp/interfaces"), "7");
    CHECK_EQ(r.schema("/sys/udp/interfaces").status, Status::Ok);
    CHECK_EQ(at(r.schema("/sys/udp/interfaces").body, "ui/widget"), "\"flags\"");
    int begins = r.fake.udpApi.begins;
    // Ethernet only: the transport starts again, on that interface.
    CHECK_EQ(r.set("/sys/udp/interfaces", "1").status, Status::Ok);
    r.pump();
    CHECK_EQ(r.fake.udpApi.begins, begins + 1);
    CHECK_EQ(int(r.fake.udpApi.interfaces), 1);
    CHECK_EQ(r.set("/sys/udp/interfaces", "0").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/udp/interfaces", "8").status, Status::InvalidValue);
    // Kept to one interface, a new address binds it again.
    const uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0xf4, 0xa6, 0x7c};
    r.dev.address(0xC0A8013C, mac);
    r.pump();
    CHECK_EQ(r.fake.udpApi.begins, begins + 2);
    r.pump(500);
    CHECK_EQ(r.fake.udpApi.begins, begins + 2);  // once

    UdpRig f;
    f.udp.interfaces(tesserkit::UdpApi::kEthernet | tesserkit::UdpApi::kAp);  // factory default
    f.online();
    CHECK_EQ(int(f.fake.udpApi.interfaces), 5);
}

}  // namespace udp_cases
