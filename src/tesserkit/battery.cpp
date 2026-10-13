#include "tesserkit/battery.h"

#include <math.h>

namespace tesserkit {

using tesser::Access;

namespace {

constexpr uint32_t kSampleMs = 1000;
constexpr float kSmoothing = 0.25f;  // weight of each new sample
constexpr float kVoltageStep = 0.01f;  // smaller changes aren't notified
constexpr uint8_t kLowRearm = 5;       // % above `low` before it may warn again

// A lithium cell at rest: volts and the charge they mean, from full to empty.
struct Point {
    float volts;
    uint8_t soc;
};
constexpr Point kLithium[] = {
    {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80}, {3.98f, 75}, {3.95f, 70},
    {3.91f, 65},  {3.87f, 60}, {3.85f, 55}, {3.84f, 50}, {3.82f, 45}, {3.80f, 40}, {3.79f, 35},
    {3.77f, 30},  {3.75f, 25}, {3.73f, 20}, {3.71f, 15}, {3.69f, 10}, {3.61f, 5},  {3.27f, 0},
};

}  // namespace

Battery& Battery::adc(int8_t pin, float ratio) {
    adcPin_ = pin;
    ratio_ = ratio;
    return *this;
}

Battery& Battery::chargingPin(int8_t pin, bool activeLow) {
    chargePin_ = pin;
    chargeActiveLow_ = activeLow;
    return *this;
}

Battery& Battery::linear(float emptyV, float fullV) {
    linear_ = true;
    emptyV_ = emptyV;
    fullV_ = fullV;
    return *this;
}

Battery& Battery::lowAt(uint8_t percent) {
    low_ = percent;
    return *this;
}

uint8_t Battery::socOf(float v) const {
    if (linear_) {
        if (fullV_ <= emptyV_ || v <= emptyV_) return 0;
        if (v >= fullV_) return 100;
        return static_cast<uint8_t>(lroundf((v - emptyV_) / (fullV_ - emptyV_) * 100.0f));
    }
    constexpr size_t n = sizeof(kLithium) / sizeof(kLithium[0]);
    if (v >= kLithium[0].volts) return 100;
    for (size_t i = 1; i < n; i++) {
        const Point& hi = kLithium[i - 1];
        const Point& lo = kLithium[i];
        if (v >= lo.volts) {
            float f = (v - lo.volts) / (hi.volts - lo.volts);
            return static_cast<uint8_t>(lroundf(static_cast<float>(lo.soc) + f * static_cast<float>(hi.soc - lo.soc)));
        }
    }
    return 0;
}

void Battery::describe(tesser::Object& o) {
    Platform& p = dev_.platform();
    if ((adcPin_ >= 0 || chargePin_ >= 0) && !p.pins) p.pins = espPins();
    pins_ = p.pins;

    o.label("Battery").ui("icon", "battery").writeAccess(Access::Admin);
    socNode_ = &o.value("soc", static_cast<const uint8_t&>(soc_))
                    .range(0, 100)
                    .label("Charge")
                    .unit("%")
                    .ui("widget", "gauge");
    voltageNode_ = &o.value("voltage", static_cast<const float&>(voltage_))
                        .label("Voltage")
                        .unit("V")
                        .ui("precision", 2)
                        .ui("widget", "chart");
    // Known from a charger's pin, or from the application.
    if (chargePin_ >= 0 || adcPin_ < 0) {
        chargingNode_ = &o.value("charging", static_cast<const bool&>(charging_)).label("Charging");
    }
    o.value("low", low_)
        .range(0, 50)
        .unit("%")
        .persist()
        .label("Warn below")
        .doc("The charge under which a warning is shown (0: never)")
        .ui("advanced");
    lowEvent_ = &o.event("lowBattery").label("Low").ui("toast", "warn").ui("hidden");
}

void Battery::start() {
    if (adcPin_ >= 0) {
        adcOk_ = pins_ && pins_->analogBegin(adcPin_);
        if (!adcOk_) tesser::logError("tesserkit: Battery: GPIO %d can't read a voltage", adcPin_);
    }
    if (chargePin_ >= 0 && !(pins_ && pins_->inputBegin(chargePin_, chargeActiveLow_))) {
        tesser::logError("tesserkit: Battery: GPIO %d can't be read", chargePin_);
        chargePin_ = -1;
    }
}

void Battery::tick(uint32_t nowMs) {
    if (!adcOk_ && chargePin_ < 0) return;
    if (sampled_ && nowMs - sampledAtMs_ < kSampleMs) return;
    sampled_ = true;
    sampledAtMs_ = nowMs;
    int charging = -1;
    if (chargePin_ >= 0) charging = pins_->input(chargePin_) != chargeActiveLow_ ? 1 : 0;
    uint32_t mv = 0;
    if (!adcOk_ || !pins_->analogMv(adcPin_, mv)) {
        if (charging >= 0 && known_) apply(voltage_, soc_, charging);
        return;
    }
    float v = static_cast<float>(mv) / 1000.0f * ratio_;
    smoothed_ = known_ ? smoothed_ + kSmoothing * (v - smoothed_) : v;
    apply(smoothed_, -1, charging);
}

void Battery::update(float volts, int soc, int charging) {
    tesser::Lock lock(dev_.api());
    apply(volts, soc, charging);
}

void Battery::apply(float volts, int soc, int charging) {
    bool first = !known_;
    known_ = true;
    if (first || fabsf(volts - voltage_) >= kVoltageStep) {
        voltage_ = volts;
        voltageNode_->changed();
    }
    uint8_t s = soc >= 0 ? static_cast<uint8_t>(soc > 100 ? 100 : soc) : socOf(volts);
    if (first || s != soc_) {
        soc_ = s;
        socNode_->changed();
    }
    if (charging >= 0 && (charging != 0) != charging_) {
        charging_ = charging != 0;
        if (chargingNode_) chargingNode_->changed();
    }
    if (charging_ || soc_ > low_ + kLowRearm) lowSent_ = false;
    if (low_ && !charging_ && soc_ <= low_ && !lowSent_) {
        lowSent_ = true;
        lowEvent_->emit(soc_);
    }
}

}  // namespace tesserkit
