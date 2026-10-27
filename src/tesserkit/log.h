#pragma once

#include <string>

#include "tesserkit/device.h"

namespace tesserkit {

// The firmware's log level, as ESP-IDF numbers it.
enum class LogLevel : uint8_t { None, Error, Warn, Info, Debug, Verbose };

// /sys/log: the firmware's log lines as events, its recent history as a
// file, and its level (docs/DESIGN.md section 5.14).
class Log : public Service {
public:
    explicit Log(Device& dev) : Service(dev, "log") {}

    // Options, before Device::begin().
    // Bytes of recent lines kept for the download (default 4096, at least
    // 512): one ring, which the lines waiting for the device's task share.
    Log& history(size_t bytes);
    // The level a new device logs at (default: the firmware's at boot).
    Log& level(LogLevel l);

    void describe(tesser::Object& o) override;

    // The recent lines, oldest first, each ending with a newline.
    std::string recent() const;

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    LogOutput* out_ = nullptr;
    LogLevel level_ = LogLevel::Warn;
    bool levelSet_ = false;
    uint32_t levelGen_ = 0;
    size_t historyBytes_ = 4096;
    uint32_t dropped_ = 0;
    tesser::Node* levelNode_ = nullptr;
    tesser::Node* droppedNode_ = nullptr;
    tesser::EventNode* line_ = nullptr;
};

}  // namespace tesserkit
