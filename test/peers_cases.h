// Peers (docs/DESIGN.md section 5.19): a device reached over UDP and NowTP,
// the second node of udp_cases with an endpoint of each kind.
#pragma once

#include <memory>

#include "rig.h"
#include "udp_cases.h"

namespace peers_cases {

using rig::at;
using rig::Op;
using rig::Status;
using tesser::PeerAddress;

const uint8_t kPeerMac[6] = {0x10, 0x52, 0x1c, 0x75, 0xf3, 0x7c};
const uint8_t kSelfMac[6] = {0xc8, 0xf0, 0x9e, 0xf4, 0xa6, 0x7c};

struct PeersRig : udp_cases::UdpRig {
    tesserkit::NowTp nowtp{dev};
    tesserkit::Peers peers{dev};
    // The other node's NowTP side: its endpoint, the messages it sends.
    std::unique_ptr<tesser::DatagramEndpoint> peerNow;
    std::vector<std::pair<PeerAddress, std::string>> peerNowOut;
    bool udpUp = true, nowUp = true;  // the links carry datagrams
    fake::Chaos::Line<fake::NowTp::Message> nowMine;
    fake::Chaos::Line<std::pair<PeerAddress, std::string>> nowTheirs;
    std::vector<std::string> lost;    // onDeviceLost()

    PeersRig() {
        peerNow.reset(new tesser::DatagramEndpoint(
            &peerApi, tesser::TransportKind::NowTP,
            [this](const PeerAddress& to, const std::string& m, tesser::Delivery) {
                peerNowOut.push_back({to, m});
                return true;
            },
            [this] { return static_cast<uint32_t>(fake.core.now); }, tesser::DatagramEndpoint::Options()));
        peers.onDeviceLost([this](const PeerAddress& d, tesser::PeerLoss why) {
            lost.push_back(std::string(1, char(d.bytes[5])) + tesser::toString(why));
        });
    }
    static PeerAddress mac(const uint8_t* m) { return PeerAddress::fromBytes(m, 6); }
    tesser::DatagramEndpoint* now() { return fake.nowtp.ep.get(); }
    // NowTP's discovery: the peer is announced, as NowTpTransport does.
    void discoverOverNowTp() {
        now()->peerSeen(mac(kPeerMac), "lamp-f37c", 0x1234, mac(kPeerMac));
        pumpAll();
    }
    // Both links, both nodes, until quiet.
    void pumpAll(uint32_t ms = 100) {
        for (uint32_t t = 0; t < ms || !peerNowOut.empty() || !fake.nowtp.sent.empty(); t += 20) {
            if (t > 20000) break;
            auto mine = fake.chaos.carry(nowMine, std::move(fake.nowtp.sent));
            fake.nowtp.sent.clear();
            auto theirs = fake.chaos.carry(nowTheirs, std::move(peerNowOut));
            peerNowOut.clear();
            if (nowUp) {
                for (auto& m : mine) peerNow->receive(mac(kSelfMac), m.text.data(), m.text.size(), true);
                for (auto& m : theirs) now()->receive(mac(kPeerMac), m.second.data(), m.second.size(), true);
            }
            peerNow->process();
            if (now()) now()->process();
            admin.process();
            if (udpUp) {
                pump(20);
            } else {  // the datagrams are lost
                fake.udpApi.out.clear();
                peerOut.clear();
                peer->tick();
                if (link()) link()->tick();
                step(20);
            }
        }
    }
    // A read through the admin client, forwarded to the node; the reply
    // comes while the links are pumped.
    std::shared_ptr<rig::Reply> fetch(const char* path) {
        auto out = std::make_shared<rig::Reply>();
        admin.request(Op::Get, path, "", [out](Status s, JsonVariantConst b) {
            out->status = s;
            serializeJson(b, out->body);
            out->done = true;
        });
        return out;
    }
    void start() {
        online();
        run(1000);
        CHECK(now() != nullptr);
    }
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 500) pumpAll(500);
    }
    std::string status(const char* key) {
        return at(get("/sys/peers/status").body, (std::string("0/") + key).c_str());
    }
};

// The events of one path, all of them so far.
struct Heard {
    rig::Events ev;
    std::vector<std::string> all;
    Heard(rig::Rig& r, const char* path) : ev(r, path) {}
    bool has(const std::string& body) {
        for (std::string& b : ev.take()) all.push_back(b);
        for (const std::string& b : all) {
            if (b == body) return true;
        }
        return false;
    }
};

TEST(peers_one_node_two_routes_in_order) {
    PeersRig r;
    r.peers.order("nowtp, udp");
    r.start();
    r.discoverOverNowTp();
    r.run(1500);
    // One node for the device, whichever transport announced it first.
    CHECK(tesserkit::findNode(r.dev.api(), "/peers/lamp-f37c") != nullptr);
    CHECK(tesserkit::findNode(r.dev.api(), "/peers/lamp-f37c-75f37c") == nullptr);
    CHECK_EQ(r.status("id"), "\"10521c75f37c\"");
    CHECK_EQ(r.status("name"), "\"lamp-f37c\"");
    CHECK_EQ(r.status("online"), "true");
    CHECK_EQ(r.status("route"), "\"nowtp\"");
    CHECK_EQ(r.status("routes"), "\"nowtp, udp\"");
    CHECK(r.status("heard") != "-1");
    // Requests go the preferred way, and time their round trip.
    auto g = r.fetch("/peers/lamp-f37c/lamp/on");
    r.pumpAll(200);
    CHECK(g->done);
    CHECK_EQ(g->status, Status::Ok);
    CHECK_EQ(g->body, "false");
    r.run(1000);
    CHECK_EQ(r.status("route"), "\"nowtp\"");
    CHECK(r.status("rtt") != "0");

    // The default order at run time, then one device's own.
    CHECK_EQ(r.set("/sys/peers/order", "\"udp, nowtp\"").status, Status::Ok);
    r.run(1000);
    CHECK_EQ(r.status("route"), "\"udp\"");
    CHECK_EQ(r.set("/sys/peers/nodes", "[{\"id\": \"10:52:1C:75:F3:7C\", \"order\": \"nowtp\"}]").status, Status::Ok);
    r.run(1000);
    CHECK_EQ(r.status("route"), "\"nowtp\"");
    CHECK_EQ(r.status("routes"), "\"nowtp, udp\"");
}

TEST(peers_settings_are_checked) {
    PeersRig r;
    r.start();
    CHECK_EQ(r.set("/sys/peers/order", "\"udp, ble\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/peers/order", "\"udp,udp\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/peers/order", "\"\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/peers/order", "\"udp/ethernet, nowtp, udp/wifi\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/peers/order", "\"udp/ethernet, udp\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/peers/order", "\"nowtp/ethernet\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/peers/order", "\"udp/cable\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/peers/order", "\"udp/wifi, udp/wifi\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/peers/order", "\"nowtp\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/peers/nodes", "[{\"id\": \"10521c75f3\"}]").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/peers/nodes", "[{\"id\": \"10521c75f37c\", \"address\": \"lamp\"}]").status,
             Status::InvalidValue);
    CHECK_EQ(r.set("/sys/peers/nodes", "[{\"id\": \"10521c75f37c\", \"order\": \"serial\"}]").status,
             Status::InvalidValue);
    CHECK_EQ(r.set("/sys/peers/nodes", "[{\"id\": \"10521c75f37c\", \"address\": \"192.168.1.97:8484\"}]").status,
             Status::Ok);
}

TEST(peers_failover_and_toasts) {
    PeersRig r;
    r.peers.order("udp, nowtp");
    r.start();
    r.discoverOverNowTp();
    r.run(1000);
    CHECK_EQ(r.status("route"), "\"udp\"");
    Heard reached(r, "/sys/peers/reached"), lost(r, "/sys/peers/lost");
    // UDP goes silent: the next request fails over to NowTP.
    r.udpUp = false;
    auto g = r.fetch("/peers/lamp-f37c/lamp/on");
    r.run(5000);
    CHECK(g->done);  // timed out: not sent again, it may have been applied
    CHECK_EQ(r.status("online"), "true");
    CHECK_EQ(r.status("route"), "\"nowtp\"");
    CHECK_EQ(r.status("routes"), "\"udp (offline), nowtp\"");
    CHECK(reached.has("\"lamp-f37c over nowtp\""));
    CHECK(r.lost.empty());  // still reachable: not gone
    // NowTP loses it too: offline, a toast, and the device is gone.
    r.nowUp = false;
    r.now()->forgetPeer(PeersRig::mac(kPeerMac));
    r.run(1500);
    CHECK_EQ(r.status("online"), "false");
    CHECK(lost.has("\"lamp-f37c\""));
    // Back over UDP: reached again, the preferred way.
    r.udpUp = true;
    r.run(6000);
    CHECK_EQ(r.status("online"), "true");
    CHECK_EQ(r.status("route"), "\"udp\"");
    CHECK(reached.has("\"lamp-f37c over udp\""));
}

TEST(peers_device_lost_from_every_transport) {
    PeersRig r;
    r.start();
    r.discoverOverNowTp();
    // The other node writes here over both transports, so both know it.
    CHECK_EQ(r.fromPeer(Op::Set, "/lamp/on", "true").status, Status::Ok);
    auto done = std::make_shared<bool>(false);
    r.peerNow->request(PeersRig::mac(kSelfMac), Op::Set, "/lamp/on", "false",
                       [done](Status, JsonVariantConst) { *done = true; });
    r.pumpAll();
    CHECK(*done);
    // Gone from UDP only (a goodbye): still there over NowTP.
    r.peer->stop();
    r.pumpAll(500);
    CHECK(r.lost.empty());
    // Gone from NowTP too.
    r.now()->forgetPeer(PeersRig::mac(kPeerMac));
    r.pumpAll(500);
    CHECK_EQ(r.lost.size(), 1u);
    if (!r.lost.empty()) CHECK_EQ(r.lost[0], std::string(1, char(0x7c)) + "lost");
}

// NowTP can't authenticate the node (another pairing key): a toast that says
// so, though UDP still reaches it, and the device isn't counted as gone.
TEST(peers_wrong_pairing_key_is_told) {
    PeersRig r;
    r.start();
    r.discoverOverNowTp();
    r.run(1000);
    Heard lost(r, "/sys/peers/lost");
    r.now()->forgetPeer(PeersRig::mac(kPeerMac), tesser::PeerLoss::AuthFailed);
    r.run(500);
    CHECK(lost.has("\"lamp-f37c: wrong pairing key\""));
    CHECK_EQ(r.status("online"), "true");  // over UDP
    CHECK(r.lost.empty());
}

// A short heap skips rebuilding the status rather than aborting in the
// device's task (DEVKIT-V3 as a gateway, 2026-10-15): the last one stands,
// and the next period catches up once there is room.
TEST(peers_status_waits_for_memory) {
    PeersRig r;
    r.peers.order("udp, nowtp");
    r.start();
    r.discoverOverNowTp();
    r.run(1000);
    // UDP goes silent: a request times out, NowTP carries on.
    r.udpUp = false;
    auto g = r.fetch("/peers/lamp-f37c/lamp/on");
    r.run(5000);
    CHECK_EQ(r.status("route"), "\"nowtp\"");
    {
        rig::BudgetLimit low(64);
        r.nowUp = false;
        r.now()->forgetPeer(PeersRig::mac(kPeerMac));  // offline on every route
        r.run(3000);
        CHECK_EQ(r.status("online"), "true");  // not rebuilt
    }
    r.run(1500);
    CHECK_EQ(r.status("online"), "false");
}

// The status is updated in place each second; "heard" and "rtt" move most
// seconds and notify the list (whole, to every subscriber) only when they
// move far: past 5 s, 30 s, 2 min, 10 min since heard, and half the round
// trip shown.
TEST(peers_status_notifies_on_change_not_every_second) {
    PeersRig r;
    r.peers.order("nowtp, udp");
    r.start();
    r.discoverOverNowTp();
    r.run(1500);
    const tesser::Node* list = tesserkit::findNode(r.dev.api(), "/sys/peers/status");
    CHECK(list != nullptr);
    if (!list) return;
    // Heard as usual: nothing to tell.
    uint32_t gen = list->generation();
    r.run(3000);
    CHECK_EQ(list->generation(), gen);
    // Silent: "heard" counts up, read as it is, and notifies past 5 s only.
    r.udpUp = r.nowUp = false;
    int changes = 0;
    int32_t lastHeard = 0;
    for (int i = 0; i < 8; i++) {
        r.run(1000);
        if (list->generation() != gen) changes++;
        gen = list->generation();
        lastHeard = atoi(r.status("heard").c_str());
    }
    CHECK(lastHeard >= 7);
    CHECK_EQ(r.status("online"), "true");  // no request failed: still thought online
    CHECK_EQ(changes, 1);
}

// Each device's Peers hooks its own Api: a second one started (and gone)
// in the same process doesn't take the first's node events.
TEST(peers_hooks_are_per_device) {
    PeersRig r;
    r.peers.order("udp, nowtp");
    r.start();
    r.discoverOverNowTp();
    r.run(1000);
    {
        PeersRig other;
        other.start();
        other.run(200);
    }
    Heard lost(r, "/sys/peers/lost");
    r.udpUp = false;
    auto g = r.fetch("/peers/lamp-f37c/lamp/on");
    r.run(5000);
    r.nowUp = false;
    r.now()->forgetPeer(PeersRig::mac(kPeerMac));
    r.run(1500);
    CHECK_EQ(r.status("online"), "false");
    CHECK(lost.has("\"lamp-f37c\""));
}

TEST(peers_device_restarted_counts_as_lost) {
    PeersRig r;
    r.start();
    r.discoverOverNowTp();
    CHECK_EQ(r.fromPeer(Op::Set, "/lamp/on", "true").status, Status::Ok);
    // It restarts: a new session over UDP, while NowTP still has it.
    r.peer->start(0x9100);
    r.peer->advertise();
    r.pumpAll(500);
    CHECK_EQ(r.lost.size(), 1u);
    if (!r.lost.empty()) CHECK_EQ(r.lost[0], std::string(1, char(0x7c)) + "restarted");
}

TEST(peers_declared_by_address) {
    PeersRig r;
    r.start();
    CHECK_EQ(r.set("/sys/peers/nodes",
                   "[{\"id\": \"aabbccddeeff\", \"name\": \"far\", \"address\": \"10.0.2.7\"},"
                   " {\"id\": \"aabbccdd0011\", \"address\": \"10.0.2.8:9000\"}]")
                 .status,
             Status::Ok);
    r.run(1000);
    CHECK(tesserkit::findNode(r.dev.api(), "/peers/far") != nullptr);
    CHECK(tesserkit::findNode(r.dev.api(), "/peers/node-dd0011") != nullptr);
    rig::Reply s = r.schema("/peers");
    CHECK_EQ(at(s.body, "children/far/id"), "\"aabbccddeeff\"");
    rig::Reply st = r.get("/sys/peers/status");
    CHECK(st.body.find("\"aabbccddeeff\"") != std::string::npos);
    // A device already found gets a UDP route rather than a second node.
    r.discoverOverNowTp();
    CHECK_EQ(r.set("/sys/peers/nodes", "[{\"id\": \"10521c75f37c\", \"address\": \"192.168.1.97\"}]").status,
             Status::Ok);
    r.run(1000);
    CHECK(tesserkit::findNode(r.dev.api(), "/peers/node-75f37c") == nullptr);
}

TEST(peers_max_calls_reach_the_endpoints) {
    PeersRig r;
    r.udp.maxCalls(24);
    r.nowtp.maxCalls(12);
    r.start();
    CHECK_EQ(r.fake.udpApi.maxCalls, 24u);
    CHECK_EQ(r.fake.nowtp.config.maxCalls, 12u);
}

// The other node with accounts: a login like Auth's, tokens it can revoke,
// and a node only an admin may write.
struct AccountsRig : PeersRig {
    int token = 1;  // the valid one is "tok-<token>"
    int logins = 0, wrongLogins = 0;
    bool secret = false;

    AccountsRig() {
        peerApi.object("secret").value("on", secret).writeAccess(tesser::Access::Admin);
        auto& a = peerApi.object("sys").object("auth");
        a.action("login", [this](const tesserkit::Credentials& c, tesser::Call& call) {
            logins++;
            if (c.name != "show" || c.password != "pw") {
                wrongLogins++;
                return call.fail(Status::Unauthorized, "wrong name or password");
            }
            std::string t = "tok-" + std::to_string(token);
            call.replyWith([&t](tesser::JsonWriter& w) {
                w.beginObject();
                w.key("token");
                w.string(t);
                w.key("access");
                w.string("admin");
                w.endObject();
            });
        });
        peerApi.authenticate([this](std::string_view t) {
            return t == "tok-" + std::to_string(token) ? tesser::Access::Admin : tesser::Access::Public;
        });
    }
    Status writeSecret() {
        auto out = std::make_shared<rig::Reply>();
        admin.request(Op::Set, "/peers/lamp-f37c/secret/on", "true", [out](Status st, JsonVariantConst) {
            out->status = st;
            out->done = true;
        });
        pumpAll(300);
        return out->done ? out->status : Status::Timeout;
    }
};

TEST(peers_sign_in_with_an_account) {
    AccountsRig r;
    r.start();
    r.run(1000);
    CHECK_EQ(r.writeSecret(), Status::Unauthorized);  // peerAccess only
    CHECK_EQ(r.status("signIn"), "\"\"");
    CHECK_EQ(r.set("/sys/peers/account", "{\"name\": \"show\", \"password\": \"pw\"}").status, Status::Ok);
    CHECK_EQ(r.value("/sys/peers/account/password"), "null");  // a secret
    r.run(1000);
    CHECK_EQ(r.logins, 1);
    CHECK_EQ(r.status("signIn"), "\"signed in as show\"");
    CHECK_EQ(r.writeSecret(), Status::Ok);
    r.run(1000);
    CHECK_EQ(r.status("access"), "\"admin\"");
    CHECK(r.secret);

    // The node revokes the token: refused, signed in again (not at once:
    // the token was given less than 30 s ago).
    r.token = 2;
    r.peer->endpoint().forgetPeer(r.self);  // it forgets this device's token too
    CHECK_EQ(r.writeSecret(), Status::Unauthorized);
    r.run(2000);
    CHECK_EQ(r.status("signIn"), "\"token refused\"");
    r.run(30000);
    CHECK_EQ(r.logins, 2);
    CHECK_EQ(r.status("signIn"), "\"signed in as show\"");
    CHECK_EQ(r.writeSecret(), Status::Ok);
}

TEST(peers_wrong_password_isnt_retried) {
    AccountsRig r;
    r.peers.account("show", "nope");
    r.start();
    r.run(1000);
    CHECK_EQ(r.status("signIn"), "\"wrong name or password\"");
    r.run(65000);
    CHECK_EQ(r.wrongLogins, 1);  // the node would lock sign-ins out
    // The node's own account, set for it: tried at once.
    CHECK_EQ(r.set("/sys/peers/nodes", "[{\"id\": \"10521c75f37c\", \"user\": \"show\", \"password\": \"pw\"}]")
                 .status,
             Status::Ok);
    r.run(1000);
    CHECK_EQ(r.status("signIn"), "\"signed in as show\"");
    CHECK_EQ(r.writeSecret(), Status::Ok);
    // No account any more: the token goes.
    CHECK_EQ(r.set("/sys/peers/nodes", "[]").status, Status::Ok);
    CHECK_EQ(r.set("/sys/peers/account", "{\"name\": \"\"}").status, Status::Ok);
    r.run(1000);
    CHECK_EQ(r.status("signIn"), "\"\"");
    CHECK_EQ(r.status("access"), "\"\"");
}

}  // namespace peers_cases
