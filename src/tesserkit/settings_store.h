// The device's settings storage, in front of NVS or a filesystem
// (docs/DESIGN.md sections 6.1 and 11, "Settings in NVS"). In front of NVS,
// it refuses a record the NVS partition couldn't rewrite, rather than let
// NVS fail (ESP_ERR_NVS_NOT_ENOUGH_SPACE) and TesserAPI try again every
// 50 ms. In front of a filesystem, it reserves the heap a write takes first.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "tesser/budget.h"
#include "tesser/storage.h"

namespace tesserkit {

// What writing a file takes from the heap on LittleFS, the record aside
// (TesserAPI reserved and allocated it already): LittleFS's cache for an
// open file (CONFIG_LITTLEFS_CACHE_SIZE, 512 bytes by default), and the
// VFS's and newlib's file structures. TesserAPI's FileStorage writes
// unbuffered: a stdio buffer would add the filesystem's block, 4 KB, which a
// tight board (a Gledopto with every service, 4.8 KB above the floor) can't
// give, and every save failed there.
constexpr size_t kFileWriteBytes = 1536;

class SettingsStore : public tesser::Storage {
public:
    // `capacity`: the NVS partition's bytes; 0: no limit (another storage).
    // `writeBytes`: the heap a write takes, reserved from TesserAPI's byte
    // budget before each one (a filesystem's buffers); 0: none.
    void wrap(tesser::Storage* inner, uint32_t capacity, size_t writeBytes = 0) {
        inner_ = inner;
        capacity_ = capacity;
        writeBytes_ = writeBytes;
    }
    tesser::Storage* inner() const { return inner_; }

    bool load(std::string_view key, std::string& out) override;
    // A record that wouldn't fit isn't written, and the stored one stays:
    // true all the same, so TesserAPI doesn't try again every 50 ms (it
    // does at the record's next change); refused() tells. Without the heap
    // for a write, false: TesserAPI tries again.
    bool save(std::string_view key, const std::string& data) override;
    bool remove(std::string_view key) override;
    bool erase() override;

    // Why the last refused record was refused ("" once every record fits).
    const std::string& refused() const { return refused_; }
    // A save that couldn't be made yet (no memory, or the write failed),
    // which TesserAPI tries again; "" once it is.
    const std::string& waiting() const { return waiting_; }

    // Whether NVS can keep rewriting a record of `record` bytes beside
    // `others` bytes of other records, in a partition of `capacity` bytes.
    // From ESP-IDF's own NVS run on the host (2026-10-15): a rewrite needs
    // the new copy written before the old one is erased, beside PHY
    // calibration (1904 bytes) and NowTP's key; one page stays free for
    // garbage collection. 5% under the measured limits.
    static bool fits(uint32_t capacity, size_t record, size_t others);
    // The largest record that fits beside `others`.
    static size_t largest(uint32_t capacity, size_t others);

    // The record a filesystem holds once the settings were moved to it
    // (docs/DESIGN.md section 6.1): written after every record, so its
    // absence means a move to do, or to do again.
    static constexpr const char* kMoved = "@moved";

private:
    size_t othersThan(std::string_view key) const;
    bool reserve(tesser::Reservation& held);

    tesser::Storage* inner_ = nullptr;
    uint32_t capacity_ = 0;
    size_t writeBytes_ = 0;
    std::map<std::string, size_t, std::less<>> sizes_;  // what each stored record takes
    std::map<std::string, std::string, std::less<>> refusals_;
    std::string refused_;
    std::string waiting_;
    void wait(std::string_view key, const char* why);
};

// Moves the settings records named `keys` from `from` (NVS; null if there
// is none) to `to` (a FileStorage), at boot (docs/DESIGN.md section 6.1).
// True if `to` holds the settings now: moved before (what `from` still
// holds of them is removed), or moved by this call (each record read back,
// SettingsStore::kMoved written, then the records removed from `from`).
// False if a step failed (`why` says which): `from` is untouched, and `to`
// emptied again, so the next boot tries again.
bool moveSettings(tesser::Storage* from, tesser::Storage& to, const std::vector<std::string>& keys, std::string& why);

}  // namespace tesserkit
