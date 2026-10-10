// The OTA slots: writing the next one, confirming and rolling back.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"

namespace tesserkit {

using tesser::Check;
using tesser::Status;

namespace {

class EspOta : public Ota {
public:
    uint32_t nextSlotSize() override {
        const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
        return p ? p->size : 0;
    }
    Check open(uint32_t size) override {
        abort();
        target_ = esp_ota_get_next_update_partition(nullptr);
        if (!target_) return Check::fail(Status::NotAllowed, "no OTA slot");
        // A known size is erased at once, in 64 KB blocks: a 1 MB upload
        // took 14 s instead of 25 s erasing sector by sector as it arrived
        // (ESP32, 2026-10-10). An unknown one is erased as it is written.
#ifdef OTA_WITH_SEQUENTIAL_WRITES
        esp_err_t err = esp_ota_begin(target_, size ? size : OTA_WITH_SEQUENTIAL_WRITES, &handle_);
#else
        esp_err_t err = esp_ota_begin(target_, size ? size : OTA_SIZE_UNKNOWN, &handle_);
#endif
        if (err != ESP_OK) {
            handle_ = 0;
            return Check::fail(Status::Internal, "couldn't start the update");
        }
        return Check::ok();
    }
    Check write(const uint8_t* data, size_t len) override {
        if (!handle_) return Check::fail(Status::Internal, "no update in progress");
        esp_err_t err = esp_ota_write(handle_, data, len);
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) return Check::fail(Status::InvalidValue, "not an app image for this chip");
        if (err == ESP_ERR_INVALID_SIZE) return Check::fail(Status::TooLarge, "image larger than the slot");
        if (err != ESP_OK) return Check::fail(Status::Internal, "flash write failed");
        return Check::ok();
    }
    Check finish() override {
        if (!handle_) return Check::fail(Status::Internal, "no update in progress");
        esp_err_t err = esp_ota_end(handle_);
        handle_ = 0;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) return Check::fail(Status::InvalidValue, "image verification failed");
        if (err != ESP_OK) return Check::fail(Status::Internal, "couldn't finish the update");
        if (esp_ota_set_boot_partition(target_) != ESP_OK) return Check::fail(Status::Internal, "couldn't select the new image");
        return Check::ok();
    }
    void abort() override {
        if (handle_) esp_ota_abort(handle_);
        handle_ = 0;
    }
    bool pending() override {
        esp_ota_img_states_t state;
        const esp_partition_t* running = esp_ota_get_running_partition();
        return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
               state == ESP_OTA_IMG_PENDING_VERIFY;
    }
    bool confirm() override { return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK; }
    Check rollback() override {
        const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
        esp_app_desc_t desc;
        if (!other || esp_ota_get_partition_description(other, &desc) != ESP_OK) {
            return Check::fail(Status::NotAllowed, "no previous firmware");
        }
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(other, &state) == ESP_OK &&
            (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED)) {
            return Check::fail(Status::NotAllowed, "the previous firmware failed");
        }
        if (esp_ota_set_boot_partition(other) != ESP_OK) return Check::fail(Status::Internal, "the previous firmware is damaged");
        return Check::ok();
    }

private:
    esp_ota_handle_t handle_ = 0;
    const esp_partition_t* target_ = nullptr;
};

}  // namespace

Ota* espOta() {
    static EspOta ota;
    return &ota;
}

}  // namespace tesserkit

#endif
