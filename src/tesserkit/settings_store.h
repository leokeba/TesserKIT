// The device's settings storage, in front of NVS (docs/DESIGN.md section 11,
// "Settings in NVS"): it refuses a record the NVS partition couldn't
// rewrite, rather than let NVS fail (ESP_ERR_NVS_NOT_ENOUGH_SPACE) and
// TesserAPI try again every 50 ms.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <string_view>

#include "tesser/storage.h"

namespace tesserkit {

class SettingsStore : public tesser::Storage {
public:
    // `capacity`: the NVS partition's bytes; 0: no limit (another storage).
    void wrap(tesser::Storage* inner, uint32_t capacity) {
        inner_ = inner;
        capacity_ = capacity;
    }
    tesser::Storage* inner() const { return inner_; }

    bool load(std::string_view key, std::string& out) override;
    // A record that wouldn't fit isn't written, and the stored one stays:
    // true all the same, so TesserAPI doesn't try again every 50 ms (it
    // does at the record's next change); refused() tells.
    bool save(std::string_view key, const std::string& data) override;
    bool remove(std::string_view key) override;
    bool erase() override;

    // Why the last refused record was refused ("" once every record fits).
    const std::string& refused() const { return refused_; }

    // Whether NVS can keep rewriting a record of `record` bytes beside
    // `others` bytes of other records, in a partition of `capacity` bytes.
    // From ESP-IDF's own NVS run on the host (2026-10-15): a rewrite needs
    // the new copy written before the old one is erased, beside PHY
    // calibration (1904 bytes) and NowTP's key; one page stays free for
    // garbage collection. 5% under the measured limits.
    static bool fits(uint32_t capacity, size_t record, size_t others);
    // The largest record that fits beside `others`.
    static size_t largest(uint32_t capacity, size_t others);

private:
    size_t othersThan(std::string_view key) const;

    tesser::Storage* inner_ = nullptr;
    uint32_t capacity_ = 0;
    std::map<std::string, size_t, std::less<>> sizes_;  // what each stored record takes
    std::map<std::string, std::string, std::less<>> refusals_;
    std::string refused_;
};

}  // namespace tesserkit
