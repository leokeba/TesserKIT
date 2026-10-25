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

// A download of a text, holding its reservation until it is done.
class StringSource : public tesser::FileSource {
public:
    StringSource(std::string text, tesser::Reservation held) : text_(std::move(text)), held_(std::move(held)) {}
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
    tesser::Reservation held_;
    size_t offset_ = 0;
};

// What a stored state is estimated at before one was taken.
constexpr uint32_t kFirstEstimate = 4096;

}  // namespace

Check Backup::state(std::string& out, tesser::Reservation& held) {
    // persistedState() grows its text by doubling: one block of up to twice
    // the text at the end. Reserved before from the last state's size, then
    // set to the block it took.
    uint32_t last = lastStateBytes_.load();
    size_t estimate = last ? last : kFirstEstimate;
    held = tesser::reserveBytes(2 * estimate, true, tesser::mallocPool(2 * estimate));
    if (!held) return Check::fail(Status::Busy, "not enough free memory for the settings now");
    out = dev_.api().persistedState();
    lastStateBytes_ = static_cast<uint32_t>(out.size());
    (void)held.resize(out.capacity());  // unchanged if the budget can't give more: it is allocated already
    held.settle();
    return Check::ok();
}

Backup& Backup::keep(uint8_t n) {
    keep_ = n;
    return *this;
}

Check Backup::restore(std::string_view text, std::vector<std::string>& skipped, bool& restart) {
    // The setting in force decides, not the one the backup holds.
    restart = restartAfter_;
    // Undoable: the settings it replaces are stored first.
    if (Files* files = dev_.files()) files->storeBackup(*this, Files::kBeforeRestore, nullptr);
    // The parsed document: ArduinoJson's pools, about as large as the text
    // or less, taken twice for safety, in pieces. TesserAPI parses a backup
    // in PSRAM where there is some (jsonAllocator(Pool::Large)).
    tesser::Reservation doc = tesser::reserveBytes(2 * text.size(), false, tesser::Pool::Large);
    if (!doc) return Check::fail(Status::Busy, "not enough free memory to restore the settings now");
    Status s = dev_.api().restore(text, &skipped);
    if (s == Status::BadRequest) return Check::fail(s, "not a settings file");
    // Applied, but its save waits for memory (settings on the filesystem
    // on a tight heap): TesserAPI saves it as soon as it can. Not restarted
    // meanwhile, which would lose it.
    saved_ = s == Status::Ok;
    if (!saved_ && dev_.settingsWaiting().empty()) return Check::fail(s, "the settings couldn't be saved");
    if (!saved_) restart = false;
    if (restart) dev_.restartIn(1000);
    return Check::ok();
}

void Backup::writeResult(tesser::JsonWriter& w, const std::vector<std::string>& skipped, bool restart) const {
    w.beginObject();
    if (!saved_) {
        w.key("saved");
        w.boolean(false);  // applied; saved once memory allows (/sys/status/settings)
    }
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
    o.label("Backup").ui("icon", "refresh").ui("group", "Maintenance").writeAccess(Access::Admin);
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
        .download([this, &dev](tesser::FileRequest& r) {
            std::string text;
            tesser::Reservation held;
            Check c = state(text, held);
            if (!c.isOk()) {
                r.refuse(c.status, c.message);
                return std::unique_ptr<tesser::FileSource>();
            }
            r.filename = dev.hostname() + "-settings.json";
            return std::unique_ptr<tesser::FileSource>(new StringSource(std::move(text), std::move(held)));
        });
    o.value("restartAfter", restartAfter_).persist().label("Restart after restoring").ui("advanced");
    // Virtual: a firmware without Files doesn't link the stored backups.
    if (Files* files = dev.files()) files->describeBackups(*this, o);
}

}  // namespace tesserkit
