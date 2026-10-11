// LittleFS (joltwallet/littlefs, which Arduino-ESP32 3.x ships) on a data
// partition, through the VFS.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"

#if __has_include("esp_littlefs.h")
#include "esp_littlefs.h"
#include "esp_partition.h"
#define TESSERKIT_LITTLEFS 1
#endif

namespace tesserkit {

#if defined(TESSERKIT_LITTLEFS)

namespace {

class EspFilesystem : public Filesystem {
public:
    bool mount(const char* label, const char* root) override {
        if (!label) {
            label = "files";
            if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label)) label = "spiffs";
        }
        if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label)) return false;
        label_ = label;
        if (esp_littlefs_mounted(label)) return true;
        esp_vfs_littlefs_conf_t conf = {};
        conf.base_path = root;
        conf.partition_label = label;
        conf.format_if_mount_failed = true;
        return esp_vfs_littlefs_register(&conf) == ESP_OK;
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
