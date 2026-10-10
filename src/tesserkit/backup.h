#pragma once

#include "tesserkit/device.h"

namespace tesserkit {

// /sys/backup: the persisted state as a JSON file, downloaded and restored
// (docs/DESIGN.md section 5.5).
class Backup : public Service {
public:
    explicit Backup(Device& dev) : Service(dev, "backup") {}

    void describe(tesser::Object& o) override;

    // Largest backup accepted, in bytes.
    static constexpr uint32_t kMaxSize = 16384;

private:
    bool restartAfter_ = true;
};

}  // namespace tesserkit
