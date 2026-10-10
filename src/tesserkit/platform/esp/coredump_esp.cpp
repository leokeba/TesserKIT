// The core dump partition (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH).
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"

#include "sdkconfig.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_partition.h"
#endif

namespace tesserkit {

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

namespace {

class EspCoredump : public Coredump {
public:
    uint32_t size() override {
        size_t addr = 0, size = 0;
        return esp_core_dump_image_get(&addr, &size) == ESP_OK ? static_cast<uint32_t>(size) : 0;
    }
    int read(uint32_t offset, uint8_t* buf, size_t len) override {
        size_t addr = 0, size = 0;
        if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return -1;
        if (offset >= size) return 0;
        if (len > size - offset) len = size - offset;
        return esp_flash_read(nullptr, buf, static_cast<uint32_t>(addr + offset), len) == ESP_OK ? static_cast<int>(len)
                                                                                                   : -1;
    }
    bool erase() override { return esp_core_dump_image_erase() == ESP_OK; }
};

}  // namespace

Coredump* espCoredump() {
    static EspCoredump dump;
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr)) return nullptr;
    return &dump;
}

#else

Coredump* espCoredump() { return nullptr; }

#endif

}  // namespace tesserkit

#endif
