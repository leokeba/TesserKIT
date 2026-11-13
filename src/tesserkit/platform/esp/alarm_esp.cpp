// NetworkClock's timed writes: a task of its own, woken by an esp_timer at
// each write's local time (docs/DESIGN.md section 5.26).
#if defined(ESP_PLATFORM)

#include <functional>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tesserkit/platform.h"

namespace tesserkit {

namespace {

class EspAlarm : public Alarm {
public:
    // Above the device's task (5): a write isn't held up by a service's tick.
    static constexpr UBaseType_t kPriority = 6;
    // applyTimed() parses the value and runs the node's setter and its
    // notifications.
    static constexpr uint32_t kStackBytes = 6144;

    bool begin(std::function<void()> fire) override {
        if (task_) return true;
        fire_ = std::move(fire);
        esp_timer_create_args_t args = {};
        args.callback = &EspAlarm::expired;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "tk_clock";
        if (esp_timer_create(&args, &timer_) != ESP_OK) return false;
        if (xTaskCreate(&EspAlarm::run, "tk_clock", kStackBytes, this, kPriority, &task_) != pdPASS) {
            esp_timer_delete(timer_);
            timer_ = nullptr;
            task_ = nullptr;
            return false;
        }
        return true;
    }

    void at(uint64_t localUs) override {
        if (!task_) return;
        esp_timer_stop(timer_);  // not running: ESP_ERR_INVALID_STATE, nothing to stop
        int64_t wait = static_cast<int64_t>(localUs) - esp_timer_get_time();
        if (wait <= 0) {
            xTaskNotifyGive(task_);
        } else {
            esp_timer_start_once(timer_, static_cast<uint64_t>(wait));
        }
    }

    uint64_t nowUs() override { return static_cast<uint64_t>(esp_timer_get_time()); }

private:
    static void expired(void* self) { xTaskNotifyGive(static_cast<EspAlarm*>(self)->task_); }

    static void run(void* self) {
        auto* alarm = static_cast<EspAlarm*>(self);
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            alarm->fire_();
        }
    }

    std::function<void()> fire_;
    esp_timer_handle_t timer_ = nullptr;
    TaskHandle_t task_ = nullptr;
};

}  // namespace

Alarm* espAlarm() {
    static EspAlarm alarm;
    return &alarm;
}

}  // namespace tesserkit

#endif
