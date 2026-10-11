// Timers (docs/DESIGN.md section 5.6).
#pragma once

#include "rig.h"

namespace timer_cases {

using rig::at;
using rig::Op;
using rig::Status;

// 2026-07-01 is a Wednesday. Local time is CET/CEST.
constexpr int64_t kWednesday0759 = 1782885540000;  // 07:59:00 CEST = 05:59 UTC

struct TimerRig : rig::Rig {
    tesserkit::System system{dev};
    tesserkit::Time time{dev};
    tesserkit::Timers timers{dev};
    bool on = false;
    int presses = 0;
    TimerRig() {
        time.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");
        auto& led = dev.api().object("led");
        led.value("on", on);
        led.action("press", [this] { presses++; });
    }
    // Moves the clock (and the device's uptime) on by `ms`, a period at a time.
    void advance(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 20) {
            fake.clock.epoch += 20;
            step(20);
        }
    }
    tesserkit::Timer timer(const char* name, const char* hhmm, const char* path, const char* value, uint8_t days = 0x7F) {
        tesserkit::Timer t;
        snprintf(t.name, sizeof(t.name), "%s", name);
        snprintf(t.time, sizeof(t.time), "%s", hhmm);
        t.path = path;
        t.value = value;
        t.days = days;
        return t;
    }
};

TEST(timers_run_at_their_minute) {
    TimerRig r;
    r.timers.add(r.timer("lights", "08:00", "/led/on", "true"));
    r.timers.add(r.timer("press", "08:00", "/led/press", "null"));
    r.fake.clock.epoch = kWednesday0759;
    CHECK(r.dev.begin());
    r.advance(59000);
    CHECK(!r.on);
    r.advance(2000);  // 08:00:01
    CHECK(r.on);
    CHECK_EQ(r.presses, 1);
    r.advance(61000);  // the rest of the minute and beyond: once
    CHECK_EQ(r.presses, 1);
    CHECK_EQ(r.value("/sys/timers/lights/last"), "\"ok\"");
}

TEST(timers_last_run_is_restored) {
    TimerRig r;
    r.timers.add(r.timer("lights", "08:00", "/led/on", "true"));
    CHECK(r.dev.begin());
    std::vector<std::string> skipped;
    CHECK_EQ(r.dev.api().restore("{\"sys\":{\"timers\":[{\"name\":\"lights\",\"last\":\"ok\"}]}}", &skipped),
             Status::Ok);
    CHECK_EQ(skipped.size(), 0u);
    CHECK_EQ(r.value("/sys/timers/lights/last"), "\"ok\"");
    CHECK_EQ(r.set("/sys/timers/lights/last", "\"no\"").status, Status::ReadOnly);
}

TEST(timers_follow_the_days) {
    TimerRig r;
    r.timers.add(r.timer("weekend", "08:00", "/led/press", "null", 0x60));   // Sat, Sun
    r.timers.add(r.timer("wednesday", "08:00", "/led/on", "true", 0x04));
    r.fake.clock.epoch = kWednesday0759;
    CHECK(r.dev.begin());
    r.advance(62000);
    CHECK_EQ(r.presses, 0);
    CHECK(r.on);
}

TEST(timers_wait_for_a_valid_time) {
    TimerRig r;
    r.timers.add(r.timer("t", "00:00", "/led/press", "null"));
    CHECK(r.dev.begin());  // the clock reads 1970
    r.advance(120000);
    CHECK_EQ(r.presses, 0);
}

TEST(timers_never_twice_in_a_minute) {
    TimerRig r;
    r.timers.add(r.timer("t", "08:00", "/led/press", "null"));
    r.fake.clock.epoch = kWednesday0759 + 60000;  // 08:00:00
    CHECK(r.dev.begin());
    r.advance(1500);
    CHECK_EQ(r.presses, 1);
    r.fake.clock.epoch -= 30000;  // a sync moves the clock back within the minute
    r.advance(30000);
    CHECK_EQ(r.presses, 1);
    // Next day, same time: again.
    r.fake.clock.epoch += 24LL * 3600 * 1000;  // 08:00:01
    r.advance(2000);
    CHECK_EQ(r.presses, 2);
}

TEST(timers_errors_are_kept) {
    TimerRig r;
    r.timers.add(r.timer("bad", "08:00", "/led/on", "\"yes\""));  // a string for a boolean
    r.fake.clock.epoch = kWednesday0759 + 60000;
    CHECK(r.dev.begin());
    r.advance(1500);
    CHECK_EQ(r.value("/sys/timers/bad/last"), "\"invalid_value: expected boolean\"");
}

TEST(timers_writes_are_checked) {
    TimerRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"a\",\"time\":\"7:00\"}]").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"a\",\"time\":\"24:00\"}]").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"a\",\"path\":\"/led/nope\"}]").status, Status::NotFound);
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"a\",\"path\":\"/led/on/x\"}]").status, Status::NotFound);
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"a\",\"value\":\"{oops\"}]").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"a\",\"days\":128}]").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"a\",\"time\":\"06:30\",\"path\":\"/led/on\",\"value\":\"false\",\"days\":31}]").status,
             Status::Ok);
    CHECK_EQ(r.set("/sys/timers/a/last", "\"forged\"").status, Status::ReadOnly);
    CHECK_EQ(r.request(r.anon, Op::Set, "/sys/timers", "[]").status, Status::Unauthorized);
    // Paths below lists, arrays and remote nodes can't be checked: accepted.
    CHECK(r.timers.resolves("/sys/timers/a/enabled"));
    CHECK(!r.timers.resolves("/"));
    CHECK(!r.timers.resolves("/led//on"));
    rig::Reply s = r.schema("/sys/timers");
    CHECK_EQ(at(s.body, "items/children/days/ui/widget"), "\"flags\"");
    CHECK_EQ(at(s.body, "items/children/days/ui/labels"), "\"Mon,Tue,Wed,Thu,Fri,Sat,Sun\"");
}

TEST(timers_refused_writes_change_nothing) {
    TimerRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"keep\",\"path\":\"/led/on\",\"value\":\"true\"}]").status, Status::Ok);
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"bad\",\"path\":\"/led/nope\"}]").status, Status::NotFound);
    CHECK_EQ(at(r.value("/sys/timers"), "0/name"), "\"keep\"");
    CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"keep\",\"time\":\"25:00\"}]").status, Status::InvalidValue);
    CHECK_EQ(at(r.value("/sys/timers"), "0/path"), "\"/led/on\"");
}

TEST(timers_catch_up_at_boot) {
    {
        TimerRig r;
        r.timers.add(r.timer("morning", "07:00", "/led/on", "true"));
        r.timers.add(r.timer("later", "09:00", "/led/press", "null"));
        r.fake.clock.epoch = kWednesday0759 + 60000;  // 08:00
        CHECK(r.dev.begin());
        r.advance(2000);
        CHECK(!r.on);  // off by default
    }
    TimerRig r;
    r.timers.add(r.timer("morning", "07:00", "/led/on", "true"));
    r.timers.add(r.timer("later", "09:00", "/led/press", "null"));
    r.timers.catchUp(true);
    r.fake.clock.epoch = kWednesday0759 + 60000;
    CHECK(r.dev.begin());
    r.advance(2000);
    CHECK(r.on);
    CHECK_EQ(r.presses, 0);  // not missed yet
    r.advance(65000);
    CHECK_EQ(r.presses, 0);  // caught up once, at boot only
}

TEST(timers_persist_whatever_the_tree_at_boot) {
    std::map<std::string, std::string> saved;
    {
        TimerRig r;
        CHECK(r.dev.begin());
        CHECK_EQ(r.set("/sys/timers", "[{\"name\":\"a\",\"path\":\"/led/on\",\"value\":\"true\"}]").status, Status::Ok);
        r.run(2500);
        saved = r.storage.records;
    }
    // A firmware whose /led appears after begin(): the timer is kept.
    struct Late : rig::Rig {
        tesserkit::Timers timers{dev};
    } r;
    r.storage.records = saved;
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/timers/a/path"), "\"/led/on\"");
}

TEST(timers_failures_are_events) {
    TimerRig r;
    r.timers.add(r.timer("bad", "08:00", "/led/on", "\"yes\""));
    r.timers.add(r.timer("good", "08:00", "/led/on", "true"));
    r.fake.clock.epoch = kWednesday0759;
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/timerFailed");
    CHECK_EQ(at(r.schema("/sys/timerFailed").body, "ui/toast"), "\"warn\"");
    r.advance(61000);
    CHECK(r.on);
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"bad: invalid_value: expected boolean\"");
}

}  // namespace timer_cases
