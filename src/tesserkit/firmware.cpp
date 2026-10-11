#include "tesserkit/firmware.h"

#include <ctype.h>
#include <string.h>

#include <ArduinoJson.h>

#include <memory>

#include "tesserkit/image.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

// What an HTTPS connection takes from the heap at its peak, its task
// included: about 34 KB measured on a classic ESP32 fetching a GitHub
// release, with a margin. Below it the handshake would fail, or leave the
// rest of the firmware without memory.
constexpr uint32_t kTlsHeap = 44 * 1024;

Check tlsRoom(Device& dev, const std::string& url) {
    if (url.compare(0, 8, "https://") != 0 || dev.core().freeHeap() >= kTlsHeap) return Check::ok();
    return Check::fail(Status::Busy, "not enough free memory for HTTPS now (about 44 KB needed)");
}

// An upload to /sys/firmware/image, streamed to the next OTA slot. It runs
// in the HTTP server's task, outside the API lock.
class ImageUpload : public tesser::FileSink {
public:
    ImageUpload(Firmware& fw, Ota& ota, uint16_t chipId, uint32_t size) : fw_(fw), writer_(ota, chipId) {
        writer_.begin(size);
    }
    ~ImageUpload() override {
        if (!done_) fw_.failed(error_ ? error_ : "upload interrupted");
    }
    Check write(const uint8_t* data, size_t len) override {
        Check c = writer_.write(data, len);
        if (!c.isOk()) {
            error_ = c.message;
            return c;
        }
        fw_.progress(writer_.received(), writer_.size());
        return c;
    }
    Check finish(tesser::JsonWriter& reply) override {
        (void)reply;
        fw_.verifying();
        Check c = writer_.finish();
        if (!c.isOk()) {
            error_ = c.message;
            return c;
        }
        done_ = true;
        fw_.installed();
        return c;
    }

private:
    Firmware& fw_;
    ImageWriter writer_;
    const char* error_ = nullptr;
    bool done_ = false;
};

constexpr size_t kChunk = 2048;

// A version parsed for compareVersions().
struct Version {
    uint32_t num[3] = {0, 0, 0};
    std::string pre;   // prerelease identifiers, without the '-'
    bool dev = false;  // a git describe suffix: after the tag
};

bool allOf(const std::string& s, size_t from, size_t to, int (*pred)(int)) {
    if (from >= to) return false;
    for (size_t i = from; i < to; i++)
        if (!pred(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

bool parseVersion(const char* text, Version& v) {
    std::string s = text ? text : "";
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    size_t plus = s.find('+');
    if (plus != std::string::npos) s.erase(plus);
    if (s.size() > 6 && s.compare(s.size() - 6, 6, "-dirty") == 0) {
        s.resize(s.size() - 6);
        v.dev = true;
    }
    // git describe: "<tag>-<commits>-g<hash>".
    size_t g = s.rfind("-g");
    if (g != std::string::npos && g > 0 && allOf(s, g + 2, s.size(), isxdigit)) {
        size_t dash = s.rfind('-', g - 1);
        if (dash != std::string::npos && allOf(s, dash + 1, g, isdigit)) {
            s.erase(dash);
            v.dev = true;
        }
    }
    size_t i = 0;
    for (int k = 0; k < 3; k++) {
        size_t start = i;
        while (i < s.size() && isdigit(static_cast<unsigned char>(s[i])) && i - start < 9) {
            v.num[k] = v.num[k] * 10 + static_cast<uint32_t>(s[i] - '0');
            i++;
        }
        if (i == start) return false;
        if (k < 2 && i < s.size() && s[i] == '.') {
            i++;
            continue;
        }
        break;
    }
    if (i == s.size()) return true;
    if (s[i] != '-' || i + 1 == s.size()) return false;
    v.pre = s.substr(i + 1);
    return true;
}

std::vector<std::string> identifiers(const std::string& pre) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t dot = pre.find('.', start);
        out.push_back(pre.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) return out;
        start = dot + 1;
    }
}

int sign(int c) { return c < 0 ? -1 : c > 0 ? 1 : 0; }

// Semver's precedence of prerelease identifiers: numbers numerically and
// before words, words in ASCII order, a shorter list first.
int comparePrerelease(const std::string& a, const std::string& b) {
    std::vector<std::string> x = identifiers(a), y = identifiers(b);
    for (size_t k = 0; k < x.size() && k < y.size(); k++) {
        bool nx = allOf(x[k], 0, x[k].size(), isdigit), ny = allOf(y[k], 0, y[k].size(), isdigit);
        if (nx != ny) return nx ? -1 : 1;
        if (nx && x[k].size() != y[k].size()) return x[k].size() < y[k].size() ? -1 : 1;
        int c = x[k].compare(y[k]);
        if (c) return sign(c);
    }
    return x.size() == y.size() ? 0 : x.size() < y.size() ? -1 : 1;
}

// An HTTP response as an ArduinoJson reader.
class BodyReader {
public:
    explicit BodyReader(HttpResponse& r) : r_(r) {}
    int read() {
        if (pos_ == len_ && !fill()) return -1;
        return buf_[pos_++];
    }
    size_t readBytes(char* out, size_t n) {
        size_t i = 0;
        for (int c; i < n && (c = read()) >= 0;) out[i++] = static_cast<char>(c);
        return i;
    }
    bool failed() const { return failed_; }

private:
    bool fill() {
        if (failed_ || ended_) return false;
        int n = r_.read(buf_, sizeof(buf_));
        if (n <= 0) {
            (n < 0 ? failed_ : ended_) = true;
            return false;
        }
        len_ = static_cast<size_t>(n);
        pos_ = 0;
        return true;
    }

    HttpResponse& r_;
    uint8_t buf_[256];
    size_t pos_ = 0, len_ = 0;
    bool failed_ = false, ended_ = false;
};

void replaceAll(std::string& s, const char* what, const std::string& with) {
    size_t n = strlen(what);
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size()))
        s.replace(at, n, with);
}

}  // namespace

int compareVersions(const char* a, const char* b, bool* ok) {
    Version x, y;
    bool good = parseVersion(a, x) && parseVersion(b, y);
    if (ok) *ok = good;
    if (!good) return 0;
    for (int k = 0; k < 3; k++)
        if (x.num[k] != y.num[k]) return x.num[k] < y.num[k] ? -1 : 1;
    if (x.pre.empty() != y.pre.empty()) return x.pre.empty() ? 1 : -1;
    int c = comparePrerelease(x.pre, y.pre);
    if (c) return c;
    return x.dev == y.dev ? 0 : x.dev ? 1 : -1;
}

// Checks the latest release of a repository and installs it.
class Firmware::GithubFeed : public Firmware::Feed {
public:
    GithubFeed(Firmware& fw, std::string repo, std::string asset)
        : fw_(fw), repo_(std::move(repo)), asset_(std::move(asset)) {}

    void describe(tesser::Object& o) override;
    void tick(uint32_t nowMs) override;
    ReleaseState state() const override { return state_; }

private:
    // What a check found.
    struct Latest {
        Status status = Status::Ok;
        std::string error;  // when status isn't Ok
        std::string tag, assetUrl;
    };
    // Asks `url` for the latest release and finds its asset named after
    // `pattern`, in the check's task.
    static Latest fetch(HttpClient& http, const std::string& url, const std::string& pattern,
                        const std::string& target);
    bool idle() const { return fw_.state_ == FirmwareState::Idle || fw_.state_ == FirmwareState::Failed; }
    // Starts a check in a task.
    void check();
    // The check's outcome, from its task.
    void checked(const Latest& r);
    void setState(ReleaseState s);

    Firmware& fw_;
    std::string repo_, asset_;
    std::string latest_, error_, assetUrl_;
    ReleaseState state_ = ReleaseState::Unchecked;
    bool autoCheck_ = true;
    bool checkDue_ = true;
    uint32_t nextCheckMs_ = 0;
    std::vector<tesser::Pending> waiters_;
    tesser::Node* latestNode_ = nullptr;
    tesser::Node* stateNode_ = nullptr;
    tesser::Node* errorNode_ = nullptr;
    tesser::EventNode* newRelease_ = nullptr;
    std::string announced_;  // the last release told of
};

void Firmware::GithubFeed::describe(tesser::Object& o) {
    auto& g = o.object("github").label("Releases");
    g.value("repo", static_cast<const std::string&>(repo_)).label("Repository").ui("advanced");
    latestNode_ = &g.value("latest", static_cast<const std::string&>(latest_)).label("Latest release");
    stateNode_ = &g.value("status", state_, {"unchecked", "checking", "current", "available", "failed"})
                      .readOnly()
                      .ui("labels", "Not checked,Checking,Up to date,Update available,Check failed");
    errorNode_ = &g.value("error", static_cast<const std::string&>(error_)).ui("visibleWhen", "status=failed");
    newRelease_ = &g.event("newRelease").label("New release").ui("toast", "info").ui("hidden");
    g.value("autoCheck", autoCheck_).persist().writeAccess(Access::Admin).label("Check daily");
    g.action("check", [this](tesser::Call& call) {
         if (!idle()) {
             call.fail(Status::Busy, "an update is in progress");
             return;
         }
         tesser::Pending p = call.defer();
         if (!p) return;
         waiters_.push_back(std::move(p));
         if (state_ != ReleaseState::Checking) check();
     }).label("Check now");
    g.action("update", [this]() -> Check {
         if (state_ != ReleaseState::Available) return Check::fail(Status::NotAllowed, "no newer release");
         return fw_.download(assetUrl_);
     })
        .label("Install")
        .writeAccess(Access::Admin)
        .ui("confirm", "Install the latest release and restart?")
        .ui("style", "primary")
        .ui("visibleWhen", "status=available");
}

void Firmware::GithubFeed::tick(uint32_t nowMs) {
    if (!stateNode_ || !autoCheck_ || !idle() || state_ == ReleaseState::Checking) return;
    if (fw_.dev_.onlineMs() < kFirstCheckMs) return;
    if (checkDue_ || static_cast<int32_t>(nowMs - nextCheckMs_) >= 0) check();
}

Firmware::GithubFeed::Latest Firmware::GithubFeed::fetch(HttpClient& http, const std::string& url,
                                                         const std::string& pattern, const std::string& target) {
    Latest out;
    auto fail = [&out](Status s, std::string message) {
        out.status = s;
        out.error = std::move(message);
        return Check::ok();  // the error is in `out`
    };
    Check c = http.get(url.c_str(), [&](HttpResponse& res) -> Check {
        int status = res.status();
        if (status == 404) return fail(Status::NotFound, "no release found");
        if (status == 403 || status == 429) return fail(Status::Busy, "GitHub's rate limit: try again later");
        if (status != 200) return fail(Status::NotFound, "the server refused the request");
        JsonDocument filter;
        filter["tag_name"] = true;
        filter["assets"][0]["name"] = true;
        filter["assets"][0]["browser_download_url"] = true;
        JsonDocument doc;
        BodyReader reader(res);
        DeserializationError e = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
        if (reader.failed()) return fail(Status::Timeout, "answer cut short");
        if (e == DeserializationError::NoMemory) return fail(Status::Internal, "no memory for the answer");
        const char* tag = doc["tag_name"];
        if (e || !tag || !*tag) return fail(Status::Internal, "unreadable answer");
        out.tag = tag;
        std::string name = pattern;
        replaceAll(name, "{target}", target);
        replaceAll(name, "{version}", out.tag[0] == 'v' || out.tag[0] == 'V' ? out.tag.substr(1) : out.tag);
        for (JsonObjectConst a : doc["assets"].as<JsonArrayConst>()) {
            const char* n = a["name"];
            const char* u = a["browser_download_url"];
            if (n && u && name == n) {
                out.assetUrl = u;
                return Check::ok();
            }
        }
        return fail(Status::NotFound, out.tag + " has no " + name);
    });
    if (!c.isOk()) {
        out.status = c.status;
        out.error = c.message ? c.message : "check failed";
    }
    return out;
}

void Firmware::GithubFeed::check() {
    checkDue_ = false;
    Device& dev = fw_.dev_;
    HttpClient* http = dev.platform().http;
    std::string url = repo_.compare(0, 7, "http://") == 0 || repo_.compare(0, 8, "https://") == 0
                          ? repo_
                          : "https://api.github.com/repos/" + repo_ + "/releases/latest";
    setState(ReleaseState::Checking);
    Check room = tlsRoom(dev, url);
    if (!room.isOk()) {
        Latest r;
        r.status = room.status;
        r.error = room.message;
        checked(r);
        return;
    }
    bool spawned = http && dev.core().spawn("tk_release", 8192, [this, http, url, pattern = asset_,
                                                                  target = dev.info().target] {
        checked(fetch(*http, url, pattern, target));
    });
    if (!spawned) {
        Latest r;
        r.status = Status::Internal;
        r.error = "no memory for the check";
        checked(r);
    }
}

void Firmware::GithubFeed::checked(const Latest& r) {
    Device& dev = fw_.dev_;
    tesser::Lock lock(dev.api());
    uint32_t now = static_cast<uint32_t>(dev.core().uptimeMs());
    if (!r.tag.empty() && r.tag != latest_) {
        latest_ = r.tag;
        latestNode_->changed();
    }
    std::string error = r.status == Status::Ok ? std::string() : r.error;
    if (error != error_) {
        error_ = error;
        errorNode_->changed();
    }
    if (r.status == Status::Ok) {
        assetUrl_ = r.assetUrl;
        bool ok = false;
        int c = compareVersions(r.tag.c_str(), dev.version().c_str(), &ok);
        // A version that isn't semver can't be ordered: any other release
        // is offered.
        bool newer = ok ? c > 0 : r.tag != dev.version();
        setState(newer ? ReleaseState::Available : ReleaseState::Current);
        nextCheckMs_ = now + kCheckEveryMs;
        if (newer) tesser::logWarning("tesserkit: firmware %s is available", r.tag.c_str());
        // Told once per release, and only when found by an automatic check:
        // a client that asked has the answer in its reply.
        if (newer && r.tag != announced_) {
            announced_ = r.tag;
            if (waiters_.empty()) newRelease_->emit(r.tag);
        }
    } else {
        assetUrl_.clear();
        setState(ReleaseState::Failed);
        nextCheckMs_ = now + kRetryCheckMs;
        tesser::logWarning("tesserkit: release check failed: %s", r.error.c_str());
    }
    for (tesser::Pending& p : waiters_) {
        if (r.status == Status::Ok) {
            p.reply(latest_);
        } else {
            p.fail(r.status, r.error.c_str());
        }
    }
    waiters_.clear();
}

void Firmware::GithubFeed::setState(ReleaseState s) {
    if (s == state_) return;
    state_ = s;
    stateNode_->changed();
}

Ota& Firmware::ota() { return *ota_; }

Firmware& Firmware::downloads() {
    Platform& p = dev_.platform();
    if (!p.http) p.http = espHttpClient();
    return *this;
}

Firmware& Firmware::github(const char* repo, const char* asset) {
    feed_.reset(new GithubFeed(*this, repo ? repo : "", asset ? asset : ""));
    return downloads();
}

ReleaseState Firmware::release() const { return feed_ ? feed_->state() : ReleaseState::Unchecked; }

void Firmware::describe(tesser::Object& o) {
    Device& dev = dev_;
    const BootInfo& info = dev.info();
    Platform& p = dev.platform();
    if (!p.ota) p.ota = espOta();
    ota_ = p.ota;

    o.label("Firmware").ui("icon", "download");
    o.value("version", dev.version());
    o.value("project", info.project).ui("advanced");
    o.value("built", info.built).ui("advanced");
    o.value("slot", info.slot).label("Running slot").ui("advanced");
    o.value("slotSize", info.slotSize).label("Slot size").unit("B").ui("advanced");
    if (ota_) {
        o.file("image")
            .label("Upload firmware")
            .writeAccess(Access::Admin)
            .maxSize(ota_->nextSlotSize())
            .accept(".bin")
            .ui("confirm", "Install this firmware and restart?")
            .upload([this, &dev](tesser::FileRequest& r) -> std::unique_ptr<tesser::FileSink> {
                if (!begin(FirmwareState::Receiving)) {
                    r.refuse(Status::Busy, "an update is in progress");
                    return nullptr;
                }
                return std::unique_ptr<tesser::FileSink>(
                    new ImageUpload(*this, *ota_, dev.info().imageChipId, static_cast<uint32_t>(r.size)));
            });
        if (p.http) {
            o.action("download", [this](const std::string& url) { return download(url); })
                .label("Update from a URL")
                .writeAccess(Access::Admin)
                .ui("placeholder", "https://example.com/firmware.bin")
                .ui("advanced");
        }
    }
    stateNode_ = &o.value("state", state_, {"idle", "receiving", "downloading", "verifying", "rebooting", "failed"})
                      .readOnly();
    progressNode_ = &o.value("progress", static_cast<const int32_t&>(progress_))
                         .range(0, 100)
                         .unit("%")
                         .ui("widget", "bar")
                         .ui("visibleWhen", "state!=idle");
    errorNode_ = &o.value("error", static_cast<const std::string&>(error_)).ui("visibleWhen", "state=failed");
    if (ota_ && p.http) {
        updateFailed_ = &o.event("updateFailed").label("Update failed").ui("toast", "error").ui("hidden");
    }
    if (ota_ && p.http && feed_) feed_->describe(o);
    pendingNode_ = &o.value("pending", static_cast<const bool&>(pending_)).ui("hidden");
    o.value("pendingNote", "This firmware rolls back at the next restart unless it is kept. It is kept automatically "
                           "after a minute online.")
        .label("New firmware")
        .ui("style", "warning")
        .ui("visibleWhen", "pending=true");
    if (ota_) {
        o.action("confirm", [this] {
             if (pending_ && !ota_->confirm()) return Status::Internal;
             if (pending_) {
                 pending_ = false;
                 pendingNode_->changed();
             }
             return Status::Ok;
         })
            .label("Keep this firmware")
            .writeAccess(Access::Admin)
            .ui("visibleWhen", "pending=true");
        o.action("rollback", [this, &dev]() -> Status {
             if (state_ != FirmwareState::Idle && state_ != FirmwareState::Failed) return Status::Busy;
             Check c = ota_->rollback();
             if (!c.isOk()) return c.status;
             setState(FirmwareState::Rebooting);
             dev.updating(true);
             dev.restartIn(500);
             return Status::Ok;
         })
            .label("Roll back")
            .writeAccess(Access::Admin)
            .ui("confirm", "Restart into the previous firmware?")
            .ui("style", "danger")
            .ui("advanced");
    }
}

void Firmware::start() {
    if (!ota_) {
        tesser::logError("tesserkit: Firmware has no OTA partitions");
        return;
    }
    pending_ = ota_->pending();
    if (pending_) tesser::logWarning("tesserkit: running a new image, not confirmed yet");
}

void Firmware::tick(uint32_t nowMs) {
    (void)nowMs;
    if (pending_ && dev_.onlineMs() >= kConfirmAfterMs) {
        if (ota_->confirm()) tesser::logWarning("tesserkit: new image confirmed");
        pending_ = false;
        pendingNode_->changed();
    }
    if (feed_) feed_->tick(nowMs);
}

void Firmware::setState(FirmwareState s) {
    if (s == state_) return;
    state_ = s;
    stateNode_->changed();
}

bool Firmware::begin(FirmwareState s) {
    if (state_ != FirmwareState::Idle && state_ != FirmwareState::Failed) return false;
    // ESP-IDF refuses an update while the running image is unconfirmed. One
    // that serves an update works: confirm it.
    if (pending_) {
        ota_->confirm();
        pending_ = false;
        pendingNode_->changed();
    }
    setState(s);
    progress_ = 0;
    progressNode_->changed();
    if (!error_.empty()) {
        error_.clear();
        errorNode_->changed();
    }
    dev_.updating(true);
    return true;
}

void Firmware::progress(uint32_t received, uint32_t total) {
    if (!total) return;
    int32_t pct = static_cast<int32_t>(static_cast<uint64_t>(received) * 100 / total);
    if (pct == progress_) return;
    tesser::Lock lock(dev_.api());
    progress_ = pct;
    progressNode_->changed();
}

void Firmware::verifying() {
    tesser::Lock lock(dev_.api());
    setState(FirmwareState::Verifying);
}

void Firmware::failed(const char* message) {
    tesser::Lock lock(dev_.api());
    error_ = message ? message : "update failed";
    errorNode_->changed();
    setState(FirmwareState::Failed);
    dev_.updating(false);
    tesser::logWarning("tesserkit: update failed: %s", error_.c_str());
}

void Firmware::downloadFailed(const char* message) {
    tesser::Lock lock(dev_.api());
    failed(message);
    if (updateFailed_) updateFailed_->emit(error_);
}

void Firmware::installed() {
    tesser::Lock lock(dev_.api());
    progress_ = 100;
    progressNode_->changed();
    setState(FirmwareState::Rebooting);
    dev_.restartIn(500);  // after the reply
}

Check Firmware::download(const std::string& url) {
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        return Check::fail(Status::InvalidValue, "http:// or https://");
    }
    HttpClient* http = dev_.platform().http;
    if (!http) return Check::fail(Status::NotAllowed, "no HTTP client in this build");
    Check room = tlsRoom(dev_, url);
    if (!room.isOk()) return room;
    if (!begin(FirmwareState::Downloading)) return Check::fail(Status::Busy, "an update is in progress");
    Ota& ota = *ota_;
    uint16_t chipId = dev_.info().imageChipId;
    uint32_t slot = ota.nextSlotSize();
    bool spawned = dev_.core().spawn("tk_download", 8192, [this, http, &ota, chipId, slot, url] {
        ImageWriter writer(ota, chipId);
        Check c = http->get(url.c_str(), [this, &writer, slot](HttpResponse& res) {
            if (res.status() == 404) return Check::fail(Status::NotFound, "HTTP 404");
            if (res.status() != 200) return Check::fail(Status::NotFound, "the server refused the download");
            int64_t length = res.length();
            if (length > static_cast<int64_t>(slot)) return Check::fail(Status::TooLarge, "image larger than the slot");
            writer.begin(length > 0 ? static_cast<uint32_t>(length) : 0);
            std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunk]);
            for (;;) {
                int n = res.read(buf.get(), kChunk);
                if (n < 0) return Check::fail(Status::Timeout, "download interrupted");
                if (n == 0) return Check::ok();
                Check w = writer.write(buf.get(), static_cast<size_t>(n));
                if (!w.isOk()) return w;
                progress(writer.received(), writer.size());
            }
        });
        if (c.isOk()) {
            verifying();
            c = writer.finish();
        }
        if (c.isOk()) {
            installed();
        } else {
            downloadFailed(c.message ? c.message : "download failed");
        }
    });
    if (!spawned) {
        failed("no memory for the download");
        return Check::fail(Status::Internal, "no memory for the download");
    }
    return Check::ok();
}

}  // namespace tesserkit
