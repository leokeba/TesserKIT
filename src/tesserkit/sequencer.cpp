#include "tesserkit/sequencer.h"

#include <string.h>

#include "tesserkit/limits.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

bool digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool relative(std::string_view s) { return s.size() > 1 && (s[0] == '+' || s[0] == '-') && digits(s.substr(1)); }

// A step's name: anything an index or a relative reference can't be.
// A step's name (24 characters at most), its index, or "+1", "-2".
constexpr size_t kStepRef = 24;
// The device's period: a step noticed later than this after its end (a
// stalled task) isn't caught up.
constexpr uint32_t kPeriodMs = 20;

// A step's write isn't saved: a looping sequence on a persisted node
// rewrote its record every 2 s (docs/DESIGN.md section 5.13).
tesser::Query unsaved() {
    tesser::Query q;
    q.persist = false;
    return q;
}

bool nameOk(std::string_view s) { return s.empty() || !(s[0] == '+' || s[0] == '-' || (s[0] >= '0' && s[0] <= '9')); }

int toIndex(std::string_view s) {
    if (s.size() > 4) return -1;  // beyond any list
    int n = 0;
    for (char c : s) n = n * 10 + (c - '0');
    return n;
}

Check jsonOk(const std::string& v) {
    if (v.size() > limits::kValue) return limits::length<limits::kValue>(v);
    JsonDocument doc;
    return deserializeJson(doc, v) ? Check::fail(Status::InvalidValue, "not JSON (an action without argument: null)")
                                   : Check::ok();
}

}  // namespace

Sequencer& Sequencer::add(const Step& s) {
    steps_.push_back(s);
    return *this;
}

Sequencer& Sequencer::boot(const char* step) {
    boot_ = step ? step : "";
    return *this;
}

Sequencer& Sequencer::finally(const char* path, const char* value) {
    finallyPath_ = path ? path : "";
    finallyValue_ = value ? value : "null";
    return *this;
}

int Sequencer::find(std::string_view ref, int from) const {
    int n = static_cast<int>(steps_.size());
    int i = -1;
    if (relative(ref)) {
        if (from < 0) return -1;
        int d = toIndex(ref.substr(1));
        if (d < 0) return -1;
        i = ref[0] == '+' ? from + d : from - d;
    } else if (digits(ref)) {
        i = toIndex(ref);
    } else if (!ref.empty()) {
        for (int k = 0; k < n; k++) {
            if (ref == steps_[static_cast<size_t>(k)].name) return k;
        }
    }
    return i >= 0 && i < n ? i : -1;
}

void Sequencer::attach(tesser::Object& sys) {
    (void)sys;  // the application's: at the root, beside its own tree
    describe(dev_.api().object(name()));
}

void Sequencer::describe(tesser::Object& o) {
    o.label("Sequencer").doc("Steps played one after another, each a write to the tree").ui("icon", "play");
    o.list("steps", steps_,
           [this](tesser::Object& e, Step& s) {
               e.value("name", s.name)
                   .validate([](const std::string& v) {
                       return nameOk(v) ? Check::ok()
                                        : Check::fail(Status::InvalidValue, "can't start with a digit, + or -");
                   })
                   .doc("Optional: steps are also named by their index");
               e.value("path", s.path)
                   .validate([this](const std::string& v) {
                       // Until the service starts, the tree may be
                       // incomplete: a boot load keeps what it stored.
                       if (v.size() > limits::kPath) return limits::length<limits::kPath>(v);
                       return !started_ || resolves(dev_.api(), v) ? Check::ok()
                                                                    : Check::fail(Status::NotFound, "no such node");
                   })
                   .ui("placeholder", "/led/on");
               e.value("value", s.value).validate(jsonOk).ui("placeholder", "true");
               e.value("wait", s.wait, {"reply", "time", "none"})
                   .doc("reply: until the write's reply (ms: a timeout); time: ms (0: until stopped); none: go on at once");
               e.value("ms", s.ms).range(0, 86400000).unit("ms");
               e.value("next", s.next)
                   .validate([](const std::string& v) {
                       if (v.size() > kStepRef) return limits::length<kStepRef>(v);
                       return v.empty() || digits(v) || relative(v) || nameOk(v)
                                  ? Check::ok()
                                  : Check::fail(Status::InvalidValue, "a step's name, its index, or +1, -2...");
                   })
                   .doc("The next step: a name, an index, or +1, -2... from this one. Empty: the end")
                   .ui("placeholder", "+1");
           })
        .maxSize(32)
        .writeAccess(Access::Admin)
        .persist()
        .label("Steps");
    o.value("boot", boot_)
        .validate([](const std::string& v) {
            if (v.size() > kStepRef) return limits::length<kStepRef>(v);
            return v.empty() || digits(v) || nameOk(v) ? Check::ok()
                                                       : Check::fail(Status::InvalidValue, "a step's name or index");
        })
        .label("Play at boot")
        .doc("The step to play from at boot: a name or an index. Empty: none")
        .writeAccess(Access::Admin)
        .persist();
    auto& f = o.object("finally")
                  .label("Finally")
                  .doc("Set when playing stops (stop, the end, or an executed step), unless another play takes over")
                  .writeAccess(Access::Admin)
                  .persist()
                  .ui("advanced");
    f.value("path", finallyPath_)
        .validate([this](const std::string& v) {
            if (v.size() > limits::kPath) return limits::length<limits::kPath>(v);
            return v.empty() || !started_ || resolves(dev_.api(), v) ? Check::ok()
                                                                      : Check::fail(Status::NotFound, "no such node");
        })
        .ui("placeholder", "/led/on");
    f.value("value", finallyValue_).validate(jsonOk).ui("placeholder", "false");
    o.action("play", [this](std::string step) {
         Status s = request(step, Mode::Chain);
         return s == Status::Ok ? Check::ok() : Check::fail(s, "no such step");
     })
        .label("Play")
        .doc("From a step (a name or an index; empty: the first), following next")
        .writeAccess(Access::User);
    o.action("execute", [this](std::string step) {
         Status s = request(step, Mode::Single);
         return s == Status::Ok ? Check::ok() : Check::fail(s, "no such step");
     })
        .label("Execute")
        .doc("One step (a name or an index), without following next")
        .writeAccess(Access::User)
        .ui("advanced");
    o.action("stop", [this] { stop(); }).label("Stop").writeAccess(Access::User);
    auto& st = o.object("status").label("Status");
    st.value("running", [this] { return running(); }).watch();
    st.value("step", [this] { return current_; }).doc("The running step's index, -1 when stopped").watch();
    st.value("name", [this] { return current_ >= 0 ? label(current_) : std::string(); }).watch();
    st.value("elapsed", [this] { return running() ? nowMs_ - startedMs_ : 0u; }).unit("ms").watch();
    st.value("duration",
             [this] {
                 return running() && static_cast<size_t>(current_) < steps_.size() &&
                                steps_[static_cast<size_t>(current_)].wait != Wait::None
                            ? steps_[static_cast<size_t>(current_)].ms
                            : 0u;
             })
        .unit("ms")
        .doc("What the step waits at most, 0: no limit")
        .watch();
    st.value("error", [this] { return error_; }).label("Last error").watch();
    failed_ = &o.event("failed").label("Step failed").ui("toast", "warn").ui("hidden");
}

void Sequencer::start() {
    client_.reset(new tesser::LocalClient(dev_.api()));  // Admin: steps run with full access
    started_ = true;
}

Status Sequencer::play(std::string_view step) { return request(step, Mode::Chain); }

Status Sequencer::execute(std::string_view step) { return request(step, Mode::Single); }

void Sequencer::stop() {
    startPending_ = -1;
    stopPending_ = true;
}

Status Sequencer::request(std::string_view step, Mode mode) {
    int i = step.empty() ? (steps_.empty() ? -1 : 0) : find(step);
    if (i < 0) return Status::NotFound;
    startPending_ = i;
    startMode_ = mode;
    return Status::Ok;
}

void Sequencer::tick(uint32_t nowMs) {
    nowMs_ = nowMs;
    if (!client_) return;
    client_->process();
    if (!booted_) {
        booted_ = true;
        if (!boot_.empty()) {
            int i = find(boot_);
            if (i < 0) {
                failed("boot", "no step \"" + boot_ + "\"");
            } else if (startPending_ < 0 && !stopPending_) {
                startPending_ = i;
                startMode_ = Mode::Chain;
            }
        }
    }
    if (stopPending_) {
        stopPending_ = false;
        if (running()) end(true);
    }
    if (startPending_ >= 0) {
        int i = startPending_;
        startPending_ = -1;
        mode_ = startMode_;  // a play while playing takes over: no finally
        begin(i, nowMs);
    }
    // A path through the steps without a wait visits each at most once: more
    // starts in one period is a loop.
    size_t starts = 0;
    while (running() && waited(nowMs)) {
        if (mode_ == Mode::Single || static_cast<size_t>(current_) >= steps_.size()) {
            end(true);
            break;
        }
        std::string next = steps_[static_cast<size_t>(current_)].next;
        if (next.empty()) {
            end(true);
            break;
        }
        int i = find(next, current_);
        if (i < 0) {
            failed(label(current_), "no step \"" + next + "\"");
            end(true);
            break;
        }
        if (++starts > steps_.size()) {
            failed(label(i), "the steps loop without waiting");
            end(true);
            break;
        }
        begin(i, endedMs(nowMs));
    }
}

uint32_t Sequencer::endedMs(uint32_t nowMs) const {
    // A timed step ended at its deadline, which the period that noticed it
    // passed by up to a period: the next one starts from there, so a show
    // of timed steps doesn't drift by a period each. A step that doesn't
    // wait ended when it started (itself perhaps at a deadline). A reply
    // ends a step when it is noticed.
    const Step& s = steps_[static_cast<size_t>(current_)];
    uint32_t end = nowMs;
    if (s.wait == Wait::Time) end = startedMs_ + s.ms;
    if (s.wait == Wait::None) end = startedMs_;
    return nowMs - end > kPeriodMs ? nowMs : end;
}

void Sequencer::begin(int index, uint32_t nowMs) {
    if (index < 0 || static_cast<size_t>(index) >= steps_.size()) {
        end(false);
        return;
    }
    current_ = index;
    startedMs_ = nowMs;
    replied_ = false;
    timedOut_ = false;
    uint32_t token = ++token_;
    const Step& s = steps_[static_cast<size_t>(index)];
    std::string what = label(index);
    bool sent = client_->set(s.path, s.value, [this, token, what](Status st, JsonVariantConst body) {
        if (st != Status::Ok) {
            std::string r = tesser::toString(st);
            if (const char* m = body["message"]) r = r + ": " + m;
            failed(what, r);
        }
        if (token == token_) replied_ = true;
    }, unsaved());
    if (!sent) {
        failed(what, "not sent");
        replied_ = true;
    }
    client_->process();  // a reply that is already there counts now
}

bool Sequencer::waited(uint32_t nowMs) {
    if (static_cast<size_t>(current_) >= steps_.size()) return true;  // the list shrank
    const Step& s = steps_[static_cast<size_t>(current_)];
    uint32_t elapsed = nowMs - startedMs_;
    switch (s.wait) {
        case Wait::None: return true;
        case Wait::Time: return s.ms > 0 && elapsed >= s.ms;
        case Wait::Reply:
            if (replied_) return true;
            if (s.ms > 0 && elapsed >= s.ms) {
                if (!timedOut_) failed(label(current_), "no reply in " + std::to_string(s.ms) + " ms");
                timedOut_ = true;
                return true;
            }
            return false;
    }
    return true;
}

void Sequencer::end(bool runFinally) {
    mode_ = Mode::Idle;
    current_ = -1;
    ++token_;  // replies still to come belong to no running step
    if (!runFinally || finallyPath_.empty()) return;
    bool sent = client_->set(finallyPath_, finallyValue_, [this](Status st, JsonVariantConst body) {
        if (st == Status::Ok) return;
        std::string r = tesser::toString(st);
        if (const char* m = body["message"]) r = r + ": " + m;
        failed("finally", r);
    }, unsaved());
    if (!sent) failed("finally", "not sent");
}

void Sequencer::failed(const std::string& what, const std::string& error) {
    error_ = what + ": " + error;
    tesser::logWarning("tesserkit: %s: %s", name(), error_.c_str());
    if (failed_) failed_->emit(error_);
}

std::string Sequencer::label(int index) const {
    if (index >= 0 && static_cast<size_t>(index) < steps_.size() && steps_[static_cast<size_t>(index)].name[0])
        return steps_[static_cast<size_t>(index)].name;
    return "#" + std::to_string(index);
}

}  // namespace tesserkit
