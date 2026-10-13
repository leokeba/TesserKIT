#pragma once

#include "tesserkit/device.h"

namespace tesserkit {

// /sys/battery: the battery's charge, voltage and charging state, read from
// an ADC pin through a divider or given by the application (docs/DESIGN.md
// section 5.14).
class Battery : public Service {
public:
    explicit Battery(Device& dev) : Service(dev, "battery") {}

    // Options, before Device::begin().
    // An ADC pin reading the battery through a divider: the battery's
    // voltage is the pin's times `ratio` (2 for two equal resistors).
    // Without one, the application calls update().
    Battery& adc(int8_t pin, float ratio = 2.0f);
    // A charger's status pin, low while charging by default (TP4056's CHRG,
    // an open drain: pulled up here).
    Battery& chargingPin(int8_t pin, bool activeLow = true);
    // The charge as a straight line from `emptyV` to `fullV`, for other
    // chemistries or several cells. By default, a lithium cell's curve
    // (3.27 V empty, 4.2 V full).
    Battery& linear(float emptyV, float fullV);
    // The charge below which `low` is emitted (default 15 %).
    Battery& lowAt(uint8_t percent);

    void describe(tesser::Object& o) override;

    // From the application (a fuel gauge, a power management chip), in any
    // task: the voltage, the charge (< 0: from the voltage), whether it
    // charges (< 0: unknown).
    void update(float volts, int soc = -1, int charging = -1);

    uint8_t soc() const { return soc_; }
    float voltage() const { return voltage_; }
    bool charging() const { return charging_; }
    // The charge of a battery at `volts`, on the configured curve.
    uint8_t socOf(float volts) const;

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    void apply(float volts, int soc, int charging);

    int8_t adcPin_ = -1;
    float ratio_ = 2.0f;
    int8_t chargePin_ = -1;
    bool chargeActiveLow_ = true;
    bool linear_ = false;
    float emptyV_ = 3.27f, fullV_ = 4.2f;
    uint8_t low_ = 15;

    Pins* pins_ = nullptr;
    bool adcOk_ = false;
    bool sampled_ = false;
    uint32_t sampledAtMs_ = 0;
    float smoothed_ = 0;  // volts, averaged over the last samples
    uint8_t soc_ = 0;
    float voltage_ = 0;
    bool charging_ = false;
    bool known_ = false;  // a value arrived
    bool lowSent_ = false;
    tesser::Node* socNode_ = nullptr;
    tesser::Node* voltageNode_ = nullptr;
    tesser::Node* chargingNode_ = nullptr;
    tesser::EventNode* lowEvent_ = nullptr;
};

}  // namespace tesserkit
