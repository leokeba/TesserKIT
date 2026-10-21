// Pulse trains on one pin (docs/DESIGN.md section 5.21): the engine against a
// simulated alarm, and PulseOutput's plans.
#pragma once

#include <math.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "check.h"
#include "tesserkit/pulses.h"
#include "tesserkit/pwm.h"

namespace pulses_cases {

using tesserkit::pulses::Stats;
using tesserkit::pulses::Timebase;
using tesserkit::pulses::Train;

// The alarm on a simulated local clock: each alarm answered `latencyUs`
// late, the pin's edges recorded.
struct FakePulses : tesserkit::Pulses {
    tesserkit::pulses::Engine engine;
    bool taken = false;
    int pin = -1;
    int begins = 0, ends = 0;
    int64_t clock = 1000000;
    int64_t latencyUs = 0;
    bool armed = false;
    int64_t alarm = 0;
    bool level = false;
    bool stuck = false;  // ever high with no alarm set: a pulse that would never end
    std::vector<std::pair<int64_t, bool>> edges;

    bool begin(int p) override {
        if (taken) return false;
        taken = true;
        pin = p;
        begins++;
        engine.clear();
        engine.stats = Stats();
        armed = false;
        return true;
    }
    void end() override {
        taken = false;
        armed = false;
        ends++;
        engine.clear();
        set(false, clock);
    }
    int64_t nowUs() override { return clock; }
    void install(const tesserkit::pulses::Plan& p) override {
        if (!taken) return;
        engine.install(p, clock);
        step(clock, clock);
    }
    Stats stats() override { return engine.stats; }
    void resetStats() override { engine.stats = Stats(); }

    void step(int64_t now, int64_t due) {
        int64_t next = 0;
        set(engine.advance(now, due, next), now);
        armed = next != INT64_MAX;
        if (armed) alarm = next;
        if (level && !armed) stuck = true;
    }
    void set(bool l, int64_t at) {
        if (l == level) return;
        level = l;
        edges.emplace_back(at, l);
    }
    // The clock to `untilUs`, the alarms answered on the way.
    void run(int64_t untilUs) {
        while (armed && alarm + latencyUs <= untilUs) {
            int64_t due = alarm;
            clock = alarm + latencyUs;
            engine.stats.alarms++;
            step(clock, due);
        }
        clock = untilUs;
    }
    // The task stalls (interrupts off) until `untilUs`, then the alarm due
    // meanwhile is answered.
    void stall(int64_t untilUs) {
        clock = untilUs;
        if (armed && alarm <= untilUs) {
            engine.stats.alarms++;
            step(clock, alarm);
        }
    }
    // The pulses seen: (start, end), local µs.
    std::vector<std::pair<int64_t, int64_t>> pulses() const {
        std::vector<std::pair<int64_t, int64_t>> out;
        for (size_t i = 0; i + 1 < edges.size(); i++) {
            if (edges[i].second && !edges[i + 1].second) out.emplace_back(edges[i].first, edges[i + 1].first);
        }
        return out;
    }
};

inline Train train(double hz, double pulseUs, double phaseUs = 0) {
    Train t;
    t.on = true;
    t.periodUs = 1e6 / hz;
    t.pulseUs = pulseUs;
    t.phaseUs = phaseUs;
    return t;
}

// The local instant of base instant `b` in `tb`.
inline double localOf(double b, const Timebase& tb) { return (b - tb.offsetUs) / tb.rate; }

TEST(pulses_one_train_on_time) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    out.setTrain(0, train(60, 500));
    CHECK(out.begin());
    CHECK_EQ(f.pin, 19);
    f.run(f.clock + 1000000);
    auto p = f.pulses();
    CHECK(p.size() >= 59 && p.size() <= 61);
    double period = 1e6 / 60;
    bool onTime = true, lengths = true;
    for (auto& q : p) {
        // Each start at n · period, the alarm rounded up to the µs.
        double n = round(static_cast<double>(q.first) / period);
        double late = static_cast<double>(q.first) - n * period;
        onTime = onTime && late > -0.001 && late < 1.0001;
        lengths = lengths && llabs(q.second - q.first - 500) <= 1;
    }
    CHECK(onTime);
    CHECK(lengths);
    CHECK(!f.stuck);
    CHECK_EQ(f.engine.stats.late, 0u);
}

TEST(pulses_union_of_overlapping_trains) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    out.setTrain(0, train(1000, 300, 0));
    out.setTrain(1, train(1000, 300, 200));  // 200..500: overlaps the first
    CHECK(out.begin());
    f.run(f.clock + 20000);
    auto p = f.pulses();
    CHECK(p.size() >= 19);
    bool merged = true;
    for (auto& q : p) merged = merged && llabs(q.second - q.first - 500) <= 1;
    CHECK(merged);  // one pulse of 500 µs per period, not two
    CHECK(!f.stuck);
}

TEST(pulses_gaps_shorter_than_the_window_merge) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    out.setTrain(0, train(1000, 300, 0));
    out.setTrain(1, train(1000, 200, 302));  // 2 µs after the first ends
    out.setTrain(2, train(1000, 100, 700));  // well apart: its own pulse
    CHECK(out.begin());
    f.run(f.clock + 10000);
    auto p = f.pulses();
    int longOnes = 0, shortOnes = 0;
    for (auto& q : p) {
        int64_t len = q.second - q.first;
        if (llabs(len - 502) <= 2) longOnes++;
        if (llabs(len - 100) <= 2) shortOnes++;
    }
    CHECK(longOnes >= 9);
    CHECK(shortOnes >= 9);
    CHECK_EQ(static_cast<size_t>(longOnes + shortOnes), p.size());
}

TEST(pulses_follow_the_timebase) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    Timebase tb;
    tb.offsetUs = 123456789.25;
    tb.rate = 1.00002;  // 20 ppm fast
    out.setTimebase(tb);
    out.setTrain(0, train(60, 500, 1000));
    CHECK(out.begin());
    double period = 1e6 / 60;
    bool onTime = true;
    for (int s = 0; s < 10; s++) {
        f.run(f.clock + 1000000);
        out.poll();
    }
    for (auto& q : f.pulses()) {
        double b = tb.offsetUs + tb.rate * static_cast<double>(q.first);
        double n = round((b - 1000) / period);
        double late = static_cast<double>(q.first) - localOf(n * period + 1000, tb);
        onTime = onTime && late >= -0.001 && late < 1.0001;
    }
    CHECK(onTime);
    CHECK(f.pulses().size() >= 599);
}

TEST(pulses_a_change_mid_pulse_keeps_the_pulse) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    out.setTrain(0, train(100, 5000, 0));  // 5 ms pulses every 10 ms
    CHECK(out.begin());
    f.run(f.clock + 30000);
    while (!f.level) f.run(f.clock + 100);
    int64_t start = f.edges.back().first;
    f.run(start + 2000);  // in the middle
    out.setTrain(0, train(100, 1000, 0));  // shorter
    f.run(start + 20000);
    auto p = f.pulses();
    auto cur = std::find_if(p.begin(), p.end(), [&](const std::pair<int64_t, int64_t>& q) { return q.first == start; });
    CHECK(cur != p.end());
    if (cur != p.end()) {
        CHECK(llabs(cur->second - cur->first - 5000) <= 1);  // not cut short
        CHECK(cur + 1 != p.end());
        if (cur + 1 != p.end()) {
            CHECK(llabs((cur + 1)->second - (cur + 1)->first - 1000) <= 1);  // the next one is
            CHECK(llabs((cur + 1)->first - start - 10000) <= 1);             // on its instant
        }
    }
    // Off mid-pulse: that pulse ends where it was to, then nothing.
    while (!f.level) f.run(f.clock + 100);
    start = f.edges.back().first;
    f.run(start + 500);
    Train off = out.train(0);
    off.on = false;
    out.setTrain(0, off);
    CHECK(f.level);
    f.run(start + 50000);
    CHECK(!f.level);
    CHECK_EQ(f.edges.back().first, start + 1000);
    CHECK(!f.armed);
    CHECK(!f.stuck);
}

TEST(pulses_replans_neither_repeat_nor_drop) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    out.setTrain(0, train(60, 500, 0));
    out.setTrain(1, train(7.3, 2000, 33333));
    CHECK(out.begin());
    // A plan at every possible place in the cycle: the device's tick, 20 ms,
    // and a time base nudged each second as a filter would.
    Timebase tb;
    for (int i = 0; i < 1000; i++) {
        f.run(f.clock + 20000);
        if (i % 50 == 0) {
            tb.offsetUs += 3;  // 3 µs a second: steering
            out.setTimebase(tb);
        } else {
            out.setTrain(0, out.train(0));  // the same: nothing to do
            out.poll();
        }
    }
    auto p = f.pulses();
    // 20 s: 1200 at 60 Hz and 146 at 7.3 Hz, a few merged where they meet.
    CHECK(p.size() >= 1300 && p.size() <= 1347);
    bool spacing = true, lengths = true;
    for (size_t i = 1; i < p.size(); i++) {
        int64_t gap = p[i].first - p[i - 1].second;
        spacing = spacing && gap > 0;
        int64_t len = p[i].second - p[i].first;
        lengths = lengths && len >= 499 && len <= 2501;
    }
    CHECK(spacing);
    CHECK(lengths);
    CHECK(!f.stuck);
}

TEST(pulses_timebase_steps) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    out.setTrain(0, train(60, 500, 0));
    CHECK(out.begin());
    f.run(f.clock + 100000);
    Timebase tb;
    tb.offsetUs = 1e6 / 60 / 3;  // a third of a period: the pulses move earlier
    out.setTimebase(tb);
    f.run(f.clock + 100000);
    tb.offsetUs = -5000000;  // seconds: a new reference
    out.setTimebase(tb);
    f.run(f.clock + 100000);
    auto p = f.pulses();
    bool lengths = true, apart = true;
    for (size_t i = 0; i < p.size(); i++) {
        lengths = lengths && llabs(p[i].second - p[i].first - 500) <= 1;
        if (i) apart = apart && p[i].first - p[i - 1].first > 5000;  // never two pulses a third of a period apart
    }
    CHECK(lengths);
    CHECK(apart);
    CHECK(p.size() >= 16 && p.size() <= 19);
    CHECK(!f.stuck);
}

TEST(pulses_late_alarms_end_pulses_at_once) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    out.setTrain(0, train(100, 3000, 0));
    CHECK(out.begin());
    f.latencyUs = 40;
    f.run(f.clock + 100000);
    Stats s = out.stats();
    CHECK(s.late > 0);
    CHECK_EQ(s.worstLateUs, 40u);
    // A stall over several periods (interrupts off): the pulses meant for it
    // are skipped, and the one in progress when it ends starts late and ends
    // on time.
    f.latencyUs = 0;
    while (f.level) f.run(f.clock + 100);
    int64_t from = f.clock;
    size_t before = f.edges.size();
    f.stall(from + 51500);  // ends 1.5 ms into a pulse
    CHECK(f.level);
    CHECK_EQ(f.edges.size(), before + 1);  // only that pulse's start
    int64_t start = f.edges.back().first;
    f.run(f.clock + 20000);
    auto p = f.pulses();
    auto cur = std::find_if(p.begin(), p.end(), [&](const std::pair<int64_t, int64_t>& q) { return q.first == start; });
    CHECK(cur != p.end());
    if (cur != p.end()) CHECK(cur->second - cur->first <= 1501);  // ended on time
    CHECK(out.stats().worstLateUs > 40000);
    CHECK(!f.stuck);
    out.resetStats();
    CHECK_EQ(out.stats().worstLateUs, 0u);
    CHECK_EQ(out.stats().late, 0u);
}

TEST(pulses_cap_bounds_the_union) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    out.setMaxPulseUs(800);
    out.setTrain(0, train(500, 600, 0));    // 0..600 every 2 ms
    out.setTrain(1, train(500, 600, 500));  // 500..1100: the union 0..1100
    out.setTrain(2, train(50, 5000, 10000));  // one train alone longer than the cap
    CHECK(out.begin());
    f.run(f.clock + 100000);
    bool bounded = true;
    for (auto& q : f.pulses()) bounded = bounded && q.second - q.first <= 801;
    CHECK(bounded);
    CHECK(out.stats().capped > 0);
    CHECK(!f.stuck);
    // Without a cap, the union is whole.
    out.setMaxPulseUs(0);
    Train two = out.train(2);
    two.on = false;
    out.setTrain(2, two);
    f.edges.clear();
    f.run(f.clock + 20000);
    bool whole = true;
    auto p = f.pulses();
    for (auto& q : p) whole = whole && llabs(q.second - q.first - 1100) <= 1;
    CHECK(whole);
    CHECK(!p.empty());
}

TEST(pulses_eight_trains_at_a_kilohertz) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    for (int i = 0; i < 8; i++) out.setTrain(i, train(1000 + 13 * i, 40, 110 * i));
    CHECK(out.begin());
    for (int s = 0; s < 4; s++) {
        f.run(f.clock + 250000);
        out.poll();
    }
    Stats st = out.stats();
    CHECK(st.edges > 8000);  // most pulses apart, some merged
    // At most an alarm for each train's edges (one ending while another is
    // high changes nothing on the pin), and the plans'.
    double hz = 0;
    for (int i = 0; i < 8; i++) hz += 1000 + 13 * i;
    CHECK(st.alarms <= static_cast<uint32_t>(2 * hz + 100));
    CHECK(!f.stuck);
}

// Days of uptime and a time base far from 0: the instants stay exact.
TEST(pulses_long_uptime) {
    FakePulses f;
    f.clock = int64_t(40) * 86400 * 1000000;  // 40 days
    tesserkit::PulseOutput out(f, 19);
    Timebase tb;
    tb.offsetUs = 3.7e11;
    tb.rate = 0.999987;
    out.setTimebase(tb);
    out.setTrain(0, train(59.94, 500, 4321.5));
    CHECK(out.begin());
    for (int s = 0; s < 5; s++) {
        f.run(f.clock + 1000000);
        out.poll();
    }
    double period = 1e6 / 59.94;
    bool onTime = true;
    for (auto& q : f.pulses()) {
        double b = tb.offsetUs + tb.rate * static_cast<double>(q.first);
        double n = round((b - 4321.5) / period);
        double late = static_cast<double>(q.first) - localOf(n * period + 4321.5, tb);
        onTime = onTime && late > -0.01 && late < 1.01;
    }
    CHECK(onTime);
    CHECK(f.pulses().size() >= 298);
}

TEST(pulses_output_ends_and_begins_again) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    Train t = train(100, 4000);
    out.setTrain(0, t);  // stored before begin()
    CHECK(!out.begun());
    CHECK(f.edges.empty());
    CHECK(out.begin());
    f.run(f.clock + 30000);
    while (!f.level) f.run(f.clock + 100);
    out.end();
    CHECK(!f.level);  // low at once
    CHECK(!f.taken);
    CHECK(out.stats().alarms == 0);
    CHECK(out.train(0) == t);  // settings kept
    // Another output can take the alarm meanwhile; this one can't.
    tesserkit::PulseOutput other(f, 21);
    CHECK(other.begin());
    CHECK(!out.begin());
    other.end();
    CHECK(out.begin());
    f.run(f.clock + 30000);
    CHECK(!f.pulses().empty());
    CHECK(!out.train(9).on);  // beyond the trains: none
}

TEST(pulses_bad_trains_stay_off) {
    FakePulses f;
    tesserkit::PulseOutput out(f, 19);
    out.setTrain(0, train(1e6, 0.5));  // 1 µs period: shorter than the window allows
    Train t = train(60, 0);
    out.setTrain(1, t);  // no pulse
    t = train(60, 500);
    t.periodUs = NAN;
    out.setTrain(2, t);
    CHECK(out.begin());
    Timebase tb;
    tb.rate = 0;  // no time base at all
    f.run(f.clock + 100000);
    CHECK(f.edges.empty());
    CHECK(!f.armed);
    out.setTimebase(tb);
    out.setTrain(3, train(60, 500));
    f.run(f.clock + 100000);
    CHECK(f.edges.empty());
}

// PwmOutput::end(): the LEDC's timer given back, so the pin can go to the
// pulse trains and back.
TEST(pwm_output_ends_and_begins_again) {
    struct Detaching : tesserkit::Pwm {
        int attached = 0, detached = -1;
        const tesserkit::pwm::Chip& chip() const override { return tesserkit::pwm::kEsp32; }
        int attach(int) override { return attached++; }
        void timing(int, const tesserkit::pwm::Timing&) override {}
        void duty(int, uint32_t) override {}
        void detach(int output) override { detached = output; }
    } pwm;
    tesserkit::PwmOutput out(pwm, 19);
    out.setFrequency(60);
    CHECK(out.begin());
    CHECK_EQ(out.output(), 0);
    out.end();
    CHECK_EQ(pwm.detached, 0);
    CHECK(!out.begun());
    CHECK_EQ(out.frequency(), 60.0);
    CHECK(out.begin());
    CHECK(out.timing().frequency > 59.9);
}

}  // namespace pulses_cases
