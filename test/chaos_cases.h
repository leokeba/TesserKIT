// The services under chaos (fake::Chaos): platform events late, out of order
// or lost, and datagrams too. Each case plays a random scenario per seed,
// calms the chaos, and checks that the service's state converged on the
// platform's.
#pragma once

#include <stdio.h>
#include <stdlib.h>

#include <random>

#include "mqtt_cases.h"
#include "network_cases.h"
#include "peers_cases.h"
#include "rig.h"

namespace chaos_cases {

using rig::Status;
using tesserkit::Event;

// Seeds per case (CHAOS_SEEDS=300 for a longer run), and the chaos's
// percentages (CHAOS_DROP, CHAOS_DELAY).
inline uint32_t env(const char* name, uint32_t fallback) {
    const char* v = getenv(name);
    return v && *v ? static_cast<uint32_t>(atoi(v)) : fallback;
}
inline uint32_t seeds(uint32_t base) { return env("CHAOS_SEEDS", base); }
inline void startChaos(fake::Chaos& c, uint32_t seed, uint32_t maxDelayMs = 3000) {
    c.start(seed, env("CHAOS_DROP", 10), env("CHAOS_DELAY", 30), maxDelayMs);
}

// Says which seed failed, once per case.
struct SeedReport {
    uint32_t seed;
    int before = check::g_caseFailures;
    explicit SeedReport(uint32_t s) : seed(s) {}
    ~SeedReport() {
        if (check::g_caseFailures != before) printf("  (seed %u)\n", static_cast<unsigned>(seed));
    }
};

// Wi-Fi only: the router drops the station, goes away and comes back,
// phones join the access point, while events are lost or late.
TEST(chaos_network_wifi_converges) {
    for (uint32_t seed = 1; seed <= seeds(40); seed++) {
        SeedReport report(seed);
        network_cases::NetRig r;
        r.air("Home", "pw");
        r.net.defaultNetwork("Home", "pw");
        startChaos(r.fake.chaos, seed);
        std::mt19937 rng(seed * 7919);
        CHECK(r.dev.begin());
        for (int i = 0; i < 25; i++) {
            switch (rng() % 5) {
                case 0: r.fake.wifi.drop(); break;
                case 1: r.fake.wifi.apStations(static_cast<int32_t>(rng() % 3)); break;
                case 2: r.fake.wifi.air.clear(); break;  // the router is off
                case 3: r.air("Home", "pw"); break;      // and on again
                default: break;
            }
            r.run(static_cast<uint32_t>(rng() % 4000));
        }
        // Calm: the router on, no phone.
        r.fake.chaos.stop();
        r.fake.wifi.air.clear();
        r.air("Home", "pw");
        r.fake.wifi.apStations(0);
        r.run(90000);  // the longest backoff (30 s), an attempt (30 s), the AP's 30 s
        CHECK(!r.fake.chaos.holding());
        CHECK(r.fake.wifi.linked);
        CHECK(r.net.connected());
        CHECK_EQ(r.value("/sys/network/wifi/status/connected"), "true");
        CHECK_EQ(r.value("/sys/network/wifi/status/ip"), "\"192.168.1.50\"");
        CHECK_EQ(r.value("/sys/network/wifi/status/state"), "\"connected\"");
        CHECK(r.dev.state() == tesserkit::State::Online);
        CHECK(!r.fake.wifi.apUp);  // fallback: down 30 s after the station joined
        CHECK_EQ(r.value("/sys/network/ap/status/active"), "false");
        CHECK_EQ(r.value("/sys/network/ap/status/clients"), "0");
        if (check::g_caseFailures) return;
    }
}

// Ethernet beside Wi-Fi: the cable comes and goes, its DHCP server too.
TEST(chaos_network_ethernet_converges) {
    for (uint32_t seed = 1; seed <= seeds(40); seed++) {
        SeedReport report(seed);
        network_cases::EthRig r;
        r.air("Home", "pw");
        r.net.defaultNetwork("Home", "pw");
        startChaos(r.fake.chaos, seed);
        std::mt19937 rng(seed * 104729);
        CHECK(r.dev.begin());
        for (int i = 0; i < 25; i++) {
            switch (rng() % 5) {
                case 0: r.fake.ethernet.plug(); break;
                case 1: r.fake.ethernet.unplug(); break;
                case 2:
                    if (r.fake.ethernet.dhcp) {
                        r.fake.ethernet.dhcp = false;
                    } else {
                        r.fake.ethernet.serveDhcp();
                    }
                    break;
                case 3: r.fake.wifi.drop(); break;
                default: break;
            }
            r.run(static_cast<uint32_t>(rng() % 4000));
        }
        // Calm: the cable in, with DHCP.
        r.fake.chaos.stop();
        r.fake.ethernet.serveDhcp();
        if (!r.fake.ethernet.state.link) r.fake.ethernet.plug();
        r.run(90000);
        CHECK(!r.fake.chaos.holding());
        CHECK(r.net.ethernetConnected());
        CHECK_EQ(r.value("/sys/network/ethernet/status/ip"), "\"192.168.1.60\"");
        CHECK_EQ(r.value("/sys/network/ethernet/status/state"), "\"connected\"");
        CHECK_EQ(r.value("/sys/network/ethernet/status/link"), "true");
        CHECK_EQ(r.dev.address(), 0xC0A8013Cu);
        // fallback: the station keeps away while Ethernet has a lease.
        CHECK(!r.fake.wifi.linked);
        CHECK_EQ(r.value("/sys/network/wifi/status/connected"), "false");
        CHECK(r.dev.state() == tesserkit::State::Online);
        if (check::g_caseFailures) return;
    }
}

// The broker accepts and drops the session, commands come in, while the
// session's events are lost or late.
TEST(chaos_mqtt_converges) {
    for (uint32_t seed = 1; seed <= seeds(40); seed++) {
        SeedReport report(seed);
        mqtt_cases::MqttRig r;
        startChaos(r.fake.chaos, seed);
        std::mt19937 rng(seed * 15485863);
        CHECK(r.dev.begin());
        r.settle();
        for (int i = 0; i < 25; i++) {
            switch (rng() % 4) {
                case 0: r.fake.mqtt.connect(); break;
                case 1: r.fake.mqtt.drop(); break;
                case 2: r.fake.mqtt.message(r.pre() + "/led/brightness/set", std::to_string(rng() % 256)); break;
                default: break;
            }
            r.run(static_cast<uint32_t>(rng() % 3000));
        }
        // Calm: connected.
        r.fake.chaos.stop();
        if (!r.fake.mqtt.linked) r.fake.mqtt.connect();
        r.run(5000);
        CHECK(!r.fake.chaos.holding());
        CHECK_EQ(r.value("/sys/mqtt/status/connected"), "true");
        CHECK_EQ(r.fake.mqtt.retained[r.pre() + "/status"].payload, "online");
        // Bridged: a change goes out, a command comes in.
        r.set("/led/on", r.on ? "false" : "true");
        r.settle();
        CHECK_EQ(r.fake.mqtt.last(r.pre() + "/led/on"), r.on ? "true" : "false");
        r.fake.mqtt.message(r.pre() + "/led/brightness/set", "77");
        r.settle();
        CHECK_EQ(r.brightness, 77);
        // And the broker's loss is seen.
        r.fake.mqtt.drop();
        r.settle();
        CHECK_EQ(r.value("/sys/mqtt/status/connected"), "false");
        if (check::g_caseFailures) return;
    }
}

// A node over UDP and NowTP, signed in with an account: links go silent and
// come back, requests go through, while datagrams are lost, late or out of
// order.
TEST(chaos_peers_converge) {
    for (uint32_t seed = 1; seed <= seeds(40) / 4; seed++) {
        SeedReport report(seed);
        peers_cases::AccountsRig r;
        r.peers.account("show", "pw");
        r.peers.order("udp, nowtp");
        r.start();
        r.discoverOverNowTp();
        r.run(1000);
        startChaos(r.fake.chaos, seed, 500);
        std::mt19937 rng(seed * 32452843);
        for (int i = 0; i < 15; i++) {
            switch (rng() % 5) {
                case 0: r.udpUp = !r.udpUp; break;
                case 1: r.nowUp = !r.nowUp; break;
                case 2: r.fetch("/peers/lamp-f37c/lamp/on"); break;
                case 3: r.writeSecret(); break;
                default: break;
            }
            r.run(500 + static_cast<uint32_t>(rng() % 3000));
        }
        // Calm: both links up.
        r.fake.chaos.stop();
        r.udpUp = r.nowUp = true;
        r.discoverOverNowTp();  // NowTP's discovery announces it again
        r.run(40000);  // a sign-in retried after 30 s at most
        CHECK_EQ(r.status("online"), "true");
        CHECK_EQ(r.status("route"), "\"udp\"");
        CHECK_EQ(r.status("routes"), "\"udp, nowtp\"");
        CHECK_EQ(r.status("signIn"), "\"signed in as show\"");
        CHECK_EQ(r.writeSecret(), Status::Ok);
        r.run(2000);  // the status's next refresh
        CHECK_EQ(r.status("access"), "\"admin\"");
        auto g = r.fetch("/peers/lamp-f37c/lamp/on");
        r.pumpAll(500);
        CHECK(g->done);
        CHECK_EQ(g->status, Status::Ok);
        if (check::g_caseFailures) return;
    }
}

}  // namespace chaos_cases
