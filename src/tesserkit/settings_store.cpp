#include "tesserkit/settings_store.h"

#include <stdio.h>

#include "tesser/platform.h"

namespace tesserkit {

namespace {

constexpr uint32_t kPageBytes = 4096;
constexpr uint32_t kPageData = 4000;   // a page's room for data, its entries' headers aside
constexpr size_t kOtherNamespaces = 2048;  // PHY calibration, NowTP's key, Wi-Fi's namespace

// Bytes of NVS for records: every page but the one garbage collection
// keeps free, 5% under what was measured.
size_t usable(uint32_t capacity) {
    uint32_t pages = capacity / kPageBytes;
    if (pages < 2) return 0;
    size_t room = static_cast<size_t>(pages - 1) * kPageData;
    room -= room / 20;
    return room > kOtherNamespaces ? room - kOtherNamespaces : 0;
}

std::string kb(size_t bytes) {
    char b[16];
    snprintf(b, sizeof(b), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    return b;
}

}  // namespace

bool SettingsStore::fits(uint32_t capacity, size_t record, size_t others) {
    if (!capacity) return true;
    size_t room = usable(capacity);
    return others <= room && record <= (room - others) / 2;
}

size_t SettingsStore::largest(uint32_t capacity, size_t others) {
    size_t room = usable(capacity);
    return others <= room ? (room - others) / 2 : 0;
}

size_t SettingsStore::othersThan(std::string_view key) const {
    size_t n = 0;
    for (const auto& kv : sizes_) {
        if (kv.first != key) n += kv.second;
    }
    return n;
}

bool SettingsStore::load(std::string_view key, std::string& out) {
    if (!inner_ || !inner_->load(key, out)) return false;
    sizes_[std::string(key)] = out.size();
    return true;
}

bool SettingsStore::reserve(tesser::Reservation& held) {
    if (!writeBytes_) return true;
    // One block: the stdio buffer is the largest piece.
    held = tesser::reserveBytes(writeBytes_, true, tesser::mallocPool(writeBytes_));
    return static_cast<bool>(held);
}

bool SettingsStore::save(std::string_view key, const std::string& data) {
    if (!inner_) return false;
    size_t others = othersThan(key);
    if (!fits(capacity_, data.size(), others)) {
        std::string why = "\"" + std::string(key) + "\" not saved: " + kb(data.size()) + ", NVS can rewrite " +
                          kb(largest(capacity_, others)) + " beside the other settings";
        auto it = refusals_.find(key);
        if (it == refusals_.end() || it->second != why) {
            tesser::logError("tesserkit: settings %s", why.c_str());
            refusals_[std::string(key)] = why;
        }
        refused_ = why;
        return true;  // the stored record stays; tried again at its next change
    }
    // Without the heap for the write, TesserAPI tries again: a filesystem
    // short of memory fails its write, or takes what the rest needs.
    tesser::Reservation held;
    if (!reserve(held)) {
        wait(key, "not enough free memory");
        return false;
    }
    if (!inner_->save(key, data)) {
        wait(key, "the write failed");
        return false;
    }
    waiting_.clear();
    sizes_[std::string(key)] = data.size();
    auto it = refusals_.find(key);
    if (it != refusals_.end()) {
        refusals_.erase(it);
        refused_ = refusals_.empty() ? std::string() : refusals_.begin()->second;
    }
    return true;
}

// Told once per reason: TesserAPI tries the save again every 50 ms.
void SettingsStore::wait(std::string_view key, const char* why) {
    std::string w = "\"" + std::string(key) + "\" waiting to be saved: " + why;
    if (w == waiting_) return;
    waiting_ = std::move(w);
    tesser::logError("tesserkit: settings %s", waiting_.c_str());
}

bool SettingsStore::remove(std::string_view key) {
    if (!inner_) return false;
    tesser::Reservation held;
    if (!reserve(held) || !inner_->remove(key)) return false;
    auto s = sizes_.find(key);
    if (s != sizes_.end()) sizes_.erase(s);
    auto it = refusals_.find(key);
    if (it != refusals_.end()) {
        refusals_.erase(it);
        refused_ = refusals_.empty() ? std::string() : refusals_.begin()->second;
    }
    return true;
}

bool SettingsStore::erase() {
    sizes_.clear();
    refusals_.clear();
    refused_.clear();
    return inner_ ? inner_->erase() : false;
}

bool moveSettings(tesser::Storage* from, tesser::Storage& to, const std::vector<std::string>& keys, std::string& why) {
    std::string data, back;
    // Moved before: the filesystem's copy is the one in use. NVS keeps the
    // copy it had, no longer written: if the filesystem doesn't mount one
    // day (or a firmware without Files is installed), the device starts from
    // those settings, its network among them, rather than from defaults.
    if (to.load(SettingsStore::kMoved, data)) return true;
    // Not moved yet, or a move cut short before its marker: from an empty
    // directory, so nothing of a cut move comes back.
    auto fail = [&to, &why](std::string reason) {
        why = std::move(reason);
        to.erase();
        tesser::logError("tesserkit: settings stay in NVS: %s", why.c_str());
        return false;
    };
    if (!to.erase()) return fail("the directory couldn't be emptied");
    std::vector<const std::string*> moved;
    for (const std::string& key : keys) {
        if (!from || !from->load(key, data)) continue;
        // The write's buffers and the copy read back: a short heap stops the
        // move rather than aborts it.
        size_t bytes = kFileWriteBytes + data.size() + 1;
        tesser::Reservation held = tesser::reserveBytes(bytes, true, tesser::mallocPool(bytes));
        if (!held) return fail("no memory to move \"" + key + "\"");
        if (!to.save(key, data)) return fail("\"" + key + "\" couldn't be written");
        back.clear();
        if (!to.load(key, back) || back != data) return fail("\"" + key + "\" read back differently");
        moved.push_back(&key);
    }
    if (!to.save(SettingsStore::kMoved, "1") || !to.load(SettingsStore::kMoved, back) || back != "1") {
        return fail("the filesystem couldn't be written");
    }
    if (!moved.empty()) {
        tesser::logWarning("tesserkit: settings moved from NVS to the filesystem (%u records; NVS keeps its copy)",
                           static_cast<unsigned>(moved.size()));
    }
    return true;
}

}  // namespace tesserkit
