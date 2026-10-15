// Log, Battery and Sleep (docs/DESIGN.md section 5.14).
#pragma once

#include "alloc_cap.h"
#include "rig.h"

namespace power_cases {

using rig::at;
using rig::Op;
using rig::Status;

// Log.

struct LogRig : rig::Rig {
    tesserkit::Log log{dev};
};

TEST(log_lines_are_events) {
    LogRig r;
    CHECK(r.dev.begin());
    CHECK(r.fake.log.began);
    rig::Events ev(r, "/sys/log/line");
    r.fake.log.write("W (1200) wifi: lost");
    r.fake.log.write("E (1300) tesser: oops");
    r.step();
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(2));
    if (got.size() == 2) {
        CHECK_EQ(got[0], "\"W (1200) wifi: lost\"");
        CHECK_EQ(got[1], "\"E (1300) tesser: oops\"");
    }
    CHECK(!r.fake.log.muted);  // muted only while emitting
}

TEST(log_keeps_recent_lines_for_download) {
    LogRig r;
    r.log.history(64);
    CHECK(r.dev.begin());
    for (int i = 0; i < 10; i++) r.fake.log.write("I (" + std::to_string(i) + ") app: line");
    r.step();
    rig::Reply d = r.download("/sys/log/recent");
    CHECK_EQ(d.status, Status::Ok);
    // 64 bytes hold the last four lines of 15 bytes and their newlines.
    CHECK_EQ(d.body, "I (6) app: line\nI (7) app: line\nI (8) app: line\nI (9) app: line\n");
    CHECK_EQ(r.download("/sys/log/recent", false).status, Status::Unauthorized);
    CHECK_EQ(r.as(tesser::Access::User, Op::Get, "/sys/log/level").status, Status::Unauthorized);
}

// Keeping lines allocates nothing: on a short heap, a log line (often about
// that very shortage) mustn't be what aborts the device (a Gledopto did,
// 2026-10-15, in Log::keep's copy). And the ring wraps, line by line.
TEST(log_keeps_lines_without_allocating) {
    LogRig r;
    r.log.history(256);
    CHECK(r.dev.begin());
    r.step();
    // The lines wait in the fake's queue first: only the device's keeping
    // of them runs under the cap.
    for (int i = 0; i < 50; i++) r.fake.log.write("W (" + std::to_string(i) + ") app: a line of some length");
    {
        alloccap::Heap h(1024, 0);
        for (int round = 0; round < 5; round++) r.step();
        CHECK(h.overruns().empty());
        if (!h.overruns().empty()) printf("%s\n", h.report().c_str());
    }
    std::string recent = r.log.recent();
    CHECK(recent.size() <= 256);
    CHECK(recent.size() > 200);
    CHECK_EQ(recent.substr(recent.size() - 34), "W (49) app: a line of some length\n");
    CHECK_EQ(recent.find("W (0) "), std::string::npos);
    CHECK_EQ(recent.substr(0, 3), "W (");  // whole lines only
    // A line longer than the history is cut to fit.
    r.fake.log.write(std::string(400, 'x'));
    r.step();
    CHECK_EQ(r.log.recent(), std::string(255, 'x') + "\n");
}

TEST(log_level_applies_and_persists) {
    LogRig r;
    r.fake.log.lvl = 3;  // the firmware's at boot
    r.fake.log.max = 4;
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/log/level"), "\"info\"");
    CHECK_EQ(r.set("/sys/log/level", "\"debug\"").status, Status::Ok);
    r.step();
    CHECK_EQ(r.fake.log.lvl, 4);
    rig::Reply above = r.set("/sys/log/level", "\"verbose\"");
    CHECK_EQ(above.status, Status::InvalidValue);
    CHECK_EQ(r.fake.log.lvl, 4);
    r.run(2100);  // persisted
    LogRig again;
    again.storage.records = r.storage.records;
    again.fake.log.lvl = 3;
    CHECK(again.dev.begin());
    CHECK_EQ(again.fake.log.lvl, 4);
}

TEST(log_default_level_and_dropped) {
    LogRig r;
    r.log.level(tesserkit::LogLevel::Error);
    CHECK(r.dev.begin());
    CHECK_EQ(r.fake.log.lvl, 1);
    for (int i = 0; i < 400; i++) r.fake.log.write("W (1) flood: a line of some length");
    r.step();
    CHECK(r.value("/sys/log/dropped") != "0");
    // Lines are taken a few per period: the rest come with the next ones.
    rig::Events ev(r, "/sys/log/line");
    r.run(400);
    CHECK(!ev.take().empty());
}

// Battery.

struct BatteryRig : rig::Rig {
    tesserkit::Battery battery{dev};
};

TEST(battery_reads_an_adc_through_a_divider) {
    BatteryRig r;
    r.fake.pins.mv[34] = 2100;  // 4.2 V halved
    r.battery.adc(34, 2.0f);
    CHECK(r.dev.begin());
    r.step();
    CHECK_EQ(r.value("/sys/battery/soc"), "100");
    CHECK_EQ(r.value("/sys/battery/voltage"), "4.2");
    CHECK_EQ(r.get("/sys/battery/charging").status, Status::NotFound);  // nothing tells
    CHECK_EQ(at(r.schema("/sys/battery/soc").body, "ui/widget"), "\"gauge\"");
    r.fake.pins.mv[34] = 1920;  // 3.84 V: half
    r.run(40000);               // smoothed over the samples
    CHECK_EQ(r.value("/sys/battery/soc"), "50");
    CHECK_EQ(r.as(tesser::Access::User, Op::Set, "/sys/battery/low", "20").status, Status::Unauthorized);
}

TEST(battery_curves) {
    BatteryRig r;
    CHECK_EQ(r.battery.socOf(4.3f), 100);
    CHECK_EQ(r.battery.socOf(3.0f), 0);
    CHECK_EQ(r.battery.socOf(3.70f), 13);  // between 3.69 (10) and 3.71 (15)
    r.battery.linear(2.0f, 3.0f);         // two alkaline cells
    CHECK_EQ(r.battery.socOf(2.5f), 50);
    CHECK_EQ(r.battery.socOf(3.2f), 100);
    CHECK_EQ(r.battery.socOf(1.8f), 0);
}

TEST(battery_charging_pin) {
    BatteryRig r;
    r.fake.pins.mv[35] = 1950;
    r.fake.pins.level[4] = true;  // CHRG high: not charging
    r.battery.adc(35).chargingPin(4);
    CHECK(r.dev.begin());
    CHECK(r.fake.pins.pulled[4]);
    r.step();
    CHECK_EQ(r.value("/sys/battery/charging"), "false");
    r.fake.pins.level[4] = false;
    r.run(1100);
    CHECK_EQ(r.value("/sys/battery/charging"), "true");
}

TEST(battery_from_the_application_and_low_warning) {
    BatteryRig r;
    r.battery.lowAt(20);
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/battery/lowBattery");
    CHECK_EQ(at(r.schema("/sys/battery/lowBattery").body, "ui/toast"), "\"warn\"");
    r.battery.update(3.9f, 60, 0);
    CHECK_EQ(r.value("/sys/battery/soc"), "60");
    CHECK_EQ(r.value("/sys/battery/charging"), "false");
    r.battery.update(3.7f, 19, 0);
    r.battery.update(3.7f, 18, 0);  // once
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "19");
    r.battery.update(3.7f, 18, 1);   // charging: armed again
    r.battery.update(3.7f, 17, 0);
    CHECK_EQ(ev.take().size(), size_t(1));
    r.battery.update(3.6f, -1, -1);  // the charge from the voltage
    CHECK_EQ(r.value("/sys/battery/soc"), "5");
    CHECK_EQ(r.value("/sys/battery/charging"), "false");  // unchanged
}

// Sleep.

struct SleepRig : rig::Rig {
    tesserkit::Sleep sleep{dev};
};

TEST(sleep_for_a_time) {
    SleepRig r;
    int parked = 0;
    r.dev.onSleep([&parked] { parked++; });
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/sleep/wakeCause"), "\"none\"");
    CHECK_EQ(r.set("/sys/sleep/sleep", "60").status, Status::Ok);
    r.run(400);
    CHECK_EQ(r.fake.power.sleeps, 0);  // the reply first
    r.run(200);
    CHECK_EQ(r.fake.power.sleeps, 1);
    CHECK_EQ(parked, 1);
    CHECK_EQ(r.fake.power.us, uint64_t(60000000));
    CHECK_EQ(r.fake.power.pin, -1);
}

TEST(sleep_until_a_pin) {
    SleepRig r;
    r.fake.power.cause = tesserkit::WakeCause::Pin;
    CHECK(r.dev.begin());
    CHECK_EQ(r.value("/sys/sleep/wakeCause"), "\"pin\"");
    CHECK_EQ(r.set("/sys/sleep/sleep", "0").status, Status::InvalidValue);  // nothing would wake it
    CHECK_EQ(r.set("/sys/sleep/wakePin", "24").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/sleep/wakePin", "0").status, Status::Ok);
    CHECK_EQ(r.set("/sys/sleep/wakeLevel", "\"high\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/sleep/wakePull", "false").status, Status::Ok);
    CHECK_EQ(r.set("/sys/sleep/sleep", "0").status, Status::Ok);
    r.run(600);
    CHECK_EQ(r.fake.power.sleeps, 1);
    CHECK_EQ(r.fake.power.us, uint64_t(0));
    CHECK_EQ(r.fake.power.pin, 0);
    CHECK(r.fake.power.high);
    CHECK(!r.fake.power.pull);
    CHECK_EQ(r.as(tesser::Access::User, Op::Set, "/sys/sleep/sleep", "10").status, Status::Unauthorized);
}

TEST(sleep_waits_for_an_update_and_saves) {
    SleepRig r;
    r.sleep.wakePin(4);
    CHECK(r.dev.begin());
    r.dev.updating(true);
    r.step();
    CHECK_EQ(r.set("/sys/sleep/sleep", "0").status, Status::Busy);
    r.dev.updating(false);
    r.step();
    // A setting changed just before still reaches storage.
    CHECK_EQ(r.set("/sys/sleep/wakeLevel", "\"high\"").status, Status::Ok);
    CHECK_EQ(r.set("/sys/sleep/sleep", "0").status, Status::Ok);
    r.run(600);
    CHECK_EQ(r.fake.power.sleeps, 1);
    SleepRig again;
    again.sleep.wakePin(4);  // the same firmware
    again.storage.records = r.storage.records;
    CHECK(again.dev.begin());
    CHECK_EQ(again.value("/sys/sleep/wakeLevel"), "\"high\"");
    CHECK_EQ(again.value("/sys/sleep/wakePin"), "4");
}

}  // namespace power_cases
