#include "tesserkit/button.h"

namespace tesserkit {

bool Button::attach(Device& dev, int8_t pin, bool activeLow, std::function<void(Press)> fn) {
    Platform& p = dev.platform();
    if (!p.pins) p.pins = espDigitalPins();
    Pins* pins = p.pins;
    if (!pins || !pins->inputBegin(pin, activeLow ? Pins::Pull::Up : Pins::Pull::Down)) {
        tesser::logError("tesserkit: GPIO %d can't be a button's input", pin);
        return false;
    }
    dev.onTick([this, pins, pin, activeLow, fn](uint32_t nowMs) {
        Press press = update(pins->input(pin) != activeLow, nowMs);
        if (press != Press::None && fn) fn(press);
    });
    return true;
}

Press Button::update(bool down, uint32_t nowMs) {
    if (down != raw_) {
        raw_ = down;
        changedMs_ = nowMs;
    }
    if (raw_ != stable_ && nowMs - changedMs_ >= options_.debounceMs) {
        stable_ = raw_;
        if (stable_) {
            pressedMs_ = changedMs_;
            told_ = Press::None;
        } else if (told_ == Press::None) {
            return Press::Short;
        }
    }
    if (!stable_) return Press::None;
    uint32_t held = nowMs - pressedMs_;
    if (told_ == Press::None && held >= options_.longMs) {
        told_ = Press::Long;
        return Press::Long;
    }
    if (told_ == Press::Long && options_.veryLongMs && held >= options_.veryLongMs) {
        told_ = Press::VeryLong;
        return Press::VeryLong;
    }
    return Press::None;
}

}  // namespace tesserkit
