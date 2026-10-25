#include "tesserkit/bluetooth.h"

namespace tesserkit {

using tesser::Access;

namespace {

constexpr uint32_t kPollMs = 500;  // the connection's state

}  // namespace

Bluetooth& Bluetooth::enable(bool on) {
    enabled_ = on;
    return *this;
}

void Bluetooth::describe(tesser::Object& o) {
    o.label("Bluetooth")
        .ui("group", "Network")
        .ui("summary", "enabled")
        .ui("icon", "bluetooth")
        .doc("The interface over Bluetooth LE, from TesserUI's launcher")
        .writeAccess(Access::Admin);
    enabledNode_ = &o.value("enabled", enabled_).persist();
    o.value("name", [this] { return name_; }).doc("What the browser's device chooser shows: the hostname");
    restartNode_ = &o.value("restartNeeded", [this] { return restart_; }).ui("hidden");
    o.value("restartWarning", "Bluetooth can't be switched off while running: the change applies after a restart.")
        .label("Restart needed")
        .ui("style", "warning")
        .ui("visibleWhen", "restartNeeded=true");
    auto& s = o.object("status").label("Status");
    status_ = &s;
    s.value("connected", static_cast<const bool&>(connected_));
    s.value("client", static_cast<const std::string&>(client_)).label("Client");
}

void Bluetooth::start() {
    Platform& p = dev_.platform();
    if (!p.ble) p.ble = espBle();
    ble_ = p.ble;
    if (!ble_) tesser::logError("tesserkit: Bluetooth has no Bluetooth LE in this build");
    enabledGen_ = enabledNode_->generation();
    apply();
}

void Bluetooth::apply() {
    if (!ble_) return;
    if (enabled_ && !started_) {
        name_ = dev_.hostname();
        started_ = advertising_ = ble_->begin(dev_.api(), name_.c_str());
        if (!started_) tesser::logError("tesserkit: Bluetooth didn't start");
    } else if (enabled_ && !advertising_) {
        if (ble_->advertise(true)) advertising_ = true;
    } else if (!enabled_ && started_) {
        // Its memory back when the transport can, else at least silent.
        if (ble_->end()) {
            started_ = advertising_ = false;
        } else if (advertising_ && ble_->advertise(false)) {
            advertising_ = false;
        }
    }
    // What runs isn't what is asked for: only a restart applies it.
    bool restart = started_ && enabled_ != advertising_;
    if (restart != restart_) {
        restart_ = restart;
        restartNode_->changed();
    }
}

void Bluetooth::tick(uint32_t nowMs) {
    if (!ble_) return;
    uint32_t gen = enabledNode_->generation();
    if (gen != enabledGen_) {
        enabledGen_ = gen;
        apply();
    }
    if (nowMs - polledAtMs_ < kPollMs) return;
    polledAtMs_ = nowMs;
    if (started_ && dev_.hostname() != name_ && ble_->name(dev_.hostname().c_str())) name_ = dev_.hostname();
    bool c = started_ && ble_->connected();
    std::string who = c ? ble_->client() : std::string();
    if (c != connected_ || who != client_) {
        connected_ = c;
        client_ = who;
        status_->changed();
    }
}

}  // namespace tesserkit
