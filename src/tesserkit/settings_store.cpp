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
    if (!inner_->save(key, data)) return false;
    sizes_[std::string(key)] = data.size();
    auto it = refusals_.find(key);
    if (it != refusals_.end()) {
        refusals_.erase(it);
        refused_ = refusals_.empty() ? std::string() : refusals_.begin()->second;
    }
    return true;
}

bool SettingsStore::remove(std::string_view key) {
    if (!inner_) return false;
    if (!inner_->remove(key)) return false;
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

}  // namespace tesserkit
