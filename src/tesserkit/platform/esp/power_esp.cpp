// Deep sleep: a timer, a wake pin (ext1 where the chip has it, else its
// GPIO wake-up), and why the chip woke.
#if defined(ESP_PLATFORM)

#include "driver/gpio.h"
#include "esp_idf_version.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "soc/soc_caps.h"
#include "tesserkit/platform.h"

#if SOC_PM_SUPPORT_EXT1_WAKEUP && SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include "driver/rtc_io.h"
#endif

namespace tesserkit {

namespace {

class EspPower : public Power {
public:
    WakeCause wakeCause() override {
        if (esp_reset_reason() != ESP_RST_DEEPSLEEP) return WakeCause::None;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        uint32_t causes = esp_sleep_get_wakeup_causes();
        auto has = [causes](esp_sleep_wakeup_cause_t c) { return (causes & (1u << c)) != 0; };
#else
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        auto has = [cause](esp_sleep_wakeup_cause_t c) { return cause == c; };
#endif
        if (has(ESP_SLEEP_WAKEUP_TIMER)) return WakeCause::Timer;
        if (has(ESP_SLEEP_WAKEUP_EXT0) || has(ESP_SLEEP_WAKEUP_EXT1) || has(ESP_SLEEP_WAKEUP_GPIO)) {
            return WakeCause::Pin;
        }
        return WakeCause::Other;
    }

    bool wakePin(int8_t pin) override {
        return pin >= 0 && pin < GPIO_NUM_MAX && esp_sleep_is_valid_wakeup_gpio(static_cast<gpio_num_t>(pin));
    }

    void deepSleep(uint64_t us, int8_t pin, bool high, bool pull) override {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        if (us) esp_sleep_enable_timer_wakeup(us);
        if (pin >= 0 && wakePin(pin)) {
            gpio_num_t io = static_cast<gpio_num_t>(pin);
            uint64_t mask = 1ULL << pin;
#if SOC_PM_SUPPORT_EXT1_WAKEUP
            // One pin: ESP32's "all low" is that pin low.
            auto mode = static_cast<esp_sleep_ext1_wakeup_mode_t>(high ? 1 : 0);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
            esp_sleep_enable_ext1_wakeup_io(mask, mode);
#else
            esp_sleep_enable_ext1_wakeup(mask, mode);
#endif
#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
            if (pull) {
                // The RTC domain keeps the pull resistor on while asleep.
#if SOC_PM_SUPPORT_RTC_PERIPH_PD
                esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#endif
                rtc_gpio_init(io);
                rtc_gpio_set_direction(io, RTC_GPIO_MODE_INPUT_ONLY);
                if (high) {
                    rtc_gpio_pullup_dis(io);
                    rtc_gpio_pulldown_en(io);
                } else {
                    rtc_gpio_pulldown_dis(io);
                    rtc_gpio_pullup_en(io);
                }
            }
#endif
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
            auto mode = high ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
            esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(mask, mode);
#else
            esp_deep_sleep_enable_gpio_wakeup(mask, mode);
#endif
            if (pull) {
                gpio_set_direction(io, GPIO_MODE_INPUT);
                gpio_set_pull_mode(io, high ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY);
            }
#endif
            (void)io;
            (void)pull;
        }
        esp_deep_sleep_start();
    }
};

}  // namespace

Power* espPower() {
    static EspPower power;
    return &power;
}

}  // namespace tesserkit

#endif
