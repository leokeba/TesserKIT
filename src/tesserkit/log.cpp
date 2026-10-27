#include "tesserkit/log.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <memory>

#include "tesser/budget.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

constexpr size_t kMinRingBytes = 512;  // room for lines waiting for the task, whatever history()
constexpr int kLinesPerTick = 16;       // the rest wait for the next period

// The recent lines' download: a copy, freed with the transfer.
class Snapshot : public tesser::FileSource {
public:
    Snapshot(char* data, size_t size, tesser::Reservation held) : data_(data), size_(size), held_(std::move(held)) {}
    ~Snapshot() override { free(data_); }
    int64_t size() const override { return static_cast<int64_t>(size_); }
    int read(uint8_t* buf, size_t cap) override {
        size_t n = std::min(cap, size_ - pos_);
        memcpy(buf, data_ + pos_, n);
        pos_ += n;
        return static_cast<int>(n);
    }

private:
    char* data_;
    size_t size_;
    size_t pos_ = 0;
    tesser::Reservation held_;
};
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

    o.label("Log").ui("icon", "menu").ui("group", "Device").ui("summary", "level").doc("The firmware's log").readAccess(Access::Admin);
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
        .download([this](tesser::FileRequest& r) -> std::unique_ptr<tesser::FileSource> {
            // A copy: the ring goes on filling while the download is sent.
            // Refused when the heap can't hold it.
            size_t size = out_ ? out_->recentSize() : 0;
            tesser::Reservation held = tesser::reserveBytes(size + 1, true, tesser::mallocPool(size + 1));
            char* copy = held ? static_cast<char*>(malloc(size + 1)) : nullptr;
            if (!copy) {
                r.refuse(Status::Busy, "not enough free memory for the log now");
                return nullptr;
            }
            size_t n = out_ ? out_->recent(copy, size) : 0;
            held.settle();
            return std::unique_ptr<tesser::FileSource>(new Snapshot(copy, n, std::move(held)));
        })
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
    // One ring for the lines waiting for this task and the recent history.
    if (!out_->begin(std::max(historyBytes_, kMinRingBytes))) {
        tesser::logError("tesserkit: Log couldn't capture the log (no memory)");
    }
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
    for (int i = 0; i < kLinesPerTick && out_->next(line); i++) line_->emit(line);
    out_->mute(false);
    uint32_t d = out_->dropped();
    if (d != dropped_) {
        dropped_ = d;
        droppedNode_->changed();
    }
}

std::string Log::recent() const {
    if (!out_) return std::string();
    std::string s(out_->recentSize(), '\0');
    s.resize(out_->recent(&s[0], s.size()));
    return s;
}

}  // namespace tesserkit
