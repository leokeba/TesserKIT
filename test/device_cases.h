// Device and System (docs/DESIGN.md sections 4 and 5.1).
#pragma once

#include "rig.h"

namespace device_cases {

using rig::at;
using rig::Status;

struct SysRig : rig::Rig {
    tesserkit::System system{dev};
    explicit SysRig(bool coredump = true) : Rig(coredump) {}
};

// Globals of two translation units: a service constructed before its
// Device's constructor runs is still mounted, in construction order, and
// Files is found before the services that look for it attach.
TEST(device_services_constructed_before_the_device) {
    alignas(tesserkit::Device) unsigned char raw[sizeof(tesserkit::Device)] = {};  // zero, as static storage
    tesserkit::Device& early = *reinterpret_cast<tesserkit::Device*>(raw);
    auto system = std::make_unique<tesserkit::System>(early);
    auto timers = std::make_unique<tesserkit::Timers>(early);
    tesserkit::Device* dev = new (raw) tesserkit::Device("Test Device");
    {
        fake::Platform fake;
        tesser::MemoryStorage storage;
        tesserkit::Firmware late{*dev};
        fake.install(*dev, false);
        dev->task(false).storage(storage);
        CHECK(dev->begin());
        CHECK(tesserkit::resolves(dev->api(), "/sys/timers"));
        CHECK(tesserkit::resolves(dev->api(), "/sys/firmware"));
        const tesser::Node* sys = dev->api().first();
        while (sys && std::string(sys->name()) != "sys") sys = sys->next();
        std::vector<std::string> order;
        if (sys) {
            for (const tesser::Node* c = static_cast<const tesser::Object*>(sys)->first(); c; c = c->next())
                order.push_back(c->name());
        }
        auto pos = [&order](const char* n) { return std::find(order.begin(), order.end(), n) - order.begin(); };
        CHECK(pos("timers") < pos("firmware"));
        system.reset();
        timers.reset();
    }
    dev->~Device();
}

TEST(device_hostname_from_label_and_mac) {
    SysRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.dev.hostname(), "test-device-a67c");
    CHECK_EQ(r.dev.version(), "1.0.0");  // the app description's
}

TEST(device_hostname_option_and_odd_labels) {
    {
        SysRig r;
        r.dev.hostname("helio").version("2.0.0");
        CHECK(r.dev.begin());
        CHECK_EQ(r.dev.hostname(), "helio");
        CHECK_EQ(r.dev.version(), "2.0.0");
    }
    {
        fake::Platform f;
        tesserkit::Device d("  Héliostat #3 -- Nord ");
        f.install(d);
        d.task(false);
        CHECK(d.begin());
        CHECK_EQ(d.hostname(), "h-liostat-3-nord-a67c");
    }
}

TEST(device_without_platform_fails) {
    tesserkit::Device d("Bare");
    d.task(false);
    CHECK(!d.begin());
}

TEST(device_sys_tree) {
    SysRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/"), "{\"sys\":{\"status\":{\"state\":\"online\",\"uptime\":1,\"heap\":180000,\"minHeap\":150000,\"maxBlock\":110000,"
                           "\"resetReason\":\"power-on\",\"chip\":\"ESP32-D0WD-V3 rev 3.0, 2 cores, 240 MHz\","
                           "\"flash\":4194304,\"appSize\":900000,\"appSlot\":2031616,\"idf\":\"v5.4\","
                           "\"mac\":\"c8:f0:9e:f4:a6:7c\"},\"coredumpPresent\":false}}");
    rig::Reply s = r.schema("/");
    CHECK_EQ(at(s.body, "label"), "\"Test Device\"");
    CHECK_EQ(at(s.body, "children/sys/ui/page"), "true");
    CHECK_EQ(at(s.body, "children/sys/children/restart/access"), "\"admin\"");
    CHECK_EQ(at(s.body, "children/sys/children/restart/ui/style"), "\"danger\"");
    CHECK_EQ(at(s.body, "children/sys/children/status/children/heap/ui/widget"), "\"chart\"");
    CHECK_EQ(at(s.body, "children/sys/children/coredump/type"), "\"file\"");
}

TEST(device_optional_status_values) {
    SysRig r(false);
    r.fake.core.boot.psram = true;
    r.fake.core.psram = 4000000;
    r.fake.core.hasSensor = true;
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/status/psram"), "4000000");
    CHECK_EQ(r.value("/sys/status/temperature"), "41.5");
    CHECK_EQ(r.get("/sys/coredump").status, Status::NotFound);  // no core dump partition
}

TEST(device_restart_after_the_reply) {
    SysRig r;
    CHECK(r.dev.begin());
    rig::Reply rep = r.set("/sys/restart");
    CHECK_EQ(rep.status, Status::Ok);
    r.run(400);
    CHECK_EQ(r.fake.core.restarts, 0);
    r.run(200);
    CHECK_EQ(r.fake.core.restarts, 1);
}

TEST(device_is_open_without_auth) {
    SysRig r;
    CHECK(r.dev.begin());
    // HTTP and WebSocket clients, with a token or without, are Admin.
    CHECK_EQ(r.dev.api().tokenAccess(""), tesser::Access::Admin);
    CHECK_EQ(r.dev.api().tokenAccess("anything"), tesser::Access::Admin);
}

TEST(device_actions_need_admin) {
    SysRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.request(r.anon, rig::Op::Set, "/sys/restart").status, Status::Unauthorized);
    CHECK_EQ(r.request(r.anon, rig::Op::Get, "/sys/status/heap").status, Status::Ok);
    rig::Reply s = r.request(r.anon, rig::Op::Get, "/sys", {}, [] {
        tesser::Query q;
        q.depth = 0;
        return q;
    }());
    CHECK_EQ(s.status, Status::Ok);
    r.run(1000);
    CHECK_EQ(r.fake.core.restarts, 0);
}

struct TimeRig : SysRig {
    tesserkit::Time time{dev};
};

TEST(device_settings_persist_across_boots) {
    std::map<std::string, std::string> saved;
    {
        TimeRig r;
        CHECK(r.dev.begin());
        CHECK_EQ(r.set("/sys/time/tz", "\"CET-1CEST,M3.5.0,M10.5.0/3\"").status, Status::Ok);
        r.run(1500);
        CHECK(r.storage.records.empty());  // debounced
        r.run(1000);
        CHECK_EQ(r.storage.records.count("sys"), 1u);
        saved = r.storage.records;
    }
    TimeRig r;
    r.storage.records = saved;
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/time/tz"), "\"CET-1CEST,M3.5.0,M10.5.0/3\"");
    CHECK_EQ(r.fake.clock.tz, "CET-1CEST,M3.5.0,M10.5.0/3");
}

TEST(device_restart_saves_pending_changes) {
    TimeRig r;
    CHECK(r.dev.begin());
    r.set("/sys/time/tz", "\"EST5EDT\"");
    r.set("/sys/restart");
    r.run(600);
    CHECK_EQ(r.fake.core.restarts, 1);
    CHECK(r.storage.records["sys"].find("EST5EDT") != std::string::npos);
}

TEST(device_factory_reset) {
    TimeRig r;
    CHECK(r.dev.begin());
    r.set("/sys/time/tz", "\"EST5EDT\"");
    r.run(2500);
    CHECK_EQ(r.storage.records.size(), 1u);
    CHECK_EQ(r.set("/sys/factoryReset").status, Status::Ok);
    CHECK(r.storage.records.empty());
    r.set("/sys/time/tz", "\"PST8PDT\"");  // too late: nothing is saved any more
    r.run(3000);
    CHECK(r.storage.records.empty());
    CHECK_EQ(r.fake.core.restarts, 1);
}

TEST(device_state_callbacks) {
    SysRig r;
    std::vector<tesserkit::State> seen;
    r.dev.onState([&seen](tesserkit::State s) { seen.push_back(s); });
    int ticks = 0;
    r.dev.onTick([&ticks](uint32_t) { ticks++; });
    CHECK(r.dev.begin());
    CHECK_EQ(r.dev.state(), tesserkit::State::Online);  // no Network: serving is online
    r.dev.fail(true);
    r.step();
    r.dev.fail(false);
    r.step();
    CHECK_EQ(seen.size(), 2u);
    if (seen.size() == 2) {
        CHECK_EQ(seen[0], tesserkit::State::Error);
        CHECK_EQ(seen[1], tesserkit::State::Online);
    }
    CHECK_EQ(ticks, 2);
    CHECK_EQ(r.value("/sys/status/state"), "\"online\"");
}

TEST(device_coredump) {
    SysRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.download("/sys/coredump").status, Status::NotFound);
    r.fake.coredump.data = std::string(3000, 'x');
    CHECK_EQ(r.value("/sys/coredumpPresent"), "true");
    std::string name;
    rig::Reply d = r.download("/sys/coredump", true, &name);
    CHECK_EQ(d.status, Status::Ok);
    CHECK_EQ(d.body.size(), 3000u);
    CHECK_EQ(name, "test-device-a67c-coredump.bin");
    CHECK_EQ(r.download("/sys/coredump", false).status, Status::Unauthorized);
    CHECK_EQ(r.set("/sys/clearCoredump").status, Status::Ok);
    CHECK_EQ(r.value("/sys/coredumpPresent"), "false");
}

}  // namespace device_cases
