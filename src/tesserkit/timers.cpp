#include "tesserkit/timers.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "tesserkit/time_service.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

bool parseHhMm(const char* s, int& h, int& m) {
    if (strlen(s) != 5 || s[2] != ':') return false;
    for (int i : {0, 1, 3, 4}) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    h = (s[0] - '0') * 10 + (s[1] - '0');
    m = (s[3] - '0') * 10 + (s[4] - '0');
    return h < 24 && m < 60;
}

}  // namespace

Timers& Timers::add(const Timer& t) {
    timers_.push_back(t);
    return *this;
}

Timers& Timers::catchUp(bool on) {
    catchUp_ = on;
    return *this;
}

bool Timers::resolves(const std::string& path) const { return tesserkit::resolves(dev_.api(), path); }

void Timers::describe(tesser::Object& o) {
    list_ = &o.list("timers", timers_,
                    [this](tesser::Object& e, Timer& t) {
                        e.value("name", t.name);
                        e.value("enabled", t.enabled);
                        e.value("time", t.time)
                            .validate([](const std::string& v) {
                                int h, m;
                                return parseHhMm(v.c_str(), h, m)
                                           ? Check::ok()
                                           : Check::fail(Status::InvalidValue, "HH:MM, 00:00 to 23:59");
                            })
                            .ui("placeholder", "08:00");
                        e.value("days", t.days)
                            .range(0, 127)
                            .ui("widget", "flags")
                            .ui("labels", "Mon,Tue,Wed,Thu,Fri,Sat,Sun");
                        e.value("path", t.path)
                            .validate([this](const std::string& v) {
                                // Until the service starts, the tree may be
                                // incomplete: a boot load keeps what it stored.
                                return !started_ || resolves(v) ? Check::ok()
                                                                : Check::fail(Status::NotFound, "no such node");
                            })
                            .ui("placeholder", "/led/on");
                        e.value("value", t.value)
                            .validate([](const std::string& v) {
                                JsonDocument doc;
                                return deserializeJson(doc, v)
                                           ? Check::fail(Status::InvalidValue,
                                                         "not JSON (an action without argument: null)")
                                           : Check::ok();
                            })
                            .ui("placeholder", "true");
                        // Read-only, not const: a backup or a boot restores it.
                        e.value("last", t.last).readOnly().label("Last run");
                    })
                 .key("name")
                 .maxSize(16)
                 .writeAccess(Access::Admin)
                 .persist()
                 .label("Timers")
                 .ui("icon", "play");
    o.value("timersCatchUp", catchUp_)
        .label("Run missed timers at boot")
        .doc("At boot, each timer's most recent missed occurrence of today runs once")
        .writeAccess(Access::Admin)
        .persist()
        .ui("advanced");
    failed_ = &o.event("timerFailed").label("Timer failed").ui("toast", "warn").ui("hidden");
}

void Timers::start() {
    Platform& p = dev_.platform();
    if (!p.clock) p.clock = espClock();
    client_.reset(new tesser::LocalClient(dev_.api()));  // Admin: timers run with full access
    started_ = true;
}

void Timers::tick(uint32_t nowMs) {
    if (client_) client_->process();
    if (nowMs - lastCheckMs_ < 1000) return;
    lastCheckMs_ = nowMs;
    Clock* clock = dev_.platform().clock;
    if (!clock) return;
    int64_t ms = clock->epochMs();
    if (ms < Time::kValidAfterMs) return;  // timers wait for a valid time
    time_t t = static_cast<time_t>(ms / 1000);
    struct tm lt;
    if (!localtime_r(&t, &lt)) return;
    int64_t day = static_cast<int64_t>(lt.tm_year) * 400 + lt.tm_yday;  // a key per local date
    int now = lt.tm_hour * 60 + lt.tm_min;
    uint8_t today = static_cast<uint8_t>(1u << ((lt.tm_wday + 6) % 7));  // Monday = bit 0
    bool catchUp = !caughtUp_ && catchUp_;
    caughtUp_ = true;
    for (const Timer& timer : timers_) {
        int h, m;
        if (!timer.enabled || !(timer.days & today) || !parseHhMm(timer.time, h, m)) continue;
        int at = h * 60 + m;
        // Due now, or (catching up at boot) missed earlier today.
        if (at != now && !(catchUp && at < now)) continue;
        int64_t key = day * 1440 + at;
        auto it = ran_.find(timer.name);
        if (it != ran_.end() && it->second == key) continue;  // at most once a minute
        ran_[timer.name] = key;
        run(timer);
    }
}

void Timers::run(const Timer& t) {
    std::string name = t.name;
    tesser::logWarning("tesserkit: timer \"%s\": set %s %s", t.name, t.path.c_str(), t.value.c_str());
    bool sent = client_->set(t.path, t.value, [this, name](Status s, JsonVariantConst body) {
        if (s == Status::Ok) return finished(name, "ok");
        std::string r = tesser::toString(s);
        if (const char* m = body["message"]) r = r + ": " + m;
        finished(name, r);
    });
    if (!sent) finished(name, "not sent");
}

void Timers::finished(const std::string& name, std::string result) {
    for (Timer& t : timers_) {
        if (name != t.name) continue;
        if (result != "ok") {
            tesser::logWarning("tesserkit: timer \"%s\": %s", t.name, result.c_str());
            failed_->emit(std::string(t.name) + ": " + result);
        }
        t.last = std::move(result);
        list_->changed();
        return;
    }
}

}  // namespace tesserkit
