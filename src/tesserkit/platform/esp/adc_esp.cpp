// ADC1 through the oneshot driver, calibrated, for Pins::analogMv(). In a file
// of its own, which espPins() is in: a firmware that only reads buttons and
// drives LEDs (espDigitalPins()) doesn't link the driver.
#if defined(ESP_PLATFORM)

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_idf_version.h"
#include "tesserkit/platform/esp/esp_internal.h"
#include "tesserkit/platform.h"
#include "tesser/platform.h"

namespace tesserkit {

namespace {

constexpr int kSamples = 16;  // averaged per reading
constexpr int kMaxAnalog = 4;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
constexpr adc_atten_t kAtten = ADC_ATTEN_DB_12;
#else
constexpr adc_atten_t kAtten = ADC_ATTEN_DB_11;
#endif

class EspAdc : public esp::Adc {
public:
    bool begin(int8_t pin) override {
        if (find(pin)) return true;
        if (count_ >= kMaxAnalog) return false;
        adc_unit_t unit;
        adc_channel_t channel;
        // ADC1 only: the ESP32's ADC2 is the Wi-Fi radio's.
        if (adc_oneshot_io_to_channel(pin, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) return false;
        if (!unit_) {
            adc_oneshot_unit_init_cfg_t config = {};
            config.unit_id = ADC_UNIT_1;
            esp_err_t err = adc_oneshot_new_unit(&config, &unit_);
            if (err != ESP_OK) {
                // Arduino's analogRead() may own it already.
                tesser::logError("tesserkit: ADC1 is in use (%s)", esp_err_to_name(err));
                unit_ = nullptr;
                return false;
            }
        }
        adc_oneshot_chan_cfg_t chan = {};
        chan.atten = kAtten;
        chan.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(unit_, channel, &chan) != ESP_OK) return false;
        Analog& a = analog_[count_++];
        a.pin = pin;
        a.channel = channel;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali = {};
        cali.unit_id = ADC_UNIT_1;
        cali.chan = channel;
        cali.atten = kAtten;
        cali.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_curve_fitting(&cali, &a.cali) != ESP_OK) a.cali = nullptr;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_line_fitting_config_t cali = {};
        cali.unit_id = ADC_UNIT_1;
        cali.atten = kAtten;
        cali.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_line_fitting(&cali, &a.cali) != ESP_OK) a.cali = nullptr;
#endif
        if (!a.cali) tesser::logWarning("tesserkit: ADC not calibrated on this chip: readings are approximate");
        return true;
    }

    bool read(int8_t pin, uint32_t& mv) override {
        Analog* a = find(pin);
        if (!a) return false;
        int64_t sum = 0;
        for (int i = 0; i < kSamples; i++) {
            int v = 0;
            esp_err_t err = a->cali ? adc_oneshot_get_calibrated_result(unit_, a->cali, a->channel, &v)
                                    : adc_oneshot_read(unit_, a->channel, &v);
            if (err != ESP_OK) return false;
            if (!a->cali) v = v * 3100 / 4095;  // roughly, at 12 dB
            sum += v;
        }
        mv = static_cast<uint32_t>(sum / kSamples);
        return true;
    }

private:
    struct Analog {
        int8_t pin = -1;
        adc_channel_t channel = ADC_CHANNEL_0;
        adc_cali_handle_t cali = nullptr;
    };
    Analog* find(int8_t pin) {
        for (int i = 0; i < count_; i++) {
            if (analog_[i].pin == pin) return &analog_[i];
        }
        return nullptr;
    }

    adc_oneshot_unit_handle_t unit_ = nullptr;
    Analog analog_[kMaxAnalog];
    int count_ = 0;
};

}  // namespace

Pins* espPins() {
    static EspAdc adc;
    esp::attachAdc(&adc);
    return espDigitalPins();
}

}  // namespace tesserkit

#endif
