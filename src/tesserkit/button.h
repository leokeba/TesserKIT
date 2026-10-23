#pragma once

// A push button on a GPIO (docs/DESIGN.md section 5.23): sampled every
// period of the device's task, debounced, told as presses. The logic is
// platform-free (update()); attach() reads a pin through the platform's
// Pins. From pixled-controller's function button, for any board's (a
// Gledopto's on GPIO 17, a devkit's BOOT button on GPIO 0).

#include <stdint.h>

#include <functional>

#include "tesserkit/device.h"

namespace tesserkit {

enum class Press : uint8_t {
    None,
    Short,     // released before the long time
    Long,      // held the long time: told once, while still held
    VeryLong,  // held the very long time (Button::Options::veryLongMs), after Long
};

class Button {
public:
    struct Options {
        uint32_t debounceMs = 30;  // a level must hold this long to count
        uint32_t longMs = 1000;
        uint32_t veryLongMs = 0;  // 0: none (10000 is WLED's factory reset)
    };

    Button() = default;
    explicit Button(const Options& options) : options_(options) {}

    // Reads `pin` every period of the device's task (20 ms), from
    // Device::begin(), and calls `fn` there with each press. Active low: the
    // button pulls the pin to ground, against the chip's pull-up; else to
    // the supply, against its pull-down. Before Device::begin(); false if
    // the pin can't be an input.
    bool attach(Device& dev, int8_t pin, bool activeLow, std::function<void(Press)> fn);

    // The button's level now (true: pressed): the press it completes, if
    // any. A short press is told on release; a long one once, when held
    // longMs, and a very long one once more at veryLongMs; the release of
    // either is nothing.
    Press update(bool down, uint32_t nowMs);
    bool pressed() const { return stable_; }
    // How long the press in progress has lasted (0 when released): for a
    // light telling the user that letting go now is a long press.
    uint32_t heldMs(uint32_t nowMs) const { return stable_ ? nowMs - pressedMs_ : 0; }

private:
    Options options_;
    bool raw_ = false;
    bool stable_ = false;
    uint32_t changedMs_ = 0;
    uint32_t pressedMs_ = 0;
    Press told_ = Press::None;  // the longest told for this press
};

}  // namespace tesserkit
