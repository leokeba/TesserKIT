// LittleFS (joltwallet/littlefs, which Arduino-ESP32 3.x ships) on a data
// partition, through the VFS.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"

#include "tesser/platform.h"

#if __has_include("esp_littlefs.h")
#include <string.h>

#include "esp_littlefs.h"
#include "esp_partition.h"
#define TESSERKIT_LITTLEFS 1
#endif

namespace tesserkit {

#if defined(TESSERKIT_LITTLEFS)

namespace {

// Whether the partition holds a LittleFS, mountable or not: its superblock,
// the first entry of the metadata pair in blocks 0 and 1, puts the magic
// "littlefs" at offset 8 of either block (LittleFS's SPEC.md). A read that
// fails counts as one: never format what couldn't be looked at.
bool holdsLittlefs(const esp_partition_t* p) {
    constexpr size_t kBlock = 4096;  // esp_littlefs's on internal flash
    for (size_t block = 0; block < 2; block++) {
        size_t at = block * kBlock + 8;
        char magic[8];
        if (at + sizeof(magic) > p->size) break;
        if (esp_partition_read(p, at, magic, sizeof(magic)) != ESP_OK) return true;
        if (memcmp(magic, "littlefs", sizeof(magic)) == 0) return true;
    }
    return false;
}

class EspFilesystem : public Filesystem {
public:
    bool mount(const char* label, const char* root, std::string& why) override {
        why.clear();
        if (!label) {
            label = "files";
            if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label)) label = "spiffs";
        }
        const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
        if (!p) return false;
        label_ = label;
        if (esp_littlefs_mounted(label)) return true;
        esp_vfs_littlefs_conf_t conf = {};
        conf.base_path = root;
        conf.partition_label = label;
        // Never esp_littlefs's format_if_mount_failed, which formats on any
        // error, no memory included: a LittleFS that doesn't mount holds the
        // settings, and the next boot may mount it (docs/DESIGN.md section
        // 6.1).
        if (esp_vfs_littlefs_register(&conf) == ESP_OK) return true;
        if (holdsLittlefs(p)) {
            why = "its LittleFS didn't mount, left as it is";
            tesser::logError("tesserkit: partition \"%s\": %s", label, why.c_str());
            return false;
        }
        tesser::logWarning("tesserkit: partition \"%s\" holds no LittleFS: formatting it", label);
        if (esp_littlefs_format(label) != ESP_OK) {
            why = "formatting it failed";
            return false;
        }
        if (esp_vfs_littlefs_register(&conf) == ESP_OK) return true;
        why = "it didn't mount once formatted";
        return false;
    }
    bool usage(uint64_t& total, uint64_t& used) override {
        size_t t = 0, u = 0;
        if (!label_ || esp_littlefs_info(label_, &t, &u) != ESP_OK) return false;
        total = t;
        used = u;
        return true;
    }
    bool format() override { return label_ && esp_littlefs_format(label_) == ESP_OK; }

private:
    const char* label_ = nullptr;
};

}  // namespace

Filesystem* espFilesystem() {
    static EspFilesystem fs;
    return &fs;
}

#else

Filesystem* espFilesystem() { return nullptr; }

#endif

}  // namespace tesserkit

#endif  // ESP_PLATFORM
