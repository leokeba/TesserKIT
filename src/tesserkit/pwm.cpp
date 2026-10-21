#include "tesserkit/pwm.h"

#include <math.h>

namespace tesserkit {
namespace pwm {

namespace {

// The ids are ledc_clk_src_t's on the ESP32 (LEDC_APB_CLK 1, LEDC_REF_TICK
// 2); a chip with one clock for every timer ignores them.
const Clock kEsp32Clocks[] = {{80e6, 1}, {1e6, 2}};
const Clock kApbClocks[] = {{80e6, 1}};

// The divider for `hz` at `bits` from `clockHz`, rounded to the nearest step,
// or 0 when it is out of the hardware's range.
uint32_t divider(double clockHz, double hz, uint8_t bits) {
    double d = clockHz * (1 << kFractionBits) / (hz * static_cast<double>(1u << bits));
    double rounded = floor(d + 0.5);
    if (!(rounded >= kMinDivider && rounded <= kMaxDivider)) return 0;  // NaN too
    return static_cast<uint32_t>(rounded);
}

void set(Timing& t, const Clock& c, uint32_t d, uint8_t bits) {
    t.clock = c.id;
    t.clockHz = c.hz;
    t.divider = d;
    t.bits = bits;
    t.frequency = c.hz * (1 << kFractionBits) / (static_cast<double>(d) * static_cast<double>(1u << bits));
}

bool fit(const Chip& chip, double hz, uint8_t bits, Timing& t) {
    for (size_t i = 0; i < chip.clockCount; i++) {
        uint32_t d = divider(chip.clocks[i].hz, hz, bits);
        if (!d) continue;
        set(t, chip.clocks[i], d, bits);
        return true;
    }
    return false;
}

}  // namespace

const Chip kEsp32 = {kEsp32Clocks, 2, 20};
const Chip kApb14 = {kApbClocks, 1, 14};

double Chip::lowestHz() const {
    // The slowest clock, its largest divider, every bit.
    return clocks[clockCount - 1].hz * (1 << kFractionBits) /
           (static_cast<double>(kMaxDivider) * static_cast<double>(1u << maxBits));
}

double Chip::highestHz() const {
    // The fastest clock undivided, one bit.
    return clocks[0].hz / static_cast<double>(1u << kMinBits);
}

Timing plan(const Chip& chip, double hz, uint8_t bits) {
    double lo = chip.lowestHz(), hi = chip.highestHz();
    hz = !(hz > lo) ? lo : hz > hi ? hi : hz;  // NaN to the lowest
    bits = bits < kMinBits ? kMinBits : bits > chip.maxBits ? chip.maxBits : bits;
    Timing t;
    if (fit(chip, hz, bits, t)) return t;
    // Too fast for this resolution when even the fastest clock's divider is
    // under 1; else too slow.
    bool tooFast = chip.clocks[0].hz / (hz * static_cast<double>(1u << bits)) < 1;
    if (tooFast) {
        for (int b = bits - 1; b >= kMinBits; b--) {
            if (fit(chip, hz, static_cast<uint8_t>(b), t)) return t;
        }
        set(t, chip.clocks[0], kMinDivider, kMinBits);
    } else {
        for (int b = bits + 1; b <= chip.maxBits; b++) {
            if (fit(chip, hz, static_cast<uint8_t>(b), t)) return t;
        }
        set(t, chip.clocks[chip.clockCount - 1], kMaxDivider, chip.maxBits);
    }
    return t;
}

Timing trimmed(const Timing& t, int32_t steps) {
    int64_t d = static_cast<int64_t>(t.divider) + steps;
    d = d < kMinDivider ? kMinDivider : d > kMaxDivider ? kMaxDivider : d;
    Timing r = t;
    r.divider = static_cast<uint32_t>(d);
    r.frequency = t.clockHz * (1 << kFractionBits) / (static_cast<double>(d) * static_cast<double>(1u << t.bits));
    return r;
}

uint32_t dutyCounts(float percent, uint8_t bits) {
    uint32_t full = 1u << bits;
    if (!(percent > 0)) return 0;  // NaN too
    if (percent >= 100) return full;
    auto c = static_cast<uint32_t>(floor(static_cast<double>(percent) / 100.0 * full + 0.5));
    return c > full ? full : c;
}

}  // namespace pwm

bool PwmOutput::begin() {
    if (output_ >= 0) return true;
    int n = pwm_.attach(pin_);
    if (n < 0) return false;
    output_ = n;
    if (!(planned_.frequency > 0)) planned_ = pwm::plan(pwm_.chip(), frequency_, resolution_);
    timing_ = pwm::trimmed(planned_, trim_);
    pwm_.timing(output_, timing_);
    applyDuty();
    return true;
}

void PwmOutput::setOn(bool on) {
    if (on == on_) return;
    on_ = on;
    applyDuty();
}

void PwmOutput::setDuty(float percent) {
    percent = !(percent > 0) ? 0 : percent > 100 ? 100 : percent;
    if (percent == duty_) return;
    duty_ = percent;
    applyDuty();
}

void PwmOutput::setFrequency(double hz) {
    if (hz == frequency_ && planned_.frequency > 0) return;
    frequency_ = hz;
    replan();
}

void PwmOutput::setResolution(uint8_t bits) {
    if (bits == resolution_ && planned_.frequency > 0) return;
    resolution_ = bits;
    replan();
}

void PwmOutput::setTrim(int32_t steps) {
    if (!(planned_.frequency > 0)) planned_ = pwm::plan(pwm_.chip(), frequency_, resolution_);
    trim_ = steps;
    retime();
}

bool PwmOutput::sample(uint32_t& count, int64_t& localUs) const {
    return output_ >= 0 && pwm_.sample(output_, count, localUs);
}

bool PwmOutput::restartAt(int64_t localUs) {
    return output_ >= 0 && pwm_.restartAt(output_, localUs);
}

double PwmOutput::appliedDuty() const {
    return 100.0 * static_cast<double>(counts()) / static_cast<double>(1u << timing_.bits);
}

double PwmOutput::pulseUs() const {
    return static_cast<double>(counts()) * timing_.stepUs();
}

void PwmOutput::replan() {
    planned_ = pwm::plan(pwm_.chip(), frequency_, resolution_);
    trim_ = 0;
    retime();
}

// Applies the planned timing with its trim. The same resolution keeps the
// counter running (Pwm::timing()), so a trim never restarts it.
void PwmOutput::retime() {
    pwm::Timing t = pwm::trimmed(planned_, trim_);
    trim_ = static_cast<int32_t>(t.divider) - static_cast<int32_t>(planned_.divider);
    if (t == timing_ && timing_.frequency > 0) return;
    // The duty's counts follow the resolution: written after the timer.
    bool bitsChanged = t.bits != timing_.bits;
    timing_ = t;
    if (output_ < 0) return;
    pwm_.timing(output_, timing_);
    if (bitsChanged) applyDuty();
}

void PwmOutput::applyDuty() {
    if (output_ >= 0) pwm_.duty(output_, counts());
}

}  // namespace tesserkit
