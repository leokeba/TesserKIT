// Backup's stored backups (docs/DESIGN.md section 5.5), kept by Files in
// backups/<name>.json. Reached through Files' virtual functions only, so a
// firmware without Files doesn't link this.
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "tesser/json_writer.h"
#include "tesser/sink.h"
#include "tesserkit/backup.h"
#include "tesserkit/files.h"

namespace tesserkit {

using tesser::Check;
using tesser::Status;

namespace {

constexpr int64_t kValidTimeMs = 1577836800000LL;  // 2020-01-01
constexpr const char* kExt = ".json";
constexpr size_t kExtLen = 5;

bool isBackup(const FileEntry& e) {
    return !e.isDir && e.name.size() > kExtLen && e.name.compare(e.name.size() - kExtLen, kExtLen, kExt) == 0;
}

}  // namespace

class StoredBackups {
public:
    StoredBackups(Device& dev, Files& files, Backup& backup) : dev_(dev), files_(files), backup_(backup) {}

    void describe(tesser::Object& o);
    Check save(const std::string& name, std::string* saved);
    void refresh();

private:
    std::string path(const std::string& name) const { return std::string(Files::kBackupDir) + "/" + name + kExt; }
    std::string defaultName() const;
    void prune(const std::string& justSaved);

    Device& dev_;
    Files& files_;
    Backup& backup_;
    std::vector<FileEntry> stored_;
    tesser::Node* node_ = nullptr;
};

std::string StoredBackups::defaultName() const {
    Clock* clock = dev_.platform().clock;
    int64_t ms = clock ? clock->epochMs() : 0;
    std::string base;
    if (ms >= kValidTimeMs) {
        time_t t = static_cast<time_t>(ms / 1000);
        struct tm local;
        localtime_r(&t, &local);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d-%H%M", &local);
        base = buf;
    }
    std::vector<FileEntry> all = files_.list(Files::kBackupDir, SIZE_MAX);
    auto taken = [&all](const std::string& name) {
        std::string file = name + kExt;
        return std::any_of(all.begin(), all.end(), [&file](const FileEntry& e) { return e.name == file; });
    };
    if (!base.empty() && !taken(base)) return base;
    for (unsigned n = base.empty() ? 1 : 2; n < 1000; n++) {
        char buf[40];
        if (base.empty()) {
            snprintf(buf, sizeof(buf), "backup-%03u", n);
        } else {
            snprintf(buf, sizeof(buf), "%s-%u", base.c_str(), n);
        }
        if (!taken(buf)) return buf;
    }
    return "backup";
}

Check StoredBackups::save(const std::string& name, std::string* saved) {
    if (!files_.mounted()) return Check::fail(Status::NotAllowed, "no filesystem");
    std::string n = name.empty() ? defaultName() : name;
    // The name with its extension must fit.
    if (n.size() + kExtLen > Files::kMaxName) return Check::fail(Status::InvalidValue, "1 to 27 characters");
    Check c = Files::checkName(n);
    if (!c.isOk()) return c;
    std::string text;
    tesser::Reservation held;
    c = backup_.state(text, held);
    if (!c.isOk()) return c;
    c = files_.write(path(n), text);
    if (!c.isOk()) return c;
    if (saved) *saved = n;
    prune(n);
    return Check::ok();
}

void StoredBackups::prune(const std::string& justSaved) {
    std::vector<FileEntry> counted;
    for (FileEntry& e : files_.list(Files::kBackupDir, SIZE_MAX)) {
        if (isBackup(e) && e.name != std::string(Files::kBeforeRestore) + kExt &&
            e.name != std::string(Files::kBeforeUpdate) + kExt) {
            counted.push_back(std::move(e));
        }
    }
    if (counted.size() <= backup_.keep_) return;
    // Oldest first; a clock that wasn't set reads as oldest.
    sortEntries(counted, [](const FileEntry& a, const FileEntry& b) {
        return a.modified != b.modified ? a.modified < b.modified : a.name < b.name;
    });
    size_t extra = counted.size() - backup_.keep_;
    for (const FileEntry& e : counted) {
        if (!extra) break;
        if (e.name == justSaved + kExt) continue;
        files_.remove(std::string(Files::kBackupDir) + "/" + e.name);
        extra--;
    }
}

void StoredBackups::refresh() {
    stored_.clear();
    for (FileEntry& e : files_.list(Files::kBackupDir, SIZE_MAX)) {
        if (isBackup(e)) stored_.push_back(std::move(e));
    }
    // Newest first, then by name (with the extension, as listed).
    sortEntries(stored_, [](const FileEntry& a, const FileEntry& b) {
        return a.modified != b.modified ? a.modified > b.modified : a.name < b.name;
    });
    if (stored_.size() > Files::kMaxEntries) stored_.resize(Files::kMaxEntries);
    for (FileEntry& e : stored_) e.name.resize(e.name.size() - kExtLen);
    if (node_) node_->changed();
}

void StoredBackups::describe(tesser::Object& o) {
    o.action("save",
             [this](const std::string& name, tesser::Call& call) {
                 std::string saved;
                 Check c = save(name, &saved);
                 if (!c.isOk()) return call.fail(c.status, c.message);
                 refresh();
                 call.reply(saved);
             })
        .label("Store on the device")
        .doc("Keeps the settings in the device's files, under this name (empty: the date)");
    node_ = &o.list("stored", stored_,
                    [this](tesser::Object& e, FileEntry& f) {
                        // The element is temporary: handlers keep copies.
                        std::string file = path(f.name);
                        e.value("name", f.name);
                        e.value("size", f.size).unit("B");
                        e.value("saved", f.modified).ui("format", "datetime").ui("advanced");
                        e.file("content")
                            .download([this, file, name = f.name](tesser::FileRequest& r) {
                                auto src = files_.source(file, r);
                                if (src) r.filename = dev_.hostname() + "-" + name + kExt;
                                return src;
                            })
                            .contentType("application/json")
                            .label("Download");
                        e.action("restore", [this, file](tesser::Call& call) {
                             std::string text;
                             tesser::Reservation held;
                             Check c = files_.read(file, text, Backup::kMaxSize, &held);
                             if (!c.isOk()) return call.fail(c.status, c.message);
                             std::vector<std::string> skipped;
                             bool restart = false;
                             c = backup_.restore(text, skipped, restart);
                             if (!c.isOk()) return call.fail(c.status, c.message);
                             call.replyWith([&](tesser::JsonWriter& w) { backup_.writeResult(w, skipped, restart); });
                         }).ui("confirm", "Replace the settings with this backup?");
                        e.action("delete", [this, file] {
                             Check c = files_.remove(file);
                             refresh();
                             return c;
                         }).ui("confirm", "Delete this backup?");
                    })
                .key("name")
                .readOnly()
                .maxSize(Files::kMaxEntries)
                .label("Stored backups");
    o.value("keep", backup_.keep_).range(1, 50).persist().label("Backups kept").ui("advanced");
}

// Here, where StoredBackups is complete.
Files::~Files() { delete backups_; }

void Files::describeBackups(Backup& backup, tesser::Object& o) {
    delete backups_;
    backups_ = new StoredBackups(dev_, *this, backup);
    backups_->describe(o);
}

Check Files::storeBackup(Backup& backup, const std::string& name, std::string* saved) {
    (void)backup;
    if (!backups_) return Check::fail(Status::NotAllowed, "no filesystem");
    return backups_->save(name, saved);
}

void Files::refreshBackups() {
    if (backups_) backups_->refresh();
}

Check Files::storeBeforeUpdate(tesser::Storage& settings, uint32_t version) {
    if (!mount()) return Check::fail(Status::NotAllowed, "no filesystem");
    // A backup's form (persistedState()'s): "@version", then the records
    // under their names, as stored, so that the firmware that stored them
    // restores them as they were. Copied, not parsed: what backups leave out
    // (the token key) is there, and a restore skips it (TesserAPI's restore
    // ignores nodes marked backup(false)). Leaving it out would take
    // ArduinoJson's parser here, about 1.8 KB of flash. The guarded sink
    // fails the backup on a short heap rather than aborts the boot.
    std::string text;
    tesser::StringSink sink(text, 0, true);
    tesser::JsonWriter w(sink);
    w.beginObject();
    w.key("@version");
    w.uinteger(version);
    bool any = false;
    for (const tesser::Node* c = dev_.api().first(); c; c = c->next()) {
        if (!c->backedUp()) continue;
        std::string data;
        if (!settings.load(c->name(), data) || data == "null") continue;
        w.key(c->name());
        w.raw(data);
        any = true;
    }
    w.endObject();
    if (!any) return Check::ok();  // a new device: nothing to keep
    if (!w.ok()) return Check::fail(Status::Busy, "not enough free memory");
    return write(std::string(kBackupDir) + "/" + kBeforeUpdate + kExt, text);
}

}  // namespace tesserkit
