// Pulse trains on one pin (docs/DESIGN.md section 5.21): a GPTimer at 1 MHz,
// started once, whose alarm is moved to each next edge. Its interrupt steps
// the engine (pulses.cpp, in IRAM) and writes the pin by register, so it
// runs while the flash is written: the build must keep the GPTimer's
// interrupt and control functions in IRAM, or begin() refuses (a pulse held
// through a flash write would stay on for tens of milliseconds).
//
// The local clock (esp_timer) and the GPTimer count the same crystal: their
// difference, read once at begin(), maps one to the other.
#if defined(ESP_PLATFORM)

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "sdkconfig.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "tesserkit/pulses.h"

#if (defined(CONFIG_GPTIMER_ISR_IRAM_SAFE) || defined(CONFIG_GPTIMER_ISR_CACHE_SAFE)) && \
    defined(CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM)
#define TESSERKIT_PULSES_SAFE 1
#endif

namespace tesserkit {

namespace {

const char* kTag = "tesserkit";

class EspPulses : public Pulses {
public:
    bool begin(int pin) override {
#if !TESSERKIT_PULSES_SAFE
        ESP_LOGE(kTag,
                 "pulses on GPIO %d: the build must set CONFIG_GPTIMER_ISR_CACHE_SAFE (ESP-IDF 5.1: "
                 "CONFIG_GPTIMER_ISR_IRAM_SAFE) and CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM",
                 pin);
        return false;
#endif
        if (timer_) {
            ESP_LOGE(kTag, "pulses on GPIO %d: the output is taken (GPIO %d)", pin, pin_);
            return false;
        }
        // The pin back to a plain output, low: from the LEDC, say.
        gpio_set_level(static_cast<gpio_num_t>(pin), 0);
        if (gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT) != ESP_OK) {
            ESP_LOGE(kTag, "pulses: GPIO %d refused", pin);
            return false;
        }
        esp_rom_gpio_connect_out_signal(static_cast<uint32_t>(pin), SIG_GPIO_OUT_IDX, false, false);
        // The interrupt is allocated on the core that registers the
        // callback: core 1, away from Wi-Fi, whose driver holds core 0's
        // interrupts for hundreds of µs (a 500 µs pulse ended 600 µs late
        // there on a Gledopto).
        Setup setup;
        setup.self = this;
        setup.caller = xTaskGetCurrentTaskHandle();
        if (xTaskCreatePinnedToCore(startTimer, "tk_pulses", 3072, &setup, 5, nullptr, kCore) != pdPASS ||
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0 || !setup.timer) {
            ESP_LOGE(kTag, "pulses on GPIO %d: the timer didn't start", pin);
            return false;
        }
        gptimer_handle_t t = setup.timer;
        portENTER_CRITICAL(&lock_);
        uint64_t count = 0;
        gptimer_get_raw_count(t, &count);
        toLocal_ = esp_timer_get_time() - static_cast<int64_t>(count);
        engine_.clear();
        engine_.stats = pulses::Stats();
        pin_ = pin;
        timer_ = t;
        portEXIT_CRITICAL(&lock_);
        return true;
    }

    void end() override {
        if (!timer_) return;
        gptimer_stop(timer_);
        portENTER_CRITICAL(&lock_);
        engine_.clear();
        gpio_ll_set_level(&GPIO, static_cast<uint32_t>(pin_), 0);
        gptimer_handle_t t = timer_;
        timer_ = nullptr;
        portEXIT_CRITICAL(&lock_);
        gptimer_disable(t);
        gptimer_del_timer(t);
    }

    int64_t nowUs() override { return esp_timer_get_time(); }

    void install(const pulses::Plan& p) override {
        if (!timer_) return;
        portENTER_CRITICAL(&lock_);
        uint64_t count = 0;
        gptimer_get_raw_count(timer_, &count);
        engine_.install(p, static_cast<int64_t>(count) + toLocal_);
        step(static_cast<int64_t>(count), static_cast<int64_t>(count));
        portEXIT_CRITICAL(&lock_);
    }

    pulses::Stats stats() override {
        portENTER_CRITICAL(&lock_);
        pulses::Stats s = engine_.stats;
        portEXIT_CRITICAL(&lock_);
        return s;
    }

    void resetStats() override {
        portENTER_CRITICAL(&lock_);
        engine_.stats = pulses::Stats();
        portEXIT_CRITICAL(&lock_);
    }

private:
    struct Setup {
        EspPulses* self = nullptr;
        TaskHandle_t caller = nullptr;
        gptimer_handle_t timer = nullptr;
    };
    static constexpr BaseType_t kCore = portNUM_PROCESSORS > 1 ? 1 : 0;

    // On kCore: the timer, its interrupt at the highest level a C handler
    // may take (from ESP-IDF 5.2; the default before), started.
    static void startTimer(void* arg) {
        auto* setup = static_cast<Setup*>(arg);
        gptimer_config_t c = {};
        c.clk_src = GPTIMER_CLK_SRC_DEFAULT;
        c.direction = GPTIMER_COUNT_UP;
        c.resolution_hz = 1000000;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
        c.intr_priority = 3;
#endif
        gptimer_handle_t t = nullptr;
        if (gptimer_new_timer(&c, &t) == ESP_OK) {
            gptimer_event_callbacks_t cb = {};
            cb.on_alarm = onAlarm;
            if (gptimer_register_event_callbacks(t, &cb, setup->self) == ESP_OK && gptimer_enable(t) == ESP_OK &&
                gptimer_start(t) == ESP_OK) {
                setup->timer = t;
            } else {
                gptimer_del_timer(t);
            }
        }
        xTaskNotifyGive(setup->caller);
        vTaskDelete(nullptr);
    }

    static bool IRAM_ATTR onAlarm(gptimer_handle_t, const gptimer_alarm_event_data_t* e, void* arg) {
        auto* self = static_cast<EspPulses*>(arg);
        portENTER_CRITICAL_ISR(&self->lock_);
        if (self->timer_) {
            self->engine_.stats.alarms++;
            self->step(static_cast<int64_t>(e->count_value), static_cast<int64_t>(e->alarm_value));
        }
        portEXIT_CRITICAL_ISR(&self->lock_);
        return false;
    }

    // Under lock_: the engine to `count` (the timer's), the pin to its level,
    // the alarm to its next edge. An alarm the counter has already passed
    // would wait for the counter to wrap, centuries away, and a pulse with
    // it: after setting one, the counter is read again, and the engine
    // stepped again while it is behind.
    void IRAM_ATTR step(int64_t count, int64_t due) {
        for (;;) {
            int64_t next = 0;
            bool level = engine_.advance(count + toLocal_, due + toLocal_, next);
            gpio_ll_set_level(&GPIO, static_cast<uint32_t>(pin_), level ? 1 : 0);
            if (next == INT64_MAX) return;
            int64_t target = next - toLocal_;
            uint64_t now = 0;
            gptimer_get_raw_count(timer_, &now);
            if (target > static_cast<int64_t>(now)) {
                gptimer_alarm_config_t a = {};
                a.alarm_count = static_cast<uint64_t>(target);
                gptimer_set_alarm_action(timer_, &a);
                gptimer_get_raw_count(timer_, &now);
                if (target > static_cast<int64_t>(now)) return;
            }
            count = static_cast<int64_t>(now);
            due = target;
        }
    }

    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    gptimer_handle_t timer_ = nullptr;
    int pin_ = -1;
    int64_t toLocal_ = 0;  // local µs = the timer's count + toLocal_
    pulses::Engine engine_;
};

}  // namespace

Pulses& espPulses() {
    static EspPulses pulses;
    return pulses;
}

}  // namespace tesserkit

#endif
