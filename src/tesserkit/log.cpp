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

constexpr size_t kCaptureBytes = 2048;  // lines waiting for the task
constexpr int kLinesPerTick = 16;        // the rest wait for the next period

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
            // A copy taken under the API lock: the ring goes on filling
            // while the download is sent. Refused when the heap can't hold it.
            tesser::Reservation held = tesser::reserveBytes(kept_ + 1, true, tesser::mallocPool(kept_ + 1));
            char* copy = held ? static_cast<char*>(malloc(kept_ + 1)) : nullptr;
            if (!copy) {
                r.refuse(Status::Busy, "not enough free memory for the log now");
                return nullptr;
            }
            copyRing(copy);
            held.settle();
            return std::unique_ptr<tesser::FileSource>(new Snapshot(copy, kept_, std::move(held)));
        })
        .ui("filename", "log.txt");
    droppedNode_ = &o.value("dropped", static_cast<const uint32_t&>(dropped_))
                        .label("Lines dropped")
                        .doc("Lost while too many came at once")
                        .ui("advanced");
}

Log::~Log() { free(ring_); }

void Log::start() {
    if (historyBytes_ && !ring_) {
        ring_ = static_cast<char*>(malloc(historyBytes_));
        if (!ring_) tesser::logError("tesserkit: Log couldn't keep recent lines (no memory)");
    }
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
    if (!ring_) return;
    size_t n = std::min(line.size(), historyBytes_ - 1) + 1;  // with its newline
    // Room by dropping the oldest lines whole.
    while (kept_ + n > historyBytes_) {
        size_t i = 0;
        while (i < kept_ && ring_[(head_ + i) % historyBytes_] != '\n') i++;
        i = std::min(i + 1, kept_);
        head_ = (head_ + i) % historyBytes_;
        kept_ -= i;
    }
    size_t at = (head_ + kept_) % historyBytes_;
    for (size_t i = 0; i + 1 < n; i++) ring_[(at + i) % historyBytes_] = line[i];
    ring_[(at + n - 1) % historyBytes_] = '\n';
    kept_ += n;
}

void Log::copyRing(char* out) const {
    for (size_t i = 0; i < kept_; i++) out[i] = ring_[(head_ + i) % historyBytes_];
}

std::string Log::recent() const {
    std::string s(kept_, '\0');
    if (kept_) copyRing(&s[0]);
    return s;
}

}  // namespace tesserkit
