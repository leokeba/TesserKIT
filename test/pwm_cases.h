// PWM outputs: plan(), dutyCounts() and PwmOutput (docs/DESIGN.md section
// 5.20). The plan's cases come from ballet-photophonique's timing tests.
#pragma once

#include <math.h>

#include <vector>

#include "check.h"
#include "tesserkit/pwm.h"

namespace pwm_cases {

using tesserkit::pwm::dutyCounts;
using tesserkit::pwm::kApb14;
using tesserkit::pwm::kEsp32;
using tesserkit::pwm::plan;

constexpr uint8_t kApb = 1, kRefTick = 2;

inline bool near(double a, double b, double tolerance) { return fabs(a - b) <= tolerance; }

// Records what outputs were given, as the LEDC would run them.
struct FakePwm : tesserkit::Pwm {
    struct Output {
        int pin = -1;
        tesserkit::pwm::Timing timing;
        uint32_t counts = 0;
        int timings = 0, duties = 0;
    };
    const tesserkit::pwm::Chip* chipUsed = &kEsp32;
    size_t limit = 8;
    std::vector<Output> outputs;
    // What sample() reads, and the local clock restartAt() measures from.
    uint32_t count = 0;
    int64_t nowUs = 0;
    int restartOutput = -1;
    int64_t restartUs = 0;
    const tesserkit::pwm::Chip& chip() const override { return *chipUsed; }
    int attach(int pin) override {
        if (outputs.size() >= limit) return -1;
        Output o;
        o.pin = pin;
        outputs.push_back(o);
        return static_cast<int>(outputs.size()) - 1;
    }
    void timing(int n, const tesserkit::pwm::Timing& t) override {
        outputs[static_cast<size_t>(n)].timing = t;
        outputs[static_cast<size_t>(n)].timings++;
    }
    void duty(int n, uint32_t counts) override {
        outputs[static_cast<size_t>(n)].counts = counts;
        outputs[static_cast<size_t>(n)].duties++;
    }
    bool sample(int, uint32_t& c, int64_t& localUs) override {
        c = count;
        localUs = nowUs;
        return true;
    }
    bool restartAt(int n, int64_t localUs) override {
        if (localUs - nowUs < kMinLeadUs) return false;
        restartOutput = n;
        restartUs = localUs;
        return true;
    }
};

TEST(pwm_plan_exact_dividers) {
    auto t = plan(kEsp32, 1000, 12);
    CHECK_EQ(t.clock, kApb);
    CHECK_EQ(int(t.bits), 12);
    CHECK_EQ(t.divider, 5000u);
    CHECK(near(t.frequency, 1000, 1e-9));
    // ESP-IDF's own example: 5 kHz at 13 bits.
    t = plan(kEsp32, 5000, 13);
    CHECK_EQ(t.divider, 500u);
    CHECK(near(t.frequency, 5000, 1e-9));
}

TEST(pwm_plan_fractional_hertz) {
    // Two outputs a half hertz apart beat at 0.5 Hz: at 8 bits the divider's
    // fractional steps resolve it.
    auto a = plan(kEsp32, 440, 8);
    auto b = plan(kEsp32, 440.5, 8);
    CHECK_EQ(a.clock, kApb);
    CHECK(near(a.frequency, 440, 0.002));
    CHECK(near(b.frequency, 440.5, 0.002));
    CHECK(near(b.frequency - a.frequency, 0.5, 0.004));
    // At 16 bits the steps are about 0.6 Hz there: the nearest one.
    auto c = plan(kEsp32, 440, 16);
    CHECK_EQ(int(c.bits), 16);
    CHECK(near(c.frequency, 440, 0.4));
    // Fluante's strobe: 60.01 Hz at 12 bits, steps under 0.001 Hz.
    auto s = plan(kEsp32, 60.01, 12);
    CHECK_EQ(s.clock, kApb);
    CHECK(near(s.frequency, 60.01, 0.001));
    CHECK(near(s.stepUs(), 1e6 / 60.01 / 4096, 0.01));
}

TEST(pwm_plan_lowers_bits_when_too_fast) {
    // 1 MHz leaves 80 steps per period: 6 bits.
    auto t = plan(kEsp32, 1e6, 12);
    CHECK_EQ(int(t.bits), 6);
    CHECK_EQ(t.clock, kApb);
    CHECK(near(t.frequency, 1e6, 1e6 * 0.002));
    // The top: 40 MHz at 1 bit (divider 1).
    t = plan(kEsp32, 40e6, 20);
    CHECK_EQ(int(t.bits), 1);
    CHECK_EQ(t.divider, tesserkit::pwm::kMinDivider);
    CHECK(near(t.frequency, 40e6, 1e-3));
}

TEST(pwm_plan_ref_tick_for_low_frequencies) {
    // APB's divider would pass 1024 at 10 Hz and 8 bits: the 1 MHz tick.
    auto t = plan(kEsp32, 10, 8);
    CHECK_EQ(t.clock, kRefTick);
    CHECK_EQ(int(t.bits), 8);
    CHECK(near(t.frequency, 10, 0.001));
}

TEST(pwm_plan_raises_bits_when_too_slow) {
    auto t = plan(kEsp32, 0.5, 4);
    CHECK_EQ(int(t.bits), 11);
    CHECK_EQ(t.clock, kRefTick);
    CHECK(near(t.frequency, 0.5, 0.001));
    // The bottom: 0.001 Hz needs all 20 bits on the tick.
    t = plan(kEsp32, 0.001, 1);
    CHECK_EQ(int(t.bits), 20);
    CHECK_EQ(t.clock, kRefTick);
    CHECK(near(t.frequency, 0.001, 1e-4));
}

TEST(pwm_plan_on_chips_with_one_clock) {
    // The S3 and C3: APB for every timer, 14 bits.
    CHECK(near(kApb14.lowestHz(), 4.768, 0.001));
    CHECK(near(kApb14.highestHz(), 40e6, 1e-6));
    auto t = plan(kApb14, 1000, 12);
    CHECK_EQ(t.divider, 5000u);
    CHECK(plan(kApb14, 1000, 16) == plan(kApb14, 1000, 14));  // clamped to 14 bits
    // 10 Hz at 8 bits: APB's divider passes 1024 until 13 bits.
    t = plan(kApb14, 10, 8);
    CHECK_EQ(t.clock, kApb);
    CHECK_EQ(int(t.bits), 13);
    CHECK(near(t.frequency, 10, 0.01));
    // Below what any resolution reaches: the lowest.
    t = plan(kApb14, 1, 8);
    CHECK_EQ(int(t.bits), 14);
    CHECK_EQ(t.divider, tesserkit::pwm::kMaxDivider);
    CHECK(near(t.frequency, kApb14.lowestHz(), 1e-9));
}

TEST(pwm_plan_every_request_lands_in_range) {
    for (const tesserkit::pwm::Chip* chip : {&kEsp32, &kApb14}) {
        for (double hz = chip->lowestHz(); hz <= chip->highestHz(); hz *= 1.37) {
            for (int bits = 1; bits <= chip->maxBits; bits++) {
                auto t = plan(*chip, hz, static_cast<uint8_t>(bits));
                if (!CHECK(t.divider >= tesserkit::pwm::kMinDivider && t.divider <= tesserkit::pwm::kMaxDivider)) {
                    return;
                }
                if (!CHECK(t.bits >= 1 && t.bits <= chip->maxBits)) return;
                // Within one divider step (1/256 of the divider, at most 1/256).
                if (!CHECK(fabs(t.frequency - hz) / hz < 1.0 / 256 + 1e-9)) return;
                // The resolution asked when the frequency allows it.
                bool fits = false;
                for (size_t i = 0; i < chip->clockCount; i++) {
                    double d = chip->clocks[i].hz * 256 / (hz * (1u << bits));
                    fits = fits || (d >= 256 && d <= tesserkit::pwm::kMaxDivider);
                }
                if (fits && !CHECK_EQ(int(t.bits), bits)) return;
            }
        }
    }
}

TEST(pwm_plan_clamps) {
    CHECK(plan(kEsp32, 0, 8) == plan(kEsp32, kEsp32.lowestHz(), 8));
    CHECK(plan(kEsp32, NAN, 8) == plan(kEsp32, kEsp32.lowestHz(), 8));
    CHECK(plan(kEsp32, 1e9, 8) == plan(kEsp32, 40e6, 8));
    CHECK(plan(kEsp32, 1000, 0) == plan(kEsp32, 1000, 1));
    CHECK(plan(kEsp32, 1000, 30) == plan(kEsp32, 1000, 20));
}

TEST(pwm_duty_counts) {
    CHECK_EQ(dutyCounts(0, 8), 0u);
    CHECK_EQ(dutyCounts(100, 8), 256u);  // always on
    CHECK_EQ(dutyCounts(50, 8), 128u);
    CHECK_EQ(dutyCounts(50, 1), 1u);
    CHECK_EQ(dutyCounts(0.1f, 20), 1049u);
    CHECK_EQ(dutyCounts(-5, 8), 0u);
    CHECK_EQ(dutyCounts(250, 8), 256u);
    CHECK_EQ(dutyCounts(NAN, 8), 0u);
}

TEST(pwm_output_stores_until_begun) {
    FakePwm pwm;
    tesserkit::PwmOutput out(pwm, 4);
    out.setFrequency(60.01);
    out.setDuty(3);
    out.setOn(true);
    CHECK(pwm.outputs.empty());
    CHECK(near(out.timing().frequency, 60.01, 0.001));  // planned already
    CHECK(out.begin());
    CHECK_EQ(out.output(), 0);
    CHECK_EQ(pwm.outputs[0].pin, 4);
    CHECK(pwm.outputs[0].timing == plan(kEsp32, 60.01, 12));
    CHECK_EQ(pwm.outputs[0].counts, dutyCounts(3, 12));
    // 3 % of 1/60.01 s, to a step.
    CHECK(near(out.pulseUs(), 0.03 / 60.01 * 1e6, out.timing().stepUs()));
    CHECK(near(out.appliedDuty(), 3, 100.0 / 4096));
}

TEST(pwm_output_applies_only_changes) {
    FakePwm pwm;
    tesserkit::PwmOutput out(pwm, 4);
    CHECK(out.begin());
    CHECK_EQ(pwm.outputs[0].counts, 0u);  // off by default
    int timings = pwm.outputs[0].timings, duties = pwm.outputs[0].duties;
    out.setOn(true);
    CHECK_EQ(pwm.outputs[0].counts, dutyCounts(50, 12));
    out.setFrequency(1000);  // unchanged: no retiming
    out.setDuty(50);
    CHECK_EQ(pwm.outputs[0].timings, timings);
    CHECK_EQ(pwm.outputs[0].duties, duties + 1);
    // A frequency the same resolution reaches: the timer only.
    out.setFrequency(2000);
    CHECK_EQ(pwm.outputs[0].timings, timings + 1);
    CHECK_EQ(pwm.outputs[0].duties, duties + 1);
    // A new resolution: the duty's counts follow it.
    out.setResolution(8);
    CHECK_EQ(int(pwm.outputs[0].timing.bits), 8);
    CHECK_EQ(pwm.outputs[0].counts, 128u);
    out.setOn(false);
    CHECK_EQ(pwm.outputs[0].counts, 0u);
    CHECK_EQ(out.pulseUs(), 0.0);
}

TEST(pwm_output_without_a_timer_left) {
    FakePwm pwm;
    pwm.limit = 1;
    tesserkit::PwmOutput a(pwm, 4), b(pwm, 5);
    CHECK(a.begin());
    CHECK(!b.begin());
    CHECK(!b.begun());
    b.setOn(true);  // stored, nothing written
    CHECK_EQ(pwm.outputs.size(), 1u);
}

TEST(pwm_trimmed_moves_the_divider) {
    auto t = plan(kEsp32, 60.01, 12);
    auto up = tesserkit::pwm::trimmed(t, 3);
    CHECK_EQ(up.divider, t.divider + 3);
    CHECK_EQ(int(up.bits), int(t.bits));
    CHECK_EQ(int(up.clock), int(t.clock));
    CHECK(up.frequency < t.frequency);  // a larger divider, a longer period
    CHECK(near(up.frequency, 80e6 * 256 / (double(t.divider + 3) * 4096), 1e-9));
    CHECK(tesserkit::pwm::trimmed(t, 0) == t);
    // Within the divider's range.
    CHECK_EQ(tesserkit::pwm::trimmed(t, -1000000).divider, tesserkit::pwm::kMinDivider);
    CHECK_EQ(tesserkit::pwm::trimmed(t, 1000000).divider, tesserkit::pwm::kMaxDivider);
}

TEST(pwm_output_trim_keeps_the_plan) {
    FakePwm pwm;
    tesserkit::PwmOutput out(pwm, 4);
    out.setFrequency(60.01);
    out.setTrim(-2);  // before begin(): stored
    CHECK(pwm.outputs.empty());
    CHECK(out.begin());
    auto planned = plan(kEsp32, 60.01, 12);
    CHECK(out.planned() == planned);
    CHECK_EQ(pwm.outputs[0].timing.divider, planned.divider - 2);
    CHECK(out.timing() == pwm.outputs[0].timing);
    CHECK(out.timing().frequency > planned.frequency);
    // The timer only, at the same resolution: the LEDC keeps counting.
    out.setOn(true);
    int timings = pwm.outputs[0].timings, duties = pwm.outputs[0].duties;
    out.setTrim(5);
    CHECK_EQ(pwm.outputs[0].timings, timings + 1);
    CHECK_EQ(pwm.outputs[0].duties, duties);
    CHECK_EQ(pwm.outputs[0].timing.divider, planned.divider + 5);
    CHECK_EQ(int(pwm.outputs[0].timing.bits), int(planned.bits));
    CHECK_EQ(out.trim(), 5);
    out.setTrim(5);  // unchanged
    CHECK_EQ(pwm.outputs[0].timings, timings + 1);
    // Clamped to the divider's range, and reported as applied.
    out.setTrim(1000000);
    CHECK_EQ(pwm.outputs[0].timing.divider, tesserkit::pwm::kMaxDivider);
    CHECK_EQ(out.trim(), int32_t(tesserkit::pwm::kMaxDivider - planned.divider));
    CHECK(out.planned() == planned);
    // A new frequency clears it.
    out.setFrequency(50);
    CHECK_EQ(out.trim(), 0);
    CHECK(out.timing() == plan(kEsp32, 50, 12));
    CHECK(out.planned() == out.timing());
    out.setTrim(1);
    out.setResolution(10);
    CHECK_EQ(out.trim(), 0);
    CHECK(out.timing() == plan(kEsp32, 50, 10));
}

TEST(pwm_output_sample_and_restart) {
    FakePwm pwm;
    tesserkit::PwmOutput a(pwm, 4), b(pwm, 5);
    uint32_t count = 0;
    int64_t at = 0;
    CHECK(!b.sample(count, at));  // before begin()
    CHECK(!b.restartAt(1000000));
    CHECK(a.begin());
    CHECK(b.begin());
    pwm.count = 1234;
    pwm.nowUs = 5000000;
    CHECK(b.sample(count, at));
    CHECK_EQ(count, 1234u);
    CHECK_EQ(at, int64_t(5000000));
    // Past, or too near: refused.
    CHECK(!b.restartAt(4000000));
    CHECK(!b.restartAt(pwm.nowUs + tesserkit::Pwm::kMinLeadUs - 1));
    CHECK_EQ(pwm.restartOutput, -1);
    CHECK(b.restartAt(5100000));
    CHECK_EQ(pwm.restartOutput, 1);
    CHECK_EQ(pwm.restartUs, int64_t(5100000));
}

// A Pwm that can't do either: the defaults refuse.
TEST(pwm_defaults_refuse_sample_and_restart) {
    struct Plain : FakePwm {
        bool sample(int o, uint32_t& c, int64_t& t) override { return tesserkit::Pwm::sample(o, c, t); }
        bool restartAt(int o, int64_t t) override { return tesserkit::Pwm::restartAt(o, t); }
    } pwm;
    tesserkit::PwmOutput out(pwm, 4);
    CHECK(out.begin());
    uint32_t count = 0;
    int64_t at = 0;
    CHECK(!out.sample(count, at));
    CHECK(!out.restartAt(1000000));
}

}  // namespace pwm_cases
