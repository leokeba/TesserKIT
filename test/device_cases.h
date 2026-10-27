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

// Records the platform events the device hands its services.
struct Recorder : tesserkit::Service {
    std::vector<tesserkit::Event> got;
    explicit Recorder(tesserkit::Device& d) : Service(d, "recorder") {}
    void describe(tesser::Object&) override {}
    size_t count(tesserkit::Event::Type t) const {
        size_t n = 0;
        for (const tesserkit::Event& e : got) n += e.type == t ? 1 : 0;
        return n;
    }

protected:
    void event(const tesserkit::Event& e) override { got.push_back(e); }
};

tesserkit::Event event(tesserkit::Event::Type type, int32_t value = 0, std::string text = {}) {
    tesserkit::Event e;
    e.type = type;
    e.value = value;
    e.text = std::move(text);
    return e;
}

// A burst of MQTT messages while the task stalls (an OTA chunk, a sign-in):
// they keep a share of the queue, the latest ones, and every other event
// still gets through.
TEST(device_event_queue_bounds_mqtt_messages) {
    using T = tesserkit::Event::Type;
    rig::Rig r;
    Recorder rec{r.dev};
    CHECK(r.dev.begin());
    r.step();
    rec.got.clear();
    for (int i = 0; i < 40; i++) r.dev.post(event(T::MqttMessage, 1, "t" + std::to_string(i)));
    r.dev.post(event(T::StaDisconnected, 200));
    for (int i = 0; i < 5; i++) r.dev.post(event(T::PeersChanged));
    for (int i = 0; i < 5; i++) r.dev.post(event(T::NowTpPeers));
    r.dev.post(event(T::EthLink, 0));
    r.step();
    CHECK_EQ(rec.count(T::MqttMessage), size_t(8));
    CHECK_EQ(rec.count(T::StaDisconnected), size_t(1));
    CHECK_EQ(rec.count(T::EthLink), size_t(1));
    CHECK_EQ(rec.count(T::PeersChanged), size_t(1));  // one pending says it all
    CHECK_EQ(rec.count(T::NowTpPeers), size_t(1));
    if (!rec.got.empty()) CHECK_EQ(rec.got[0].text, "t32");  // the latest eight, in order
    CHECK_EQ(r.dev.mqttMessagesDropped(), 32u);

    // Full of other events: MQTT's messages give way to them, and the queue
    // still holds 32.
    rec.got.clear();
    for (int i = 0; i < 8; i++) r.dev.post(event(T::MqttMessage, 1, "m" + std::to_string(i)));
    for (int i = 0; i < 30; i++) r.dev.post(event(T::ApClients, i));
    r.dev.post(event(T::MqttMessage, 1, "late"));
    r.step();
    CHECK_EQ(rec.got.size(), size_t(32));
    CHECK_EQ(rec.count(T::ApClients), size_t(30));
    CHECK_EQ(rec.count(T::MqttMessage), size_t(2));
    if (!rec.got.empty()) CHECK_EQ(rec.got.back().text, "late");
    CHECK_EQ(r.dev.mqttMessagesDropped(), 39u);
    // Beyond 32 of them, other events are dropped as before.
    rec.got.clear();
    for (int i = 0; i < 40; i++) r.dev.post(event(T::ApClients, i));
    r.step();
    CHECK_EQ(rec.got.size(), size_t(32));
}

// The queue's room grows from TesserAPI's byte budget, in the platform's
// tasks: refused, the event is dropped (and counted) rather than the device
// aborting. Grown once, the room stays: the device's task keeps it.
TEST(device_event_queue_grows_from_the_budget) {
    using T = tesserkit::Event::Type;
    rig::Rig r;
    Recorder rec{r.dev};
    CHECK(r.dev.begin());
    r.step();
    rec.got.clear();
    uint32_t dropped = r.dev.mqttMessagesDropped();
    {
        rig::BudgetLimit limit(1);  // nothing to give
        for (int i = 0; i < 4; i++) r.dev.post(event(T::MqttMessage, 1, "x"));
    }
    r.step();
    CHECK(rec.got.size() < 4);
    CHECK(r.dev.mqttMessagesDropped() > dropped);
    // Room enough for 32, in both of the task's vectors.
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 32; i++) r.dev.post(event(T::ApClients, i));
        r.step();
    }
    rec.got.clear();
    {
        rig::BudgetLimit limit(1);
        for (int round = 0; round < 2; round++) {
            for (int i = 0; i < 32; i++) r.dev.post(event(T::ApClients, i));
            r.step();
        }
    }
    CHECK_EQ(rec.got.size(), size_t(64));  // no growth needed
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
        CHECK_EQ(d.hostname(), "heliostat-3-nord-a67c");  // accents dropped
    }
}

TEST(device_name) {
    SysRig r;
    r.dev.hostname("helio");  // the application's: until the user names it
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/name"), "\"\"");
    // Unnamed, the label and the MAC's end: nodes of one firmware differ.
    CHECK_EQ(r.dev.name(), "Test Device a67c");
    CHECK_EQ(r.dev.defaultName(), "Test Device a67c");
    CHECK_EQ(at(r.schema("/sys/name").body, "ui/placeholder"), "\"Test Device a67c\"");
    CHECK_EQ(at(r.schema("/sys/name").body, "ui/nodeName"), "true");
    CHECK_EQ(r.dev.hostname(), "helio");
    uint32_t hash = r.dev.api().schemaHash();
    uint32_t gen = r.dev.hostnameGeneration();

    // Named: the root's label at once (the schema's hash with it), and the
    // hostname.
    CHECK_EQ(r.set("/sys/name", "\"Lampe d'entrée\"").status, Status::Ok);
    CHECK_EQ(at(r.schema("/").body, "label"), "\"Lampe d'entrée\"");
    CHECK(r.dev.api().schemaHash() != hash);
    CHECK_EQ(r.dev.hostname(), "lampe-d-entree");
    CHECK(r.dev.hostnameGeneration() != gen);
    // A name without letters or digits leaves the hostname to the default.
    CHECK_EQ(r.set("/sys/name", "\"★ ★\"").status, Status::Ok);
    CHECK_EQ(r.dev.hostname(), "helio");
    // At most 32 bytes; no control characters.
    CHECK_EQ(r.set("/sys/name", "\"123456789012345678901234567890123\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/name", "\"a\\nb\"").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/name", "\"12345678901234567890123456789012\"").status, Status::Ok);
    CHECK_EQ(r.dev.hostname(), "12345678901234567890123456789012");
    // Back to the default name.
    CHECK_EQ(r.set("/sys/name", "\"\"").status, Status::Ok);
    CHECK_EQ(at(r.schema("/").body, "label"), "\"Test Device a67c\"");
    CHECK_EQ(r.dev.api().schemaHash(), hash);
    CHECK_EQ(r.dev.hostname(), "helio");
    CHECK_EQ(r.request(r.anon, rig::Op::Set, "/sys/name", "\"x\"").status, Status::Unauthorized);
}

TEST(device_name_persists) {
    std::map<std::string, std::string> saved;
    {
        SysRig r;
        CHECK(r.dev.begin());
        CHECK_EQ(r.set("/sys/name", "\"Porch\"").status, Status::Ok);
        r.run(2500);
        saved = r.storage.records;
    }
    SysRig r;
    r.storage.records = saved;
    CHECK(r.dev.begin());
    CHECK_EQ(r.dev.name(), "Porch");
    CHECK_EQ(at(r.schema("/").body, "label"), "\"Porch\"");
    CHECK_EQ(r.dev.hostname(), "porch");
}

TEST(device_without_platform_fails) {
    tesserkit::Device d("Bare");
    d.task(false);
    CHECK(!d.begin());
}

TEST(device_sys_tree) {
    SysRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/"), "{\"sys\":{\"name\":\"\",\"status\":{\"state\":\"online\",\"uptime\":1,\"heap\":180000,\"minHeap\":150000,\"maxBlock\":110000,\"settings\":\"\","
                           "\"resetReason\":\"power-on\",\"chip\":\"ESP32-D0WD-V3 rev 3.0, 2 cores, 240 MHz\","
                           "\"flash\":4194304,\"appSize\":900000,\"appSlot\":2031616,\"idf\":\"v5.4\","
                           "\"mac\":\"c8:f0:9e:f4:a6:7c\"},\"coredumpPresent\":false}}");
    rig::Reply s = r.schema("/");
    CHECK_EQ(at(s.body, "label"), "\"Test Device a67c\"");
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
    r.fake.core.minPsram = 3500000;
    r.fake.core.hasSensor = true;
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/status/psram"), "4000000");
    CHECK_EQ(r.value("/sys/status/minPsram"), "3500000");
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
        CHECK_EQ(r.storage.records.count("sys"), 0u);  // debounced (the version is written at once)
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
    CHECK_EQ(r.storage.records.size(), 2u);  // sys and @version
    // The action replies first: the device's task erases, at its next loop,
    // not the task the request came in on (a transport's).
    CHECK_EQ(r.set("/sys/factoryReset").status, Status::Ok);
    CHECK_EQ(r.storage.records.size(), 2u);
    r.step();
    CHECK(r.storage.records.empty());
    r.set("/sys/time/tz", "\"PST8PDT\"");  // too late: nothing is saved any more
    r.run(3000);
    CHECK(r.storage.records.empty());
    CHECK_EQ(r.fake.core.restarts, 1);
}

// Services hear of a factory reset in the device's task, whichever task the
// action came in on (an HTTP or NowTP one), and only then is the storage
// swapped: NowTp's forgetKey() then never races NowTP's own task.
struct ResetWatcher : tesserkit::Service {
    std::thread::id resetIn;
    int resets = 0;
    explicit ResetWatcher(tesserkit::Device& d) : Service(d, "watcher") {}
    void describe(tesser::Object&) override {}

protected:
    void factoryReset() override {
        resetIn = std::this_thread::get_id();
        resets++;
    }
};

TEST(device_factory_reset_runs_in_the_device_task) {
    SysRig r;
    ResetWatcher w{r.dev};
    CHECK(r.dev.begin());
    rig::Reply reply;
    std::thread transport([&r, &reply] { reply = r.set("/sys/factoryReset"); });
    transport.join();
    CHECK_EQ(reply.status, Status::Ok);
    CHECK_EQ(w.resets, 0);
    r.step();
    CHECK_EQ(w.resets, 1);
    CHECK(w.resetIn == std::this_thread::get_id());
    r.run(600);
    CHECK_EQ(r.fake.core.restarts, 1);
    CHECK_EQ(w.resets, 1);
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

// A service whose default changed in a later TesserKIT: speed was 3 before
// settings version 1, 5 since. Its migrate() drops the old default.
struct Demo : tesserkit::Service {
    explicit Demo(tesserkit::Device& d) : Service(d, "demo") {}
    int speed = 5;
    std::vector<uint32_t> from;  // the versions migrate() saw
    void describe(tesser::Object& o) override { o.value("speed", speed).persist(); }
    void migrate(uint32_t v, JsonObject settings) override {
        from.push_back(v);
        if (v < 1 && settings["speed"] == 3) settings.remove("speed");
    }
};

struct VersionRig : TimeRig {
    tesserkit::Backup backup{dev};
    Demo demo{dev};
    int level = 3;
    VersionRig() { dev.api().object("app").value("level", level).range(0, 10).persist(); }
};

TEST(device_settings_from_before_versions) {
    // Stored by a TesserKIT before settings versions: every default frozen
    // at first boot, an old default among them, and the user's choices.
    VersionRig r;
    r.storage.records["sys"] =
        "{\"time\":{\"tz\":\"EST5EDT\",\"sntp\":{\"enabled\":true,\"server\":\"pool.ntp.org\"}},"
        "\"backup\":{\"restartAfter\":true},\"demo\":{\"speed\":3}}";
    r.storage.records["app"] = "{\"level\":3}";
    CHECK(r.dev.begin());
    CHECK_EQ(r.demo.from.size(), 1u);
    CHECK_EQ(r.demo.from.empty() ? 99u : r.demo.from[0], 0u);
    CHECK_EQ(r.demo.speed, 5);                           // the new default
    CHECK_EQ(r.value("/sys/time/tz"), "\"EST5EDT\"");    // the user's choice survives
    // Saved again, sparse, under the version.
    CHECK_EQ(r.storage.records["@version"], std::to_string(tesserkit::Device::kSettingsVersion));
    const std::string& sys = r.storage.records["sys"];
    CHECK(sys.find("EST5EDT") != std::string::npos);
    CHECK(sys.find("pool.ntp.org") == std::string::npos);
    CHECK(sys.find("restartAfter") == std::string::npos);
    CHECK(sys.find("speed") == std::string::npos);
    CHECK_EQ(r.storage.records.count("app"), 0u);  // only defaults: no record

    // A user's value equal to nothing old survives, and state already at
    // this version isn't migrated again.
    VersionRig again;
    again.storage.records["sys"] = "{\"demo\":{\"speed\":7}}";
    CHECK(again.dev.begin());
    CHECK_EQ(again.demo.speed, 7);
    VersionRig current;
    current.storage.records = again.storage.records;
    current.storage.records["sys"] = "{\"demo\":{\"speed\":3}}";
    CHECK(current.dev.begin());
    CHECK(current.demo.from.empty());
    CHECK_EQ(current.demo.speed, 3);  // chosen under version 1: the user's
}

TEST(device_application_settings_version) {
    // The application's migration runs after TesserKIT's, for every record,
    // with the application's own stored version.
    VersionRig r;
    std::vector<std::string> seen;
    r.dev.settingsVersion(2, [&seen](std::string_view record, uint32_t version, JsonVariant value) {
        std::string s(record);
        s += "@" + std::to_string(version) + ":" + (value["demo"]["speed"].isNull() ? "-" : "speed");
        seen.push_back(s);
        if (record == "app" && version < 2 && value["level"] == 4) value.remove("level");  // was 4, now 3
        if (record == "old") return false;
        return true;
    });
    r.storage.records["sys"] = "{\"demo\":{\"speed\":3}}";
    r.storage.records["app"] = "{\"level\":4}";
    CHECK(r.dev.begin());
    std::sort(seen.begin(), seen.end());
    CHECK_EQ(seen.size(), 2u);
    seen.resize(2);
    CHECK_EQ(seen[0], "app@0:-");
    CHECK_EQ(seen[1], "sys@0:-");  // TesserKIT's migration came first
    CHECK_EQ(r.level, 3);
    CHECK_EQ(r.demo.speed, 5);
    CHECK_EQ(r.storage.records["@version"], std::to_string((2u << 16) | tesserkit::Device::kSettingsVersion));

    // Stored under version 1 of the application and TesserKIT's current one:
    // only the application's migration runs.
    VersionRig next;
    std::vector<uint32_t> versions;
    next.dev.settingsVersion(2, [&versions](std::string_view, uint32_t version, JsonVariant) {
        versions.push_back(version);
        return true;
    });
    next.storage.records["@version"] = std::to_string((1u << 16) | tesserkit::Device::kSettingsVersion);
    next.storage.records["app"] = "{\"level\":6}";
    CHECK(next.dev.begin());
    CHECK_EQ(versions.size(), 1u);
    CHECK_EQ(versions.empty() ? 99u : versions[0], 1u);
    CHECK(next.demo.from.empty());
    CHECK_EQ(next.level, 6);
}

TEST(device_backup_from_before_versions_migrates) {
    // A backup without "@version" goes through the migrations as it is
    // restored: the old default isn't taken as the user's choice.
    VersionRig r;
    r.dev.settingsVersion(1, [](std::string_view record, uint32_t version, JsonVariant value) {
        if (record == "app" && version < 1 && value["lvl"].is<int>()) {  // renamed
            value["level"] = value["lvl"];
            value.remove("lvl");
        }
        return true;
    });
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/backup/restartAfter", "false").status, Status::Ok);
    rig::Reply u = r.upload("/sys/backup/settings",
                            "{\"sys\":{\"demo\":{\"speed\":3},\"time\":{\"tz\":\"EST5EDT\"}},\"app\":{\"lvl\":8}}");
    CHECK_EQ(u.status, Status::Ok);
    CHECK_EQ(u.body, "{\"skipped\":[],\"restart\":false}");
    CHECK_EQ(r.demo.speed, 5);
    CHECK_EQ(r.level, 8);
    CHECK_EQ(r.value("/sys/time/tz"), "\"EST5EDT\"");
    CHECK_EQ(r.demo.from.size(), 1u);
    // A backup taken now carries the version, and isn't migrated again.
    rig::Reply d = r.download("/sys/backup/settings");
    CHECK_EQ(at(d.body, "@version"), std::to_string((1u << 16) | tesserkit::Device::kSettingsVersion));
    CHECK_EQ(r.upload("/sys/backup/settings", d.body).status, Status::Ok);
    CHECK_EQ(r.demo.from.size(), 1u);
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
    // Clearing it tells subscribers: the value is a getter.
    const tesser::Node* present = tesserkit::findNode(r.dev.api(), "/sys/coredumpPresent");
    CHECK(present != nullptr);
    uint32_t gen = present ? present->generation() : 0;
    CHECK_EQ(r.set("/sys/clearCoredump").status, Status::Ok);
    CHECK_EQ(r.value("/sys/coredumpPresent"), "false");
    CHECK(present && present->generation() != gen);
}

}  // namespace device_cases
