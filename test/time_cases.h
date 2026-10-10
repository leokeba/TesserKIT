// Time (docs/DESIGN.md section 5.3).
#pragma once

#include "rig.h"

namespace time_cases {

using rig::at;
using rig::Status;
using tesserkit::TimeSource;

struct TimeRig : rig::Rig {
    tesserkit::Time time{dev};
};

constexpr int64_t kJuly = 1782907200000;    // 2026-07-01T12:00:00Z
constexpr int64_t kJanuary = 1767268800000;  // 2026-01-01T12:00:00Z

TEST(time_iso8601_follows_the_time_zone) {
    TimeRig r;
    r.time.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
    CHECK(r.dev.begin());
    CHECK_EQ(r.fake.clock.tz, "CET-1CEST,M3.5.0,M10.5.0/3");
    CHECK_EQ(tesserkit::Time::iso8601(kJuly), "2026-07-01T14:00:00+02:00");
    CHECK_EQ(tesserkit::Time::iso8601(kJanuary), "2026-01-01T13:00:00+01:00");
    CHECK_EQ(r.set("/sys/time/tz", "\"<-0330>3:30\"").status, Status::Ok);
    r.step();
    CHECK_EQ(tesserkit::Time::iso8601(kJanuary), "2026-01-01T08:30:00-03:30");
    CHECK_EQ(r.set("/sys/time/tz", "\"\"").status, Status::InvalidValue);
    r.set("/sys/time/tz", "\"UTC0\"");
    r.step();
    r.fake.clock.epoch = kJuly + 1500;
    CHECK_EQ(r.value("/sys/time/now"), "\"2026-07-01T12:00:01+00:00\"");
    CHECK_EQ(r.value("/sys/time/epoch"), "1782907201");
}

TEST(time_sntp_sync) {
    TimeRig r;
    int synced = 0;
    r.dev.onTimeSynced([&synced] { synced++; });
    CHECK(r.dev.begin());
    CHECK(r.fake.clock.sntpOn);
    CHECK_EQ(r.fake.clock.server, "pool.ntp.org");
    CHECK_EQ(r.value("/sys/time/synced"), "false");
    CHECK_EQ(r.value("/sys/time/source"), "\"none\"");
    r.fake.clock.sync(kJuly);
    r.step();
    CHECK_EQ(r.value("/sys/time/synced"), "true");
    CHECK_EQ(r.value("/sys/time/source"), "\"sntp\"");
    r.fake.clock.sync(kJuly + 3600000);
    r.step();
    CHECK_EQ(synced, 1);  // once per boot
}

TEST(time_sntp_asks_again_when_online) {
    struct R : rig::Rig {
        tesserkit::Network net{dev};
        tesserkit::Time time{dev};
    } r;
    fake::Air a;
    a.ssid = "Home";
    a.password = "pw";
    r.fake.wifi.air.push_back(a);
    r.net.defaultNetwork("Home", "pw");
    r.fake.wifi.connectSilent = true;
    CHECK(r.dev.begin());
    r.run(1000);
    int calls = r.fake.clock.sntpCalls;
    r.fake.wifi.connectSilent = false;
    r.run(12000);  // the silent attempt times out, the next one joins
    CHECK(r.dev.state() == tesserkit::State::Online);
    CHECK_EQ(r.fake.clock.sntpCalls, calls + 1);
}

TEST(time_sntp_settings) {
    TimeRig r;
    r.time.defaultServer("time.example.com");
    CHECK(r.dev.begin());
    CHECK_EQ(r.fake.clock.server, "time.example.com");
    CHECK_EQ(r.set("/sys/time/sntp", "{\"enabled\":false}").status, Status::Ok);
    r.step();
    CHECK(!r.fake.clock.sntpOn);
    CHECK_EQ(r.set("/sys/time/sntp", "{\"enabled\":true,\"server\":\"10.0.0.1\"}").status, Status::Ok);
    r.step();
    CHECK(r.fake.clock.sntpOn);
    CHECK_EQ(r.fake.clock.server, "10.0.0.1");
}

TEST(time_set_from_the_browser) {
    TimeRig r;
    int synced = 0;
    r.dev.onTimeSynced([&synced] { synced++; });
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/time/set", "1000").status, Status::InvalidValue);  // 1970
    CHECK_EQ(r.set("/sys/time/set", "1782907200000").status, Status::Ok);
    CHECK_EQ(r.fake.clock.epoch, kJuly);
    CHECK_EQ(r.value("/sys/time/source"), "\"set\"");
    CHECK_EQ(synced, 1);
    // SNTP, still running, wins over it.
    r.fake.clock.sync(kJuly + 5000);
    r.step();
    CHECK_EQ(r.value("/sys/time/source"), "\"sntp\"");
    CHECK_EQ(r.request(r.anon, rig::Op::Set, "/sys/time/set", "1782907200000").status, Status::Unauthorized);
    rig::Reply s = r.schema("/sys/time/set");
    CHECK_EQ(at(s.body, "ui/value"), "\"$now\"");
    CHECK_EQ(at(s.body, "arg"), "\"integer\"");
}

TEST(time_external_source_wins) {
    TimeRig r;
    CHECK(r.dev.begin());
    r.time.external(kJuly);
    CHECK_EQ(r.fake.clock.epoch, kJuly);
    CHECK(!r.fake.clock.sntpOn);  // stopped: it doesn't override GPS
    CHECK_EQ(r.time.source(), TimeSource::External);
    r.fake.clock.sync(kJanuary);  // a late answer
    r.step();
    CHECK_EQ(r.time.source(), TimeSource::External);
    // Toggling SNTP doesn't restart it either.
    r.set("/sys/time/sntp/enabled", "false");
    r.set("/sys/time/sntp/enabled", "true");
    r.step();
    CHECK(!r.fake.clock.sntpOn);
}

TEST(time_schema_hints) {
    TimeRig r;
    CHECK(r.dev.begin());
    rig::Reply s = r.schema("/sys/time");
    CHECK_EQ(at(s.body, "children/tz/ui/widget"), "\"timezone\"");
    CHECK_EQ(at(s.body, "children/tz/access"), "\"admin\"");
    CHECK_EQ(at(s.body, "children/tz/persist"), "true");
    CHECK_EQ(at(s.body, "children/source/enum"), "[\"none\",\"sntp\",\"set\",\"external\"]");
}

}  // namespace time_cases
