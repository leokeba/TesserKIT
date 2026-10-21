#include "tesserkit/pulses.h"

#include <math.h>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
// The engine runs in the alarm's interrupt, while the flash may be written.
#define TESSERKIT_PULSES_IRAM IRAM_ATTR
#else
#define TESSERKIT_PULSES_IRAM
#endif

namespace tesserkit {
namespace pulses {

namespace {

constexpr int64_t kNever = INT64_MAX;

// a / b rounded up, for b > 0.
int64_t ceilDiv(int64_t a, int64_t b) { return a >= 0 ? (a + b - 1) / b : -((-a) / b); }

int64_t fixed(double us) { return static_cast<int64_t>(llround(us * static_cast<double>(kOneUs))); }

}  // namespace

Plan plan(const Train* trains, int count, const Timebase& tb, double maxPulseUs, int64_t localNowUs) {
    Plan p;
    p.epochUs = localNowUs;
    p.count = count < 0 ? 0 : count > kMaxTrains ? kMaxTrains : count;
    bool capped = maxPulseUs > 0 && isfinite(maxPulseUs);
    p.maxPulse = capped ? fixed(maxPulseUs) : 0;
    if (!(tb.rate > 0) || !isfinite(tb.rate) || !isfinite(tb.offsetUs)) return p;
    // Rounding: the base's magnitude only (0.25 µs at 1.7e15 µs, Unix time;
    // far less from a boot), since the pulse's place in its period comes
    // from fmod(), which is exact.
    double base = tb.offsetUs + tb.rate * static_cast<double>(localNowUs);
    for (int i = 0; i < p.count; i++) {
        const Train& t = trains[i];
        if (!t.on || !(t.periodUs >= kMinPeriodUs) || !isfinite(t.periodUs) || !(t.pulseUs > 0) ||
            !isfinite(t.phaseUs)) {
            continue;
        }
        double pulse = t.pulseUs;
        if (capped && pulse > maxPulseUs) pulse = maxPulseUs;
        // The first pulse not over yet: its start s = n · period + phase,
        // with s + pulse > base. `wait`: from base − pulse to s.
        double r = fmod(base - pulse - t.phaseUs, t.periodUs);
        double wait = r > 0 ? t.periodUs - r : -r;
        Plan::Line& line = p.lines[i];
        line.anchor = fixed((wait - pulse) / tb.rate);
        line.period = fixed(t.periodUs / tb.rate);
        line.pulse = fixed(pulse / tb.rate);
        line.enabled = line.period > 0 && line.pulse > 0;
    }
    return p;
}

void Engine::install(const Plan& p, int64_t nowUs) {
    // The lanes' times move to the plan's epoch. Plans come every second
    // (PulseOutput::poll()); after more than kMaxSpanUs a lane starts over.
    int64_t span = epochUs_ - p.epochUs;
    bool keep = span > -kMaxSpanUs && span < kMaxSpanUs;
    int64_t shift = keep ? span * kOneUs : 0;
    int64_t t = (nowUs - p.epochUs) * kOneUs;
    int64_t grace = kGraceUs * kOneUs;
    for (int i = 0; i < kMaxTrains; i++) {
        Lane& l = lanes_[i];
        if (!keep) l = Lane();
        bool had = l.enabled;
        l.on += shift;
        l.off += shift;
        Plan::Line line = i < p.count ? p.lines[i] : Plan::Line();
        l.period = line.period;
        l.pulse = line.pulse;
        l.enabled = line.enabled;
        // Disabled: a pulse in progress still ends where it was to.
        if (!line.enabled) continue;
        // The plan's pulse to come next: after the one in progress, or the
        // nearest to the one the lane was waiting for, so a plan that moves
        // the pulses by less than half a period neither repeats one nor
        // drops one. A new lane's: the first that started at most kGraceUs
        // ago.
        int64_t ref = l.high ? l.on + line.period / 2 : had ? l.on - line.period / 2 : t - grace;
        int64_t on = line.anchor + ceilDiv(ref - line.anchor, line.period) * line.period;
        if (!l.high) {
            while (on < t - grace) on += line.period;  // too late: skipped
        }
        // While high, `on` is the current pulse's start: its end adds a period.
        l.on = l.high ? on - line.period : on;
    }
    highSince_ = level_ && keep ? highSince_ + shift : 0;
    if (!keep) level_ = false;
    epochUs_ = p.epochUs;
    maxPulse_ = p.maxPulse;
}

TESSERKIT_PULSES_IRAM bool Engine::advance(int64_t nowUs, int64_t dueUs, int64_t& nextUs) {
    int64_t late = nowUs - dueUs;
    if (late > static_cast<int64_t>(kLateUs)) stats.late++;
    if (late > static_cast<int64_t>(stats.worstLateUs)) {
        stats.worstLateUs = late > 0xFFFFFFFF ? 0xFFFFFFFFu : static_cast<uint32_t>(late);
    }
    int64_t rel = nowUs - epochUs_;
    if (rel < 0) rel = 0;
    if (rel > kMaxSpanUs) rel = kMaxSpanUs;  // a plan this old: nothing is left to do
    int64_t t = rel << kFracBits;
    int64_t horizon = t + (kWindowUs << kFracBits);
    bool high = false;
    int64_t since = kNever;
    for (int i = 0; i < kMaxTrains; i++) {
        Lane& l = lanes_[i];
        // Every edge up to the horizon, in order: an end passed ends the
        // pulse at once, a pulse wholly passed is skipped, a gap within the
        // window leaves the lane high.
        for (;;) {
            if (l.high) {
                if (l.off > horizon) break;
                l.high = false;
                l.on += l.period;
                continue;
            }
            if (!l.enabled || l.on > horizon) break;
            // Far behind (a long stall): sixteen pulses over at a time.
            if (l.on + l.pulse + (l.period << 4) <= horizon) {
                l.on += l.period << 4;
                continue;
            }
            l.high = true;
            l.off = l.on + l.pulse;
        }
        if (l.high) {
            high = true;
            if (l.on < since) since = l.on;
        }
    }
    // The cap: the union high for maxPulse_ ends, whichever trains keep it.
    if (high && !level_) highSince_ = since < t ? since : t;
    if (high && maxPulse_ > 0 && t - highSince_ >= maxPulse_) {
        for (int i = 0; i < kMaxTrains; i++) {
            Lane& l = lanes_[i];
            if (!l.high) continue;
            l.high = false;
            do {
                l.on += l.period;
            } while (l.on <= horizon);
        }
        high = false;
        stats.capped++;
    }
    int64_t next = kNever;
    for (int i = 0; i < kMaxTrains; i++) {
        const Lane& l = lanes_[i];
        int64_t c = l.high ? l.off : l.enabled ? l.on : kNever;
        if (c < next) next = c;
    }
    if (high && maxPulse_ > 0 && highSince_ + maxPulse_ < next) next = highSince_ + maxPulse_;
    if (high != level_) {
        stats.edges++;
        level_ = high;
    }
    nextUs = next == kNever ? kNever : epochUs_ + ((next + kOneUs - 1) >> kFracBits);
    return high;
}

void Engine::clear() {
    for (Lane& l : lanes_) l = Lane();
    level_ = false;
    maxPulse_ = 0;
}

}  // namespace pulses

bool PulseOutput::begin() {
    if (begun_) return true;
    if (!driver_.begin(pin_)) return false;
    begun_ = true;
    apply();
    return true;
}

void PulseOutput::end() {
    if (!begun_) return;
    driver_.end();
    begun_ = false;
}

void PulseOutput::setTrain(int index, const pulses::Train& t) {
    if (index < 0 || index >= kTrains || t == trains_[index]) return;
    trains_[index] = t;
    apply();
}

const pulses::Train& PulseOutput::train(int index) const {
    static const pulses::Train none;
    return index < 0 || index >= kTrains ? none : trains_[index];
}

void PulseOutput::setTimebase(const pulses::Timebase& tb) {
    if (tb == timebase_) return;
    timebase_ = tb;
    apply();
}

void PulseOutput::setMaxPulseUs(double us) {
    us = us > 0 ? us : 0;
    if (us == maxPulseUs_) return;
    maxPulseUs_ = us;
    apply();
}

void PulseOutput::poll() {
    if (begun_ && driver_.nowUs() - plannedAtUs_ >= kReplanUs) apply();
}

void PulseOutput::apply() {
    if (!begun_) return;
    int64_t now = driver_.nowUs();
    driver_.install(pulses::plan(trains_, kTrains, timebase_, maxPulseUs_, now));
    plannedAtUs_ = now;
}

}  // namespace tesserkit
