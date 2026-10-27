// Bluetooth LE on TesserAPI's BleTransport: NimBLE on ESP-IDF (with
// CONFIG_BT_NIMBLE_ENABLED), Arduino's BLE library otherwise. Without either,
// espBle() is null.
#if defined(ESP_PLATFORM)

#include "tesserkit/platform.h"
#include "tesser/transports/ble_transport.h"

#if defined(ARDUINO)
#include <soc/soc_caps.h>
#if defined(SOC_BLE_SUPPORTED) && (defined(CONFIG_BLUEDROID_ENABLED) || defined(CONFIG_NIMBLE_ENABLED))
#define TESSERKIT_BLE 1
#endif
#elif defined(CONFIG_BT_NIMBLE_ENABLED)
#define TESSERKIT_BLE 1
#endif

#if defined(TESSERKIT_BLE)

#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if !defined(ARDUINO)
#include "esp_bt.h"
#endif

namespace tesserkit {

namespace {

class EspBle : public Ble {
public:
    bool begin(tesser::Api& api, const char* name) override {
        if (!ble_) ble_.reset(new tesser::BleTransport(api));
#if defined(ARDUINO)
        if (ble_->begun()) return true;
        ble_->begin(name);
        // Arduino's transport handles requests in poll(), outside the
        // device's task, which holds the API lock while it runs.
        if (xTaskCreate(pollTask, "tk_ble", 6144, ble_.get(), 4, nullptr) != pdPASS) return false;
        return true;
#else
        if (released_) return false;  // its memory is the heap's now
        if (ble_->begun()) return true;
        return ble_->begin(name) == ESP_OK;
#endif
    }
    bool end() override {
#if defined(ARDUINO)
        // The BLE library leaks its service at each deinit: stop() instead.
        return false;
#else
        // NimBLE's host and the controller, about 60 KB on a classic ESP32
        // (TesserAPI's measurement: 53 KB free with it on, 113 KB after).
        // It doesn't wait for the request task: fine under the API lock, and
        // when the client switching it off is a Bluetooth one.
        return ble_ && ble_->end() == ESP_OK;
#endif
    }
    bool release() override {
#if defined(ARDUINO)
        // Arduino's core releases it itself when no Bluetooth library is
        // linked (btInUse()); with one, its library may touch the stack's
        // static data from the start.
        return false;
#else
        if (released_) return true;
        if (ble_) return false;  // begun once: NimBLE's data is in use
        // The controller's DRAM (CONFIG_BTDM_RESERVE_DRAM on the classic
        // ESP32, about 56 KB) and the static data of the controller and
        // NimBLE. The classic ESP32's controller does both Bluetooths: BTDM
        // releases both, and the host's data with them; the others' do BLE.
#if CONFIG_IDF_TARGET_ESP32
        esp_err_t err = esp_bt_mem_release(ESP_BT_MODE_BTDM);
#else
        esp_err_t err = esp_bt_mem_release(ESP_BT_MODE_BLE);
#endif
        released_ = err == ESP_OK;
        return released_;
#endif
    }
    bool advertise(bool on) override {
        if (!ble_) return false;
        if (on) {
            ble_->start();
        } else {
            ble_->stop();  // and drops the client
        }
        return true;
    }
    bool name(const char* name) override {
        if (!ble_) return false;
        ble_->setName(name);  // a connected client sees it once it leaves
        return true;
    }
    bool connected() override { return ble_ && ble_->connected(); }
    std::string client() override { return ble_ ? ble_->peerAddress() : std::string(); }

private:
#if defined(ARDUINO)
    static void pollTask(void* arg) {
        auto* ble = static_cast<tesser::BleTransport*>(arg);
        for (;;) {
            ble->poll();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
#endif

    std::unique_ptr<tesser::BleTransport> ble_;
#if !defined(ARDUINO)
    bool released_ = false;  // release(): begin() can't until a restart
#endif
};

}  // namespace

Ble* espBle() {
    static EspBle ble;
    return &ble;
}

}  // namespace tesserkit

#else

namespace tesserkit {
Ble* espBle() { return nullptr; }
}  // namespace tesserkit

#endif
#endif
