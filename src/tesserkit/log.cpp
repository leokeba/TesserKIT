#include "tesserkit/log.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr size_t kCaptureBytes = 2048;  // lines waiting for the task
constexpr int kLinesPerTick = 16;        // the rest wait for the next period
const char* const kLevels[] = {"none", "error", "warn", "info", "debug", "verbose"};

}  // namespace

Log& Log::history(size_t bytes) {
    historyBytes_ = bytes;
    return *this;
}

Log& Log::level(LogLevel l) {
    level_ = l;
    levelSet_ = true;
    return *this;
}

void Log::describe(tesser::Object& o) {
    Platform& p = dev_.platform();
    if (!p.log) p.log = espLog();
    out_ = p.log;
    if (out_ && !levelSet_) level_ = static_cast<LogLevel>(out_->level());
    LogOutput* out = out_;

    o.label("Log").ui("icon", "menu").doc("The firmware's log").readAccess(Access::Admin);
    levelNode_ = &o.value("level", level_, {"none", "error", "warn", "info", "debug", "verbose"})
                      .validate([out](JsonVariantConst v) {
                          uint8_t max = out ? out->maxLevel() : 5;
                          for (uint8_t i = max + 1; i < 6; i++) {
                              if (v == kLevels[i]) {
                                  return Check::fail(Status::InvalidValue,
                                                     "above what the firmware was built with (CONFIG_LOG_MAXIMUM_LEVEL)");
                              }
                          }
                          return Check::ok();
                      })
                      .persist()
                      .doc("What the firmware logs, on its console too");
    line_ = &o.event("line").label("Lines").ui("widget", "log").ui("lines", 200);
    o.file("recent")
        .label("Recent lines")
        .contentType("text/plain")
        .text([this] { return recent(); }, nullptr)
        .ui("filename", "log.txt");
    droppedNode_ = &o.value("dropped", static_cast<const uint32_t&>(dropped_))
                        .label("Lines dropped")
                        .doc("Lost while too many came at once")
                        .ui("advanced");
}

void Log::start() {
    if (!out_) {
        tesser::logError("tesserkit: Log has no log output in this build");
        return;
    }
    if (static_cast<uint8_t>(level_) > out_->maxLevel()) level_ = static_cast<LogLevel>(out_->maxLevel());
    out_->level(static_cast<uint8_t>(level_));
    levelGen_ = levelNode_->generation();
    if (!out_->begin(kCaptureBytes)) tesser::logError("tesserkit: Log couldn't capture the log");
}

void Log::tick(uint32_t nowMs) {
    (void)nowMs;
    if (!out_) return;
    if (levelNode_->generation() != levelGen_) {
        levelGen_ = levelNode_->generation();
        out_->level(static_cast<uint8_t>(level_));
    }
    // What this emits must not come back as more lines: a transport warning
    // per event would feed itself.
    out_->mute(true);
    std::string line;
    for (int i = 0; i < kLinesPerTick && out_->next(line); i++) {
        keep(line);
        line_->emit(line);
    }
    out_->mute(false);
    uint32_t d = out_->dropped();
    if (d != dropped_) {
        dropped_ = d;
        droppedNode_->changed();
    }
}

void Log::keep(const std::string& line) {
    if (!historyBytes_) return;
    lines_.push_back(line);
    kept_ += line.size() + 1;
    while (kept_ > historyBytes_ && !lines_.empty()) {
        kept_ -= lines_.front().size() + 1;
        lines_.pop_front();
    }
}

std::string Log::recent() const {
    std::string s;
    s.reserve(kept_);
    for (const std::string& l : lines_) s.append(l).push_back('\n');
    return s;
}

}  // namespace tesserkit
