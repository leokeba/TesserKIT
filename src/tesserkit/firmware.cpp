#include "tesserkit/firmware.h"

#include <string.h>

#include <memory>

#include "tesserkit/image.h"

namespace tesserkit {

using tesser::Access;
using tesser::Check;
using tesser::Status;

namespace {

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

}  // namespace

Ota& Firmware::ota() { return *ota_; }

Firmware& Firmware::downloads() {
    Platform& p = dev_.platform();
    if (!p.http) p.http = espHttpClient();
    return *this;
}

void Firmware::describe(tesser::Object& o) {
    Device& dev = dev_;
    const BootInfo& info = dev.info();
    Platform& p = dev.platform();
    if (!p.ota) p.ota = espOta();
    ota_ = p.ota;

    o.label("Firmware");
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
    pendingNode_ = &o.value("pending", static_cast<const bool&>(pending_))
                        .label("Not confirmed yet")
                        .doc("This image rolls back at the next reset unless confirmed");
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

void Firmware::installed() {
    tesser::Lock lock(dev_.api());
    progress_ = 100;
    progressNode_->changed();
    setState(FirmwareState::Rebooting);
    dev_.restartIn(500);  // after the reply
}

Status Firmware::download(const std::string& url) {
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) return Status::InvalidValue;
    HttpClient* http = dev_.platform().http;
    if (!http) return Status::NotAllowed;
    if (!begin(FirmwareState::Downloading)) return Status::Busy;
    Ota& ota = *ota_;
    uint16_t chipId = dev_.info().imageChipId;
    uint32_t slot = ota.nextSlotSize();
    bool spawned = dev_.core().spawn("tk_download", 8192, [this, http, &ota, chipId, slot, url] {
        ImageWriter writer(ota, chipId);
        Check c = http->get(
            url.c_str(),
            [&writer, slot](int64_t length) {
                if (length > static_cast<int64_t>(slot)) return Check::fail(Status::TooLarge, "image larger than the slot");
                writer.begin(length > 0 ? static_cast<uint32_t>(length) : 0);
                return Check::ok();
            },
            [this, &writer](const uint8_t* data, size_t len) {
                Check w = writer.write(data, len);
                if (w.isOk()) progress(writer.received(), writer.size());
                return w;
            });
        if (c.isOk()) {
            verifying();
            c = writer.finish();
        }
        if (c.isOk()) {
            installed();
        } else {
            failed(c.message ? c.message : "download failed");
        }
    });
    if (!spawned) {
        failed("no memory for the download");
        return Status::Internal;
    }
    return Status::Ok;
}

}  // namespace tesserkit
