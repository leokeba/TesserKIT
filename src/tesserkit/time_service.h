#pragma once

#include <string>

#include "tesserkit/device.h"

namespace tesserkit {

enum class TimeSource : uint8_t { None, Sntp, Set, External };

// /sys/time: the clock, SNTP, the time zone, and setting the time from the
// browser (docs/DESIGN.md section 5.3).
class Time : public Service {
public:
    explicit Time(Device& dev) : Service(dev, "time") {}

    // Factory defaults, before Device::begin().
    Time& defaultTz(const char* posix);
    Time& defaultServer(const char* server);
    Time& sntp(bool on);

    void describe(tesser::Object& o) override;

    // Sets the clock from another source (GPS, RTC), which SNTP then
    // doesn't override. Takes the API lock: from onTick() or a task that may
    // wait for it, not from a control loop (docs/DESIGN.md section 5.3).
    void external(int64_t epochMs);
    bool synced() const { return synced_; }
    TimeSource source() const { return source_; }

    // Times before this aren't valid (the clock wasn't set).
    static constexpr int64_t kValidAfterMs = 1704067200000;  // 2024-01-01

    // Local time as ISO 8601 with its offset ("2026-10-10T18:30:00+02:00").
    static std::string iso8601(int64_t epochMs);

protected:
    void start() override;
    void tick(uint32_t nowMs) override;
    void event(const Event& e) override;

private:
    void applyTz();
    void applySntp();
    void setBy(TimeSource source);

    Clock* clock_ = nullptr;
    std::string tz_ = "UTC0";
    bool sntpOn_ = true;
    std::string server_ = "pool.ntp.org";
    bool synced_ = false;
    TimeSource source_ = TimeSource::None;
    tesser::Node* tzNode_ = nullptr;
    tesser::Node* sntpNode_ = nullptr;
    tesser::Node* serverNode_ = nullptr;
    tesser::Node* syncedNode_ = nullptr;
    tesser::Node* sourceNode_ = nullptr;
    uint32_t tzGen_ = 0;
    uint32_t sntpGen_ = 0;
    bool online_ = false;
    uint32_t routeGen_ = 0;  // Device::routeGeneration() last seen
};

}  // namespace tesserkit
