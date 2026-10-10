#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "tesser/client.h"
#include "tesserkit/device.h"

namespace tesserkit {

// A scheduled write: an element of /sys/timers.
struct Timer {
    char name[25] = "";
    bool enabled = true;
    char time[6] = "08:00";  // HH:MM, local time
    uint8_t days = 0x7F;     // bit 0 = Monday
    std::string path;        // the node to set or call
    std::string value = "null";  // its JSON value or argument
    std::string last;        // the last run's result: "ok" or the error
};

// /sys/timers and /sys/timersCatchUp (docs/DESIGN.md section 5.6). Mounted
// in /sys itself.
class Timers : public Service {
public:
    explicit Timers(Device& dev) : Service(dev, "timers") {}

    void attach(tesser::Object& sys) override { describe(sys); }
    // Describes the timers list and timersCatchUp into `o`.
    void describe(tesser::Object& o) override;

    // Factory defaults, before Device::begin().
    Timers& add(const Timer& t);
    Timers& catchUp(bool on);

    // Whether `path` names a node of the tree (or may, below a list, an
    // array or a remote node).
    bool resolves(const std::string& path) const;

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    void run(const Timer& t);
    void finished(const std::string& name, std::string result);

    std::vector<Timer> timers_;
    bool catchUp_ = false;
    bool started_ = false;
    bool caughtUp_ = false;
    tesser::Node* list_ = nullptr;
    uint32_t lastCheckMs_ = 0;
    std::map<std::string, int64_t> ran_;  // timer → the local minute it last ran in
    std::unique_ptr<tesser::LocalClient> client_;
};

}  // namespace tesserkit
