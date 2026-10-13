#pragma once

#include "tesserkit/device.h"

namespace tesserkit {

// /sys/sleep: deep sleep for a time or until a pin wakes the device
// (docs/DESIGN.md section 5.14). The application parks its hardware in
// Device::onSleep().
class Sleep : public Service {
public:
    // The longest sleep the action takes: a year.
    static constexpr uint32_t kMaxSeconds = 31536000;

    explicit Sleep(Device& dev) : Service(dev, "sleep") {}

    // Factory defaults, before Device::begin(): the pin that wakes the
    // device, at `high` or low, held at the other level by the chip's pull
    // resistor unless `pull` is false (a pin with its own resistor).
    Sleep& wakePin(int8_t pin, bool high = false, bool pull = true);

    void describe(tesser::Object& o) override;

    // Sleeps for `seconds` (0: until the wake pin), half a second from now,
    // so a reply gets out first. Under the API lock.
    tesser::Check sleep(uint32_t seconds);
    WakeCause wakeCause() const { return cause_; }

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    enum class Level : uint8_t { Low, High };

    Power* power_ = nullptr;
    int8_t pin_ = -1;
    Level level_ = Level::Low;
    bool pull_ = true;
    WakeCause cause_ = WakeCause::None;
    bool armed_ = false;
    bool armNow_ = false;  // set by sleep(): the delay starts at the next tick
    uint32_t atMs_ = 0;
    uint32_t seconds_ = 0;
};

}  // namespace tesserkit
