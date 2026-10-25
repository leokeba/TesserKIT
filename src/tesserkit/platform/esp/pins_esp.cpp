// GPIOs: digital inputs and outputs, and the ADC that espPins() attaches
// (adc_esp.cpp).
#if defined(ESP_PLATFORM)

#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "tesserkit/platform/esp/esp_internal.h"

namespace tesserkit {

namespace {

class EspPins : public Pins {
public:
    esp::Adc* adc = nullptr;  // attached by espPins()

    bool analogBegin(int8_t pin) override { return adc && adc->begin(pin); }
    bool analogMv(int8_t pin, uint32_t& mv) override { return adc && adc->read(pin, mv); }

    // gpio_reset_pin() and the calls after it rather than gpio_config(),
    // which links the interrupt setup too.
    bool inputBegin(int8_t pin, Pull pull) override {
        if (pin < 0 || !GPIO_IS_VALID_GPIO(pin)) return false;
        gpio_num_t n = static_cast<gpio_num_t>(pin);
        gpio_reset_pin(n);
        if (gpio_set_direction(n, GPIO_MODE_INPUT) != ESP_OK) return false;
        // Input-only pins (the ESP32's 34 to 39) have no pull resistors: an
        // external one holds them.
        if (GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
            gpio_set_pull_mode(n, pull == Pull::Up     ? GPIO_PULLUP_ONLY
                                  : pull == Pull::Down ? GPIO_PULLDOWN_ONLY
                                                       : GPIO_FLOATING);
        }
        return true;
    }

    bool input(int8_t pin) override { return gpio_get_level(static_cast<gpio_num_t>(pin)) != 0; }

    bool outputBegin(int8_t pin) override {
        if (pin < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(pin)) return false;
        // The flash's and PSRAM's pins, and outputs another driver reserved.
        uint64_t bit = 1ULL << pin;
        if (esp_gpio_is_reserved(bit)) return false;
        gpio_num_t n = static_cast<gpio_num_t>(pin);
        gpio_reset_pin(n);
        gpio_set_pull_mode(n, GPIO_FLOATING);
        gpio_set_level(n, 0);
        if (gpio_set_direction(n, GPIO_MODE_OUTPUT) != ESP_OK) return false;
        esp_gpio_reserve(bit);
        return true;
    }

    void output(int8_t pin, bool high) override { gpio_set_level(static_cast<gpio_num_t>(pin), high ? 1 : 0); }
};

EspPins& shared() {
    static EspPins pins;
    return pins;
}

}  // namespace

void esp::attachAdc(esp::Adc* adc) { shared().adc = adc; }

Pins* espDigitalPins() { return &shared(); }

}  // namespace tesserkit

#endif
