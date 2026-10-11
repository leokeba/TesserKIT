#include "tesserkit/backup.h"

#include <string.h>

#include <memory>
#include <string>
#include <vector>

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
              [this, &dev](std::string_view text, tesser::JsonWriter& reply) -> Check {
                  // The setting in force decides, not the one the backup holds.
                  bool restart = restartAfter_;
                  std::vector<std::string> skipped;
                  Status s = dev.api().restore(text, &skipped);
                  if (s == Status::BadRequest) return Check::fail(s, "not a settings file");
                  if (s != Status::Ok) return Check::fail(s, "the settings couldn't be saved");
                  reply.beginObject();
                  reply.key("skipped");
                  reply.beginArray();
                  for (const std::string& p : skipped) reply.string(p);
                  reply.endArray();
                  reply.key("restart");
                  reply.boolean(restart);
                  reply.endObject();
                  if (restart) dev.restartIn(1000);
                  return Check::ok();
              })
        .download([&dev](tesser::FileRequest& r) {
            r.filename = dev.hostname() + "-settings.json";
            return std::unique_ptr<tesser::FileSource>(new StringSource(dev.api().persistedState()));
        });
    o.value("restartAfter", restartAfter_).persist().label("Restart after restoring").ui("advanced");
}

}  // namespace tesserkit
