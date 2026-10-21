// The LEDC (docs/DESIGN.md section 5.20), from ballet-photophonique's
// pwm_esp.cpp. Each output takes a timer and a channel of the same number,
// the lowest free (detach() gives them back): on the classic ESP32 the high-speed group's four
// first, then the low-speed group's; elsewhere the low-speed group's.
// TesserMotion takes the low-speed group's from the top down.
//
// ledc_timer_config() takes whole hertz and picks its own divider, so it only
// sets a timer up; plan()'s divider, resolution and clock are then written to
// the timer's registers through the low-level layer, which the driver reads
// back for its duty checks (ledc_get_max_duty()).
//
// A phase lock's restart (restartAt()) is a one-shot alarm of a GPTimer,
// taken at the first restart: its interrupt resets the LEDC timer, a few µs
// late. The handler is in IRAM, but the interrupt only runs while the flash is
// written when the build sets CONFIG_GPTIMER_ISR_CACHE_SAFE.
#if defined(ESP_PLATFORM)

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/ledc.h"
#include "esp_rom_gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/ledc_ll.h"
#include "sdkconfig.h"
#include "soc/gpio_sig_map.h"
#include "soc/ledc_struct.h"
#include "soc/soc_caps.h"
#include "tesserkit/pwm.h"

namespace tesserkit {

namespace {

const char* kTag = "tesserkit";

#if SOC_LEDC_SUPPORT_HS_MODE
constexpr int kOutputs = 2 * SOC_LEDC_TIMER_NUM;
#else
constexpr int kOutputs = SOC_LEDC_TIMER_NUM;
#endif

// The chips whose timers' counters read at LEDC.timer_group[mode].timer[n]
// .value.timer_cnt.
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3 || \
    CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2
#define TESSERKIT_LEDC_COUNTER 1
#endif

struct Slot {
    ledc_mode_t mode;
    ledc_timer_t timer;
    ledc_channel_t channel;
};

Slot slot(int output) {
#if SOC_LEDC_SUPPORT_HS_MODE
    int n = output % SOC_LEDC_TIMER_NUM;
    return {output < SOC_LEDC_TIMER_NUM ? LEDC_HIGH_SPEED_MODE : LEDC_LOW_SPEED_MODE, static_cast<ledc_timer_t>(n),
            static_cast<ledc_channel_t>(n)};
#else
    return {LEDC_LOW_SPEED_MODE, static_cast<ledc_timer_t>(output), static_cast<ledc_channel_t>(output)};
#endif
}

class EspPwm : public Pwm {
public:
    const pwm::Chip& chip() const override {
#if SOC_LEDC_SUPPORT_REF_TICK
        return pwm::kEsp32;
#else
        return pwm::kApb14;
#endif
    }

    int attach(int pin) override {
#if !SOC_LEDC_SUPPORT_APB_CLOCK
        ESP_LOGE(kTag, "PWM: this chip's LEDC has no APB clock");
        return -1;
#endif
        int output = 0;
        while (output < kOutputs && used_[output]) output++;
        if (output >= kOutputs) {
            ESP_LOGE(kTag, "PWM on GPIO %d: no LEDC timer left", pin);
            return -1;
        }
        Slot s = slot(output);
        // APB at 1 kHz, 10 bits until timing(); where the low-speed timers
        // share a clock, this makes it APB.
        ledc_timer_config_t t = {};
        t.speed_mode = s.mode;
        t.timer_num = s.timer;
        t.duty_resolution = LEDC_TIMER_10_BIT;
        t.freq_hz = 1000;
        t.clk_cfg = LEDC_USE_APB_CLK;
        if (ledc_timer_config(&t) != ESP_OK) {
            ESP_LOGE(kTag, "PWM on GPIO %d: timer setup failed", pin);
            return -1;
        }
        ledc_channel_config_t c = {};
        c.gpio_num = pin;
        c.speed_mode = s.mode;
        c.channel = s.channel;
        c.timer_sel = s.timer;
        c.duty = 0;
        if (ledc_channel_config(&c) != ESP_OK) {
            ESP_LOGE(kTag, "PWM: GPIO %d refused", pin);
            return -1;
        }
        used_[output] = true;
        pins_[output] = pin;
        waiting_[output] = false;
        return output;
    }

    // The channel idles low, its timer pauses, and the pin goes back from
    // the LEDC's signal to a plain output, low.
    void detach(int output) override {
        if (!valid(output)) return;
        Slot s = slot(output);
        ledc_stop(s.mode, s.channel, 0);
        ledc_timer_pause(s.mode, s.timer);
        int pin = pins_[output];
        gpio_set_level(static_cast<gpio_num_t>(pin), 0);
        gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(static_cast<uint32_t>(pin), SIG_GPIO_OUT_IDX, false, false);
        used_[output] = false;
        waiting_[output] = false;
        portENTER_CRITICAL(&lock_);
        if (pending_ && restart_.timer == s.timer && restart_.mode == s.mode) pending_ = false;
        portEXIT_CRITICAL(&lock_);
    }

    void timing(int output, const pwm::Timing& t) override {
        if (!valid(output)) return;
        Slot s = slot(output);
        ledc_dev_t* hw = &LEDC;  // LEDC_LL_GET_HW() takes a group from ESP-IDF 6 on
        uint32_t bits = 0;
        ledc_ll_get_duty_resolution(hw, s.mode, s.timer, &bits);
        ledc_ll_set_clock_divider(hw, s.mode, s.timer, t.divider);
        ledc_ll_set_duty_resolution(hw, s.mode, s.timer, t.bits);
#if SOC_LEDC_HAS_TIMER_SPECIFIC_MUX
        ledc_ll_set_clock_source(hw, s.mode, s.timer, t.clock == 2 ? LEDC_REF_TICK : LEDC_APB_CLK);
#endif
        if (s.mode == LEDC_LOW_SPEED_MODE) ledc_ll_ls_timer_update(hw, s.mode, s.timer);
        // A counter already past the new top would run on to the counter's
        // wrap: minutes on a slow clock. A new period starts instead.
        if (bits != t.bits) ledc_timer_rst(s.mode, s.timer);
    }

    // The LEDC applies a duty at the end of the period, and
    // ledc_update_duty() spins in a critical section until the previous one
    // was applied: at low frequencies, longer than the interrupt watchdog
    // allows. So a duty is only written once the channel's previous one is
    // done; until then the latest waits here, for poll().
    void duty(int output, uint32_t counts) override {
        if (!valid(output)) return;
        waiting_[output] = true;
        counts_[output] = counts;
        flush(output);
    }

    void poll() override {
        for (int i = 0; i < kOutputs; i++) {
            if (used_[i]) flush(i);
        }
    }

    bool sample(int output, uint32_t& count, int64_t& localUs) override {
#if TESSERKIT_LEDC_COUNTER
        if (!valid(output)) return false;
        Slot s = slot(output);
        portENTER_CRITICAL(&lock_);
        count = LEDC.timer_group[s.mode].timer[s.timer].value.timer_cnt;
        localUs = esp_timer_get_time();
        portEXIT_CRITICAL(&lock_);
        return true;
#else
        (void)output, (void)count, (void)localUs;
        return false;
#endif
    }

    // The GPTimer counts µs from its own start: the alarm is its count now
    // plus the lead, both read with the local clock.
    bool restartAt(int output, int64_t localUs) override {
        if (!valid(output)) return false;
        if (!alarm_ && !startAlarm()) return false;
        uint64_t count = 0;
        portENTER_CRITICAL(&lock_);
        pending_ = false;  // an earlier restart, replaced
        int64_t lead = localUs - esp_timer_get_time();
        gptimer_get_raw_count(alarm_, &count);
        portEXIT_CRITICAL(&lock_);
        if (lead < kMinLeadUs) return false;
        gptimer_alarm_config_t a = {};
        a.alarm_count = count + static_cast<uint64_t>(lead);
        if (gptimer_set_alarm_action(alarm_, &a) != ESP_OK) return false;
        portENTER_CRITICAL(&lock_);
        restart_ = slot(output);
        pending_ = true;
        portEXIT_CRITICAL(&lock_);
        return true;
    }

private:
    bool valid(int output) const { return output >= 0 && output < kOutputs && used_[output]; }

    bool startAlarm() {
        gptimer_config_t c = {};
        c.clk_src = GPTIMER_CLK_SRC_DEFAULT;
        c.direction = GPTIMER_COUNT_UP;
        c.resolution_hz = 1000000;
        gptimer_handle_t t = nullptr;
        if (gptimer_new_timer(&c, &t) != ESP_OK) {
            ESP_LOGE(kTag, "PWM: no timer left for a restart");
            return false;
        }
        gptimer_event_callbacks_t cb = {};
        cb.on_alarm = onAlarm;
        if (gptimer_register_event_callbacks(t, &cb, this) != ESP_OK || gptimer_enable(t) != ESP_OK ||
            gptimer_start(t) != ESP_OK) {
            ESP_LOGE(kTag, "PWM: the restart timer didn't start");
            gptimer_del_timer(t);
            return false;
        }
        alarm_ = t;
        return true;
    }

    // Without auto-reload, the alarm fires once. In IRAM: a build with
    // CONFIG_GPTIMER_ISR_CACHE_SAFE (which PulseOutput needs) refuses a
    // callback in flash; ledc_ll_timer_rst() is inline.
    static bool IRAM_ATTR onAlarm(gptimer_handle_t, const gptimer_alarm_event_data_t*, void* self) {
        auto* p = static_cast<EspPwm*>(self);
        portENTER_CRITICAL_ISR(&p->lock_);
        if (p->pending_) {
            p->pending_ = false;
            ledc_ll_timer_rst(&LEDC, p->restart_.mode, p->restart_.timer);
        }
        portEXIT_CRITICAL_ISR(&p->lock_);
        return false;
    }

    void flush(int output) {
        if (!waiting_[output]) return;
        Slot s = slot(output);
        if (LEDC.channel_group[s.mode].channel[s.channel].conf1.duty_start) return;
        ledc_set_duty(s.mode, s.channel, counts_[output]);
        ledc_update_duty(s.mode, s.channel);
        waiting_[output] = false;
    }

    bool used_[kOutputs] = {};
    int pins_[kOutputs] = {};
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    gptimer_handle_t alarm_ = nullptr;
    volatile bool pending_ = false;
    Slot restart_ = {};
    bool waiting_[kOutputs] = {};
    uint32_t counts_[kOutputs] = {};
};

}  // namespace

Pwm& espPwm() {
    static EspPwm pwm;
    return pwm;
}

}  // namespace tesserkit

#endif
