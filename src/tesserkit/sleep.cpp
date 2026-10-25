#include "tesserkit/sleep.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr uint32_t kDelayMs = 500;  // the reply gets out first

}  // namespace

Sleep& Sleep::wakePin(int8_t pin, bool high, bool pull) {
    pin_ = pin;
    level_ = high ? Level::High : Level::Low;
    pull_ = pull;
    return *this;
}

void Sleep::describe(tesser::Object& o) {
    Platform& p = dev_.platform();
    if (!p.power) p.power = espPower();
    power_ = p.power;
    if (power_) cause_ = power_->wakeCause();
    Power* power = power_;

    o.label("Sleep").ui("icon", "moon").ui("group", "Device").doc("Deep sleep, woken by a timer or a pin").writeAccess(Access::Admin);
    o.action("sleep", [this](uint32_t seconds) { return sleep(seconds); })
        .range(0, kMaxSeconds)
        .unit("s")
        .label("Sleep")
        .doc("For this long (0: until the wake pin). The device is unreachable until it wakes")
        .ui("confirm", "Put the device to sleep?")
        .ui("widget", "number");
    o.value("wakePin", pin_)
        .range(-1, 63)
        .ui("labels", "-1=none")
        .validate([power](int8_t v) {
            return v < 0 || !power || power->wakePin(v)
                       ? Check::ok()
                       : Check::fail(Status::InvalidValue, "this pin can't wake the chip from deep sleep");
        })
        .persist()
        .label("Wake pin")
        .doc("The GPIO that wakes the device (-1: none)")
        .ui("widget", "number");
    o.value("wakeLevel", level_, {"low", "high"}).persist().label("Wakes when").ui("visibleWhen", "wakePin!=-1");
    o.value("wakePull", pull_)
        .persist()
        .label("Pull resistor")
        .doc("Holds the pin at the other level while asleep: off for a pin with its own resistor")
        .ui("visibleWhen", "wakePin!=-1")
        .ui("advanced");
    o.value("wakeCause", cause_, {"none", "timer", "pin", "other"})
        .readOnly()
        .label("Woken by")
        .doc("How this boot woke from deep sleep (none: it didn't)");
}

void Sleep::start() {
    if (!power_) tesser::logError("tesserkit: Sleep has no power management in this build");
}

Check Sleep::sleep(uint32_t seconds) {
    if (!power_) return Check::fail(Status::NotAllowed, "no deep sleep in this build");
    if (seconds > kMaxSeconds) return Check::fail(Status::InvalidValue, "at most a year");
    if (!seconds && pin_ < 0) return Check::fail(Status::InvalidValue, "nothing would wake it: give a time or a wake pin");
    if (pin_ >= 0 && !power_->wakePin(pin_)) return Check::fail(Status::InvalidValue, "the wake pin can't wake the chip");
    if (dev_.state() == State::Updating) return Check::fail(Status::Busy, "an update is running");
    seconds_ = seconds;
    armNow_ = true;
    return Check::ok();
}

void Sleep::tick(uint32_t nowMs) {
    if (armNow_) {
        armNow_ = false;
        armed_ = true;
        atMs_ = nowMs + kDelayMs;
    }
    if (!armed_ || static_cast<int32_t>(nowMs - atMs_) < 0) return;
    armed_ = false;
    tesser::logWarning("tesserkit: sleeping%s", seconds_ ? "" : " until the wake pin");
    dev_.sleeping();
    power_->deepSleep(static_cast<uint64_t>(seconds_) * 1000000u, pin_, level_ == Level::High, pull_);
    tesser::logError("tesserkit: deep sleep failed");  // it doesn't return otherwise
}

}  // namespace tesserkit
