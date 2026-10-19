#pragma once

// A precise PWM output on the LEDC (docs/DESIGN.md section 5.20): frequency,
// duty and resolution, applied to a timer of its own. The LEDC divides a
// source clock by a divider with 8 fractional bits, then counts 2^bits steps
// per period, so frequency and resolution share one budget:
// clock / (divider · 2^bits) = frequency. plan() finds the settings closest
// to what was asked, for the chip's clocks; platform-free.
//
// From ballet-photophonique's outputs (its timing.cpp and pwm_esp.cpp), for
// it and Fluante's strobe.

#include <stddef.h>
#include <stdint.h>

namespace tesserkit {
namespace pwm {

constexpr int kFractionBits = 8;
constexpr uint32_t kMinDivider = 1u << kFractionBits;  // 1.0
constexpr uint32_t kMaxDivider = 0x3FFFF;               // 1023.996: 18 bits
constexpr uint8_t kMinBits = 1;

// A source clock a timer can count.
struct Clock {
    double hz;
    uint8_t id;  // the platform's
};

// What the chip's timers can do: their clocks, fastest first (the finest
// frequency steps), and their widest counter.
struct Chip {
    const Clock* clocks;
    size_t clockCount;
    uint8_t maxBits;

    // The frequencies some resolution reaches.
    double lowestHz() const;
    double highestHz() const;
};

// The classic ESP32: APB (80 MHz) and REF_TICK (1 MHz) for each timer, 20
// bits: 0.001 Hz to 40 MHz.
extern const Chip kEsp32;
// The ESP32-S3, C3 and others with APB: one clock for every timer (80 MHz),
// 14 bits: 4.8 Hz to 40 MHz.
extern const Chip kApb14;

struct Timing {
    uint8_t clock = 0;     // Clock::id
    double clockHz = 0;
    uint32_t divider = kMinDivider;  // fixed point, 8 fractional bits
    uint8_t bits = kMinBits;
    double frequency = 0;  // what the divider gives, not what was asked

    bool operator==(const Timing& o) const {
        return clock == o.clock && divider == o.divider && bits == o.bits;
    }
    bool operator!=(const Timing& o) const { return !(*this == o); }
    // One step of the counter, in µs.
    double stepUs() const { return frequency > 0 ? 1e6 / (frequency * static_cast<double>(1u << bits)) : 0; }
};

// The settings for `hz` at `bits` of resolution: the fastest clock whose
// divider fits. When none does, the nearest resolution that can: fewer bits
// for a frequency too high, more for one too low. Beyond what any resolution
// reaches, the closest end.
Timing plan(const Chip& chip, double hz, uint8_t bits);

// The compare value for `percent` (0–100) of a period at `bits`: 0 to 2^bits,
// 2^bits being always on.
uint32_t dutyCounts(float percent, uint8_t bits);

}  // namespace pwm

// The chip's PWM timers: platform/esp/pwm_esp.cpp drives the LEDC; tests use
// a fake that records what it was given.
class Pwm {
public:
    virtual ~Pwm() = default;
    virtual const pwm::Chip& chip() const = 0;
    // An output on `pin`, low until a duty comes, with a timer and a channel
    // of its own so it has its own frequency: its number, or -1 when none is
    // left. Taken from the bottom up (TesserMotion takes them from the top).
    virtual int attach(int pin) = 0;
    // The output's timer: clock, divider and resolution, applied together.
    // A new divider alone keeps the counter running, with no glitch, so a
    // phase lock can steer the period; a new resolution starts a new period.
    virtual void timing(int output, const pwm::Timing& t) = 0;
    // The compare value, 0 to 2^bits of the last timing. It takes effect at
    // the end of the current period; one written while an earlier one still
    // waits for its period is kept, and applied by poll().
    virtual void duty(int output, uint32_t counts) = 0;
    // Applies the duties that were waiting. Often (every tick of the
    // device), under the same lock as the other calls.
    virtual void poll() {}
};

#if defined(ESP_PLATFORM)
// The chip's LEDC.
Pwm& espPwm();
#endif

// An output's settings and what its timer runs. Setters before begin() only
// store; nothing locks here: call them from one task (or under the API
// lock, from the tree's setters).
class PwmOutput {
public:
    PwmOutput(Pwm& pwm, int pin) : pwm_(pwm), pin_(pin) {}

    static constexpr double kFrequency = 1000;
    static constexpr float kDuty = 50;
    static constexpr uint8_t kResolution = 12;

    // Attaches the pin and applies the settings; false if no timer is left
    // or the pin was refused.
    bool begin();
    bool begun() const { return output_ >= 0; }

    bool on() const { return on_; }
    float duty() const { return duty_; }             // percent, as asked
    double frequency() const { return frequency_; }  // Hz, as asked
    uint8_t resolution() const { return resolution_; }  // bits, as asked
    void setOn(bool on);
    void setDuty(float percent);
    void setFrequency(double hz);
    void setResolution(uint8_t bits);

    // What the timer runs: the frequency the divider gives, and a resolution
    // that may differ from the one asked when the frequency doesn't allow it.
    // Planned by begin() and the setters (zeros before).
    const pwm::Timing& timing() const { return timing_; }
    // The compare value applied: 0 while off.
    uint32_t counts() const { return on_ ? pwm::dutyCounts(duty_, timing_.bits) : 0; }
    // The duty applied, in percent, and the length of the high part in µs.
    double appliedDuty() const;
    double pulseUs() const;

    int pin() const { return pin_; }
    int output() const { return output_; }  // the Pwm's number, -1 before begin()

private:
    void retime();
    void applyDuty();

    Pwm& pwm_;
    int pin_;
    int output_ = -1;
    bool on_ = false;
    float duty_ = kDuty;
    double frequency_ = kFrequency;
    uint8_t resolution_ = kResolution;
    pwm::Timing timing_;
};

}  // namespace tesserkit
