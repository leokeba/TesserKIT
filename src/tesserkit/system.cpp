#include "tesserkit/system.h"

#include <stdio.h>

#include <memory>

namespace tesserkit {

using tesser::Access;

namespace {

// Streams the stored core dump.
class CoredumpSource : public tesser::FileSource {
public:
    explicit CoredumpSource(Coredump& dump) : dump_(dump), size_(dump.size()) {}
    int64_t size() const override { return size_; }
    int read(uint8_t* buf, size_t cap) override {
        if (offset_ >= size_) return 0;
        size_t n = size_ - offset_ < cap ? size_ - offset_ : cap;
        int got = dump_.read(offset_, buf, n);
        if (got > 0) offset_ += static_cast<uint32_t>(got);
        return got;
    }

private:
    Coredump& dump_;
    uint32_t size_;
    uint32_t offset_ = 0;
};

}  // namespace

void System::describe(tesser::Object& o) {
    Device& dev = dev_;
    const BootInfo& info = dev.info();
    resetReason_ = info.resetReason;

    auto& s = o.object("status").label("Status").ui("icon", "info");
    dev.stateNode(&s.value("state", dev.stateVar(),
                           {"booting", "provisioning", "connecting", "online", "updating", "error"})
                       .readOnly());
    s.value("uptime", [&dev] { return static_cast<uint32_t>(dev.core().uptimeMs() / 1000); }).unit("s").watch();
    s.value("heap", [&dev] { return dev.core().freeHeap(); }).label("Free heap").unit("B").watch().ui("widget", "chart");
    s.value("minHeap", [&dev] { return dev.core().minFreeHeap(); }).label("Lowest free heap").unit("B").watch();
    if (info.psram) s.value("psram", [&dev] { return dev.core().freePsram(); }).label("Free PSRAM").unit("B").watch();
    s.value("resetReason", resetReason_,
            {"power-on", "software", "panic", "watchdog", "brownout", "deep-sleep", "other"})
        .readOnly()
        .label("Last reset");
    s.value("chip", info.chip);
    s.value("flash", info.flashSize).label("Flash size").unit("B");
    s.value("appSize", info.appSize).label("Firmware size").unit("B");
    s.value("appSlot", info.slotSize).label("Firmware slot").unit("B");
    s.value("idf", info.idf).label("ESP-IDF");
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1], info.mac[2], info.mac[3],
             info.mac[4], info.mac[5]);
    mac_ = mac;
    s.value("mac", static_cast<const std::string&>(mac_)).label("MAC");
    float celsius = 0;
    if (dev.core().temperature(celsius)) {
        s.value("temperature", [&dev] {
             float t = 0;
             dev.core().temperature(t);
             return t;
         })
            .unit("°C")
            .watch()
            .ui("precision", 1);
    }

    o.action("restart", [&dev] { dev.restartIn(500); })
        .writeAccess(Access::Admin)
        .ui("confirm", "Restart the device?")
        .ui("style", "danger");
    o.action("factoryReset",
             [&dev] { return dev.factoryReset() ? tesser::Status::Ok : tesser::Status::Internal; })
        .label("Factory reset")
        .writeAccess(Access::Admin)
        .ui("confirm", "Erase every setting and restart?")
        .ui("style", "danger")
        .ui("advanced");

    Platform& p = dev.platform();
    if (!p.coredump) p.coredump = espCoredump();
    if (Coredump* dump = p.coredump) {
        o.file("coredump")
            .label("Core dump")
            .readAccess(Access::Admin)
            .download([dump, &dev](tesser::FileRequest& r) -> std::unique_ptr<tesser::FileSource> {
                if (!dump->size()) {
                    r.refuse(tesser::Status::NotFound, "no core dump");
                    return nullptr;
                }
                r.filename = dev.hostname() + "-coredump.bin";
                return std::unique_ptr<tesser::FileSource>(new CoredumpSource(*dump));
            })
            .ui("advanced");
        o.value("coredumpPresent", [dump] { return dump->size() > 0; }).label("Core dump present").ui("advanced");
        o.action("clearCoredump", [dump] { return dump->erase() ? tesser::Status::Ok : tesser::Status::Internal; })
            .label("Clear the core dump")
            .writeAccess(Access::Admin)
            .ui("advanced");
    }
}

}  // namespace tesserkit
