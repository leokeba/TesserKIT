#pragma once

#include <string>
#include <vector>

#include "tesserkit/device.h"

namespace tesserkit {

// /sys/backup: the persisted state as a JSON file, downloaded and restored,
// and stored on the device when it has a Files service (docs/DESIGN.md
// section 5.5).
class Backup : public Service {
public:
    explicit Backup(Device& dev) : Service(dev, "backup") {}

    void describe(tesser::Object& o) override;

    // Factory default, before Device::begin(): how many stored backups to
    // keep (default 10).
    Backup& keep(uint8_t n);

    // Stores the settings as backups/<name>.json in the Files service,
    // replacing one of that name; "" names it after the local time
    // ("2026-10-12-1430"), or "backup-001" without one. The oldest backups
    // beyond keep() are removed. `saved` gets the name.
    tesser::Check save(const std::string& name, std::string* saved = nullptr);

    // Largest backup accepted, in bytes.
    static constexpr uint32_t kMaxSize = 16384;

private:
    friend class StoredBackups;

    // Applies a backup; the device restarts after it if restartAfter is set.
    tesser::Check restore(std::string_view text, std::vector<std::string>& skipped, bool& restart);
    static void writeResult(tesser::JsonWriter& w, const std::vector<std::string>& skipped, bool restart);

    bool restartAfter_ = true;
    uint8_t keep_ = 10;
};

}  // namespace tesserkit
