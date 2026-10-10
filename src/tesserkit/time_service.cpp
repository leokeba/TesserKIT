#include "tesserkit/time_service.h"

#include <stdio.h>
#include <time.h>

#include <utility>

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
// days_from_civil), to find the local offset without timegm().
int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

}  // namespace

std::string Time::iso8601(int64_t epochMs) {
    time_t t = static_cast<time_t>(epochMs / 1000);
    struct tm lt;
    if (!localtime_r(&t, &lt)) return "";
    int64_t local = daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400 + lt.tm_hour * 3600 +
                    lt.tm_min * 60 + lt.tm_sec;
    int64_t offset = (local - static_cast<int64_t>(t)) / 60;  // minutes east of UTC
    char sign = offset < 0 ? '-' : '+';
    if (offset < 0) offset = -offset;
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1,
             lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, sign, static_cast<int>(offset / 60),
             static_cast<int>(offset % 60));
    return buf;
}

Time& Time::defaultTz(const char* posix) {
    tz_ = posix ? posix : "UTC0";
    return *this;
}

Time& Time::defaultServer(const char* server) {
    server_ = server ? server : "";
    return *this;
}

Time& Time::sntp(bool on) {
    sntpOn_ = on;
    return *this;
}

void Time::describe(tesser::Object& o) {
    o.label("Time");
    o.value("now", [this] { return clock_ ? iso8601(clock_->epochMs()) : std::string(); })
        .label("Local time")
        .watch();
    o.value("epoch", [this] { return clock_ ? clock_->epochMs() / 1000 : int64_t(0); })
        .label("Unix time")
        .unit("s")
        .ui("advanced");
    syncedNode_ = &o.value("synced", static_cast<const bool&>(synced_));
    sourceNode_ = &o.value("source", source_, {"none", "sntp", "set", "external"}).readOnly();
    tzNode_ = &o.value("tz", [this] { return tz_; },
                       [this](const std::string& v) -> Check {
                           if (v.empty() || v.size() > 63) {
                               return Check::fail(Status::InvalidValue, "a POSIX TZ string of 1 to 63 characters");
                           }
                           tz_ = v;
                           return Check::ok();
                       })
                   .label("Time zone")
                   .writeAccess(Access::Admin)
                   .persist()
                   .ui("widget", "timezone")
                   .ui("placeholder", "CET-1CEST,M3.5.0,M10.5.0/3");
    auto& s = o.object("sntp").label("Network time").writeAccess(Access::Admin).ui("advanced");
    sntpNode_ = &s.value("enabled", sntpOn_).persist();
    serverNode_ = &s.value("server", server_).persist();
    o.action("set",
             [this](int64_t ms) -> Status {
                 if (ms < kValidAfterMs) return Status::InvalidValue;
                 if (!clock_) return Status::Internal;
                 clock_->setEpochMs(ms);
                 setBy(TimeSource::Set);
                 return Status::Ok;
             })
        .label("Set from this device")
        .writeAccess(Access::Admin)
        .ui("value", "$now");
}

void Time::start() {
    Platform& p = dev_.platform();
    if (!p.clock) p.clock = espClock();
    clock_ = p.clock;
    if (!clock_) {
        tesser::logError("tesserkit: Time has no clock");
        return;
    }
    Device& dev = dev_;
    clock_->begin([&dev](Event e) { dev.post(std::move(e)); });
    tzGen_ = tzNode_->generation();
    sntpGen_ = static_cast<uint32_t>(sntpNode_->generation()) + serverNode_->generation();
    applyTz();
    applySntp();
}

void Time::applyTz() { clock_->timezone(tz_.c_str()); }

void Time::applySntp() { clock_->sntp(sntpOn_ && source_ != TimeSource::External, server_.c_str()); }

void Time::setBy(TimeSource source) {
    source_ = source;
    sourceNode_->changed();
    if (!synced_) {
        synced_ = true;
        syncedNode_->changed();
    }
    dev_.timeSynced();
}

void Time::external(int64_t epochMs) {
    tesser::Lock lock(dev_.api());
    if (!clock_ || epochMs < kValidAfterMs) return;
    clock_->setEpochMs(epochMs);
    bool stopSntp = source_ != TimeSource::External;
    setBy(TimeSource::External);
    if (stopSntp) applySntp();  // SNTP doesn't override an external source
}

void Time::event(const Event& e) {
    if (e.type != Event::Type::TimeSynced || !clock_) return;
    if (source_ == TimeSource::External) return;
    setBy(TimeSource::Sntp);
}

void Time::tick(uint32_t nowMs) {
    (void)nowMs;
    if (!clock_) return;
    // Coming online: ask SNTP again now rather than at its next retry.
    bool online = dev_.state() == State::Online;
    if (online && !online_ && !synced_) applySntp();
    online_ = online;
    if (tzNode_->generation() != tzGen_) {
        tzGen_ = tzNode_->generation();
        applyTz();
    }
    uint32_t g = static_cast<uint32_t>(sntpNode_->generation()) + serverNode_->generation();
    if (g != sntpGen_) {
        sntpGen_ = g;
        applySntp();
    }
}

}  // namespace tesserkit
