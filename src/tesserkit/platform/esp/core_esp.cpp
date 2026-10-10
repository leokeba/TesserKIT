// The chip: identity, heap, temperature, restart, tasks.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"

#include <ctype.h>
#include <stdio.h>

#include <memory>
#include <string>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_IDF_TARGET_ESP32
#include "esp_efuse.h"
#endif
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

namespace tesserkit {

namespace {

std::string chipModel() {
#if CONFIG_IDF_TARGET_ESP32
    switch (esp_efuse_get_pkg_ver()) {
        case 0: return "ESP32-D0WDQ6";
        case 1: return "ESP32-D0WD";
        case 2: return "ESP32-D2WD";
        case 4: return "ESP32-PICO-D4";
        case 5: return "ESP32-PICO-V3-02";
        case 6: return "ESP32-D0WDR2";
        default: return "ESP32";
    }
#else
    // "esp32s3" → "ESP32-S3"
    std::string t = CONFIG_IDF_TARGET;
    std::string out;
    for (size_t i = 0; i < t.size(); i++) {
        if (i == 5) out.push_back('-');
        out.push_back(static_cast<char>(toupper(static_cast<unsigned char>(t[i]))));
    }
    return out;
#endif
}

ResetReason resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return ResetReason::PowerOn;
        case ESP_RST_SW: return ResetReason::Software;
        case ESP_RST_PANIC: return ResetReason::Panic;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return ResetReason::Watchdog;
        case ESP_RST_BROWNOUT: return ResetReason::Brownout;
        case ESP_RST_DEEPSLEEP: return ResetReason::DeepSleep;
        default: return ResetReason::Other;
    }
}

// The length of the app image in `p`, from its segment headers (as
// esp_image_get_metadata() finds it, without linking the bootloader's
// verification code): the segments, a checksum byte padded to 16, the
// appended SHA-256.
uint32_t imageLength(const esp_partition_t* p) {
    uint8_t header[24];
    if (esp_partition_read(p, 0, header, sizeof(header)) != ESP_OK || header[0] != 0xE9) return 0;
    uint32_t len = sizeof(header);
    for (uint8_t i = 0; i < header[1]; i++) {
        uint32_t segment[2];  // load address, length
        if (esp_partition_read(p, len, segment, sizeof(segment)) != ESP_OK) return 0;
        len += sizeof(segment) + segment[1];
        if (len > p->size) return 0;
    }
    len = (len + 1 + 15) & ~15u;
    if (header[23]) len += 32;
    return len;
}

void taskMain(void* arg) {
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(arg));
    (*fn)();
    fn.reset();
    vTaskDelete(nullptr);
}

class EspCore : public Core {
public:
    void info(BootInfo& o) override {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        std::string model = chipModel();
#if CONFIG_IDF_TARGET_ESP32
        if (chip.revision >= 300 && model.compare(0, 9, "ESP32-D0W") == 0) model += "-V3";
#endif
        char buf[96];
        snprintf(buf, sizeof(buf), "%s rev %d.%d, %d core%s, %u MHz", model.c_str(), chip.revision / 100,
                 chip.revision % 100, chip.cores, chip.cores > 1 ? "s" : "",
                 static_cast<unsigned>(esp_rom_get_cpu_ticks_per_us()));
        o.chip = buf;
        o.target = CONFIG_IDF_TARGET;
        o.imageChipId = static_cast<uint16_t>(CONFIG_IDF_FIRMWARE_CHIP_ID);
        uint32_t flash = 0;
        if (esp_flash_get_size(nullptr, &flash) == ESP_OK) o.flashSize = flash;
        o.idf = esp_get_idf_version();
        esp_read_mac(o.mac, ESP_MAC_WIFI_STA);
        o.resetReason = resetReason();
        o.psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
        const esp_app_desc_t* app = esp_app_get_description();
        o.version = app->version;
        o.project = app->project_name;
        o.built = std::string(app->date) + " " + app->time;
        if (const esp_partition_t* running = esp_ota_get_running_partition()) {
            o.slot = running->label;
            o.slotSize = running->size;
            o.appSize = imageLength(running);
        }
    }
    uint64_t uptimeMs() override { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
    uint32_t freeHeap() override { return esp_get_free_heap_size(); }
    uint32_t minFreeHeap() override { return esp_get_minimum_free_heap_size(); }
    uint32_t freePsram() override { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); }
    bool temperature(float& celsius) override {
#if SOC_TEMP_SENSOR_SUPPORTED
        if (!sensor_) {
            temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
            if (temperature_sensor_install(&config, &sensor_) != ESP_OK) return false;
            if (temperature_sensor_enable(sensor_) != ESP_OK) return false;
        }
        return temperature_sensor_get_celsius(sensor_, &celsius) == ESP_OK;
#else
        (void)celsius;
        return false;
#endif
    }
    void restart() override { esp_restart(); }
    void random(uint8_t* out, size_t len) override { esp_fill_random(out, len); }
    bool spawn(const char* name, uint32_t stackBytes, std::function<void()> fn) override {
        auto* arg = new std::function<void()>(std::move(fn));
        if (xTaskCreate(taskMain, name, stackBytes, arg, 2, nullptr) == pdPASS) return true;
        delete arg;
        return false;
    }

private:
#if SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_handle_t sensor_ = nullptr;
#endif
};

}  // namespace

Core* espCore() {
    static EspCore core;
    return &core;
}

}  // namespace tesserkit

#endif
