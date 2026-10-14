#pragma once

#include <string>

#include "tesserkit/device.h"

namespace tesserkit {

// /sys/bluetooth: the API over Bluetooth LE (docs/DESIGN.md section 5.10),
// TesserAPI's BleTransport, so TesserUI's launcher reaches the device from a
// phone with no network in common. A Bluetooth client is a network client:
// Admin while the device is open, then the level of the token it presents.
class Bluetooth : public Service {
public:
    explicit Bluetooth(Device& dev) : Service(dev, "bluetooth") {}

    // Factory default (true), before Device::begin().
    Bluetooth& enable(bool on);

    void describe(tesser::Object& o) override;

    bool connected() const { return connected_; }

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    void apply();

    bool enabled_ = true;
    Ble* ble_ = nullptr;
    bool started_ = false;      // the stack runs
    bool advertising_ = false;  // what was last applied
    bool restart_ = false;      // a change waits for a restart
    std::string name_;          // the name advertised
    bool connected_ = false;
    std::string client_;
    tesser::Node* enabledNode_ = nullptr;
    tesser::Node* status_ = nullptr;
    tesser::Node* restartNode_ = nullptr;
    uint32_t enabledGen_ = 0;
    uint32_t polledAtMs_ = 0;
};

}  // namespace tesserkit
