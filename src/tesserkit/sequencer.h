#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tesser/client.h"
#include "tesserkit/device.h"

namespace tesserkit {

// What a step waits for before the next one (docs/DESIGN.md section 5.13).
enum class Wait : uint8_t {
    Reply,  // the set's reply (a deferred action's end); ms > 0: a timeout
    Time,   // ms after the step starts; 0: until stopped
    None,   // nothing: the next step starts at once
};

// A step of a sequence: an element of /sequencer/steps.
struct Step {
    char name[25] = "";          // optional; steps are also named by index
    std::string path;            // the node to set or call
    std::string value = "null";  // its JSON value or argument
    Wait wait = Wait::Reply;
    uint32_t ms = 0;
    std::string next;            // a name, an index, "+1" or "-2"; "" ends
};

// A list of timed steps, each a set on the tree, played one after another
// (docs/DESIGN.md section 5.13). Mounted at the root as /<name>, so an
// application can have several.
class Sequencer : public Service {
public:
    explicit Sequencer(Device& dev, const char* name = "sequencer") : Service(dev, name) {}

    void attach(tesser::Object& sys) override;
    void describe(tesser::Object& o) override;

    // Factory defaults, before Device::begin().
    Sequencer& add(const Step& s);
    Sequencer& boot(const char* step);  // played at boot ("" none)
    Sequencer& finally(const char* path, const char* value = "null");

    // Under the API lock (the device's task, or tesser::Lock). The step
    // starts in the next period. NotFound if `step` names none.
    tesser::Status play(std::string_view step);     // follows next
    tesser::Status execute(std::string_view step);  // this step only
    void stop();

    bool running() const { return mode_ != Mode::Idle; }
    int current() const { return current_; }  // the step's index, -1 idle
    // The index of the step `ref` names: a name (the first step with it),
    // an index, or (with `from`) "+n" / "-n" from that step. -1 if none.
    int find(std::string_view ref, int from = -1) const;
    const std::vector<Step>& steps() const { return steps_; }

protected:
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    enum class Mode : uint8_t { Idle, Chain, Single };

    tesser::Status request(std::string_view step, Mode mode);
    void begin(int index, uint32_t nowMs);
    bool waited(uint32_t nowMs);
    // When the running step, whose wait is over, ended: where the next starts.
    uint32_t endedMs(uint32_t nowMs) const;
    void end(bool runFinally);
    void failed(const std::string& what, const std::string& error);
    std::string label(int index) const;

    std::vector<Step> steps_;
    std::string boot_;
    std::string finallyPath_;
    std::string finallyValue_ = "null";
    Mode mode_ = Mode::Idle;
    int current_ = -1;
    uint32_t startedMs_ = 0;
    uint32_t nowMs_ = 0;
    uint32_t token_ = 0;  // the running step's; later replies to earlier steps are stale
    bool replied_ = false;
    bool timedOut_ = false;
    // Requests from actions, applied in the next period.
    bool stopPending_ = false;
    int startPending_ = -1;
    Mode startMode_ = Mode::Idle;
    bool started_ = false;
    bool booted_ = false;
    std::string error_;
    tesser::EventNode* failed_ = nullptr;
    std::unique_ptr<tesser::LocalClient> client_;
};

}  // namespace tesserkit
