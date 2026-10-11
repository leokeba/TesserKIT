#include "tesserkit/backup.h"

#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "tesserkit/files.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

class StringSource : public tesser::FileSource {
public:
    explicit StringSource(std::string text) : text_(std::move(text)) {}
    int64_t size() const override { return static_cast<int64_t>(text_.size()); }
    int read(uint8_t* buf, size_t cap) override {
        size_t n = text_.size() - offset_;
        if (n > cap) n = cap;
        memcpy(buf, text_.data() + offset_, n);
        offset_ += n;
        return static_cast<int>(n);
    }

private:
    std::string text_;
    size_t offset_ = 0;
};

}  // namespace

Backup& Backup::keep(uint8_t n) {
    keep_ = n;
    return *this;
}

Check Backup::restore(std::string_view text, std::vector<std::string>& skipped, bool& restart) {
    // The setting in force decides, not the one the backup holds.
    restart = restartAfter_;
    // Undoable: the settings it replaces are stored first.
    if (Files* files = dev_.files()) files->storeBackup(*this, Files::kBeforeRestore, nullptr);
    Status s = dev_.api().restore(text, &skipped);
    if (s == Status::BadRequest) return Check::fail(s, "not a settings file");
    if (s != Status::Ok) return Check::fail(s, "the settings couldn't be saved");
    if (restart) dev_.restartIn(1000);
    return Check::ok();
}

void Backup::writeResult(tesser::JsonWriter& w, const std::vector<std::string>& skipped, bool restart) {
    w.beginObject();
    w.key("skipped");
    w.beginArray();
    for (const std::string& p : skipped) w.string(p);
    w.endArray();
    w.key("restart");
    w.boolean(restart);
    w.endObject();
}

Check Backup::save(const std::string& name, std::string* saved) {
    Files* files = dev_.files();
    return files ? files->storeBackup(*this, name, saved) : Check::fail(Status::NotAllowed, "no filesystem");
}

void Backup::describe(tesser::Object& o) {
    Device& dev = dev_;
    o.label("Backup").ui("icon", "refresh").writeAccess(Access::Admin);
    o.file("settings")
        .label("Settings")
        .readAccess(Access::Admin)
        .contentType("application/json")
        .accept(".json")
        .maxSize(kMaxSize)
        .ui("confirm", "Replace the settings with this backup?")
        .text(nullptr,
              [this](std::string_view text, tesser::JsonWriter& reply) {
                  std::vector<std::string> skipped;
                  bool restart = false;
                  Check c = restore(text, skipped, restart);
                  if (c.isOk()) writeResult(reply, skipped, restart);
                  return c;
              })
        .download([&dev](tesser::FileRequest& r) {
            r.filename = dev.hostname() + "-settings.json";
            return std::unique_ptr<tesser::FileSource>(new StringSource(dev.api().persistedState()));
        });
    o.value("restartAfter", restartAfter_).persist().label("Restart after restoring").ui("advanced");
    // Virtual: a firmware without Files doesn't link the stored backups.
    if (Files* files = dev.files()) files->describeBackups(*this, o);
}

}  // namespace tesserkit
