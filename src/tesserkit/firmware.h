#pragma once

#include <string>

#include "tesserkit/device.h"

namespace tesserkit {

enum class FirmwareState : uint8_t { Idle, Receiving, Downloading, Verifying, Rebooting, Failed };

// /sys/firmware: updates by upload or from a URL, confirmation and rollback
// (docs/DESIGN.md section 5.4).
class Firmware : public Service {
public:
    explicit Firmware(Device& dev) : Service(dev, "firmware") {}

    void describe(tesser::Object& o) override;

    FirmwareState state() const { return state_; }
    bool pending() const { return pending_; }

    // A new image is confirmed once the device has been online this long.
    static constexpr uint32_t kConfirmAfterMs = 60000;

    // Used by transfers, from their own tasks: they take the API lock.
    void progress(uint32_t received, uint32_t total);
    void failed(const char* message);
    void verifying();
    void installed();

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    Ota& ota();
    // Starts an update in `state`; false when one is already running.
    bool begin(FirmwareState state);
    void setState(FirmwareState s);
    tesser::Status download(const std::string& url);

    Ota* ota_ = nullptr;
    FirmwareState state_ = FirmwareState::Idle;
    int32_t progress_ = 0;
    std::string error_;
    bool pending_ = false;
    tesser::Node* stateNode_ = nullptr;
    tesser::Node* progressNode_ = nullptr;
    tesser::Node* errorNode_ = nullptr;
    tesser::Node* pendingNode_ = nullptr;
};

}  // namespace tesserkit
