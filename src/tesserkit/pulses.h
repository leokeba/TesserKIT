#pragma once

// Pulse trains on one pin (docs/DESIGN.md section 5.21): several strobes,
// each with its own period, phase and pulse length, the pin high whenever any
// of them is (their union: the driver is on or off, overlaps aren't
// brighter). The GPIO matrix gives a pin one peripheral signal, so LEDC
// channels can't share it: the edges are scheduled instead, by a timer's
// alarm whose interrupt drives the pin.
//
// The application's task plans the trains in doubles, from a time base it
// gives (plan()); the interrupt steps them in integers (Engine), so it never
// touches floating point and runs from IRAM while the flash is written.
//
// For Fluante's layers (2026-10-21), and any application with several pulse
// trains on one pin, or pulses at instants of network time.

#include <stddef.h>
#include <stdint.h>

namespace tesserkit {
namespace pulses {

constexpr int kMaxTrains = 8;
// The engine's fixed point: 2^-20 µs. Times are kept from an epoch reset at
// each plan, so 2^43 µs (101 days) between plans is the range.
constexpr int kFracBits = 20;
constexpr int64_t kOneUs = int64_t(1) << kFracBits;
constexpr int64_t kMaxSpanUs = int64_t(1) << 42;
// Edges this close are taken by one interrupt: a gap shorter merges two
// pulses into one; a pulse shorter isn't emitted.
constexpr int64_t kWindowUs = 4;
// The shortest period planned: shorter ones leave their train off.
constexpr double kMinPeriodUs = 4 * kWindowUs;
// An alarm later than this counts as late (Stats::late).
constexpr uint32_t kLateUs = 20;
// A new plan's pulse that should have begun up to this long before it was
// installed begins at once, shortened; one earlier is skipped.
constexpr int64_t kGraceUs = 50;

// One train, in the time base's µs.
struct Train {
    bool on = false;
    double periodUs = 1e6 / 60;
    double phaseUs = 0;  // the pulses' start within the period: n · period + phase
    double pulseUs = 500;

    bool operator==(const Train& o) const {
        return on == o.on && periodUs == o.periodUs && phaseUs == o.phaseUs && pulseUs == o.pulseUs;
    }
    bool operator!=(const Train& o) const { return !(*this == o); }
};

// The application's clock for the trains: base = offsetUs + rate · local,
// local being the local clock (esp_timer_get_time(), µs). Network time as
// the base makes nodes asked the same trains pulse together.
struct Timebase {
    double offsetUs = 0;
    double rate = 1;

    bool operator==(const Timebase& o) const { return offsetUs == o.offsetUs && rate == o.rate; }
    bool operator!=(const Timebase& o) const { return !(*this == o); }
};

// The trains on the local clock, for the engine: each train's pulses begin at
// anchor + k · period, in 2^-20 µs from `epochUs` (local µs).
struct Plan {
    struct Line {
        bool enabled = false;
        int64_t period = 0;
        int64_t pulse = 0;
        int64_t anchor = 0;
    };
    int64_t epochUs = 0;
    int count = 0;
    Line lines[kMaxTrains];
    int64_t maxPulse = 0;  // the union's longest pulse; 0: no limit
};

// Plans `count` trains in `tb` at local time `localNowUs`. A train is
// enabled when on, with a period of at least kMinPeriodUs, a pulse longer
// than 0 and a positive rate; its pulses are capped at `maxPulseUs` (0: no
// cap), and one at least as long as its period keeps it high.
Plan plan(const Train* trains, int count, const Timebase& tb, double maxPulseUs, int64_t localNowUs);

struct Stats {
    uint32_t alarms = 0;       // interrupts
    uint32_t edges = 0;        // the pin's changes
    uint32_t late = 0;         // interrupts more than kLateUs after their alarm
    uint32_t worstLateUs = 0;  // since begin()
    uint32_t capped = 0;       // pulses the cap ended
};

// What the interrupt runs: integers, additions and comparisons only. Not
// locked: its owner calls it under the interrupt's lock.
class Engine {
public:
    // Takes a plan at local time `nowUs`, from each train's next edge: a
    // pulse in progress keeps its end, and each train's next pulse is the
    // plan's nearest to the one it had (a new train's: the first not yet
    // over, at most kGraceUs late).
    void install(const Plan& p, int64_t nowUs);
    // Steps every train to `nowUs` (local µs) and kWindowUs beyond: the pin's
    // level, and in `nextUs` when to come back (local µs, rounded up),
    // INT64_MAX for never. `dueUs`: the alarm this call answers, for the
    // lateness (Stats); nowUs for none.
    bool advance(int64_t nowUs, int64_t dueUs, int64_t& nextUs);
    bool level() const { return level_; }
    // Every train off at once, the pin low.
    void clear();
    Stats stats;

private:
    struct Lane {
        int64_t period = 0;
        int64_t pulse = 0;
        int64_t on = 0;   // the next pulse's start; while high, the current one's
        int64_t off = 0;  // while high, the current pulse's end
        bool enabled = false;
        bool high = false;
    };
    Lane lanes_[kMaxTrains];
    int64_t epochUs_ = 0;
    int64_t maxPulse_ = 0;
    int64_t highSince_ = 0;
    bool level_ = false;
};

}  // namespace pulses

// The alarm and the pin: platform/esp/pulses_esp.cpp drives a GPTimer; tests
// use a fake that runs the engine on a simulated clock.
class Pulses {
public:
    virtual ~Pulses() = default;
    // Takes the alarm and the pin, driven low. False if it is taken, or the
    // build can't keep the interrupt running while the flash is written.
    virtual bool begin(int pin) = 0;
    // Gives them back, the pin left low.
    virtual void end() = 0;
    // The local clock: esp_timer_get_time(), µs.
    virtual int64_t nowUs() = 0;
    // Installs a plan (Engine::install()) under the interrupt's lock, and
    // sets the alarm for its first edge.
    virtual void install(const pulses::Plan& p) = 0;
    virtual pulses::Stats stats() = 0;
    // The counts back to zero, the worst lateness with them.
    virtual void resetStats() = 0;
};

#if defined(ESP_PLATFORM)
// The chip's: one output, on a GPTimer of its own.
Pulses& espPulses();
#endif

// Pulse trains on one pin, and the time base they follow. Setters before
// begin() only store; nothing locks here: call them from one task (or under
// the API lock, from the tree's setters).
class PulseOutput {
public:
    PulseOutput(Pulses& driver, int pin) : driver_(driver), pin_(pin) {}

    static constexpr int kTrains = pulses::kMaxTrains;
    // poll() plans again this often, so the engine's integer steps never
    // drift from the trains' instants.
    static constexpr int64_t kReplanUs = 1000000;

    // Takes the alarm and the pin, and starts the trains that are on.
    bool begin();
    // Stops, gives the alarm back, the pin left low: another output (a
    // PwmOutput) may take it.
    void end();
    bool begun() const { return begun_; }

    // Each change takes effect from the next edge: a pulse in progress is
    // never cut short or stretched.
    void setTrain(int index, const pulses::Train& t);
    const pulses::Train& train(int index) const;
    void setTimebase(const pulses::Timebase& tb);
    const pulses::Timebase& timebase() const { return timebase_; }
    // The union's longest pulse, µs: longer ones end there (Stats::capped).
    // 0, the default: no limit.
    void setMaxPulseUs(double us);
    double maxPulseUs() const { return maxPulseUs_; }

    // Plans again every kReplanUs: call it often (the device's tick).
    void poll();
    // Since begin(), or resetStats().
    pulses::Stats stats() const { return begun_ ? driver_.stats() : pulses::Stats(); }
    void resetStats() {
        if (begun_) driver_.resetStats();
    }
    int pin() const { return pin_; }

private:
    void apply();

    Pulses& driver_;
    int pin_;
    bool begun_ = false;
    pulses::Train trains_[kTrains];
    pulses::Timebase timebase_;
    double maxPulseUs_ = 0;
    int64_t plannedAtUs_ = 0;
};

}  // namespace tesserkit
