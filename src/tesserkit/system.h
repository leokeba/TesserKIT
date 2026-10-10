#pragma once

#include "tesserkit/device.h"

namespace tesserkit {

// /sys/status, /sys/restart, /sys/factoryReset and the core dump
// (docs/DESIGN.md section 5.1). Mounted in /sys itself.
class System : public Service {
public:
    explicit System(Device& dev) : Service(dev, "status") {}

    void attach(tesser::Object& sys) override { describe(sys); }
    // Describes status/, restart, factoryReset and the core dump into `o`.
    void describe(tesser::Object& o) override;

private:
    ResetReason resetReason_ = ResetReason::Other;
    std::string mac_;
};

}  // namespace tesserkit
