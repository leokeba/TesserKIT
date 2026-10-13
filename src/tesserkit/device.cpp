#include "tesserkit/device.h"

#include <string.h>

#include <ctype.h>
#include <stdio.h>

#include <utility>

namespace tesserkit {

const char* toString(State s) {
    switch (s) {
        case State::Booting: return "booting";
        case State::Provisioning: return "provisioning";
        case State::Connecting: return "connecting";
        case State::Online: return "online";
        case State::Updating: return "updating";
        case State::Error: return "error";
    }
    return "error";
}

const tesser::Node* findNode(const tesser::Object& root, std::string_view path, bool* opaque) {
    if (opaque) *opaque = false;
    const tesser::Node* node = &root;
    size_t start = !path.empty() && path[0] == '/' ? 1 : 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        if (end == start) return nullptr;  // "//"
        switch (node->type()) {
            case tesser::NodeType::Object:
                if (path.substr(start, end - start) == "*") {  // every child: checked by the API when used
                    if (opaque) *opaque = true;
                    return nullptr;
                }
                node = static_cast<const tesser::Object*>(node)->child(path.substr(start, end - start));
                if (!node) return nullptr;
                break;
            case tesser::NodeType::List:
            case tesser::NodeType::Array:
            case tesser::NodeType::Remote:
                if (opaque) *opaque = true;
                return nullptr;
            default:
                return nullptr;  // below a leaf
        }
        start = end + 1;
    }
    return node;
}

bool resolves(const tesser::Object& root, std::string_view path) {
    bool opaque;
    const tesser::Node* node = findNode(root, path, &opaque);
    return opaque || (node && node != &root);
}

Service::Service(Device& dev, const char* name) : dev_(dev), name_(name) {
    // Registration order is mount order. Nothing here touches another
    // translation unit's globals: services are often globals themselves.
    Service** p = &dev.services_;
    while (*p) p = &(*p)->next_;
    *p = this;
}

Device::Device(const char* label)
    : label_(label)
#if defined(ESP_PLATFORM)
      ,
      http_(api_)
#endif
{
}

Device::~Device() { delete erased_; }

Device& Device::hostname(const char* name) {
    hostname_ = name ? name : "";
    return *this;
}

Device& Device::version(const char* v) {
    version_ = v ? v : "";
    return *this;
}

Device& Device::ui(bool on) {
    ui_ = on;
    return *this;
}

Device& Device::httpPort(uint16_t port) {
    httpPort_ = port;
    return *this;
}

Device& Device::cors(const char* origin) {
    cors_ = origin;
    return *this;
}

Device& Device::task(bool own) {
    ownTask_ = own;
    return *this;
}

Device& Device::storage(tesser::Storage& s) {
    storage_ = &s;
    return *this;
}

Device& Device::extraHandlers(uint8_t n) {
    extraHandlers_ = n;
    return *this;
}

std::string Device::defaultHostname() const {
    // The label lowercased, other characters as single hyphens, then the
    // MAC's last two bytes: "Heliostat 3" → "heliostat-3-a67c".
    std::string h;
    for (const char* c = label_ ? label_ : ""; *c && h.size() < 26; c++) {
        unsigned char u = static_cast<unsigned char>(*c);
        if (isalnum(u)) {
            h.push_back(static_cast<char>(tolower(u)));
        } else if (!h.empty() && h.back() != '-') {
            h.push_back('-');
        }
    }
    while (!h.empty() && h.back() == '-') h.pop_back();
    if (h.empty()) h = "tesser";
    char tail[8];
    snprintf(tail, sizeof(tail), "-%02x%02x", info_.mac[4], info_.mac[5]);
    return h + tail;
}

bool Device::begin() {
    if (started_) return true;
    if (!platform_.core) platform_.core = espCore();
    if (!platform_.core) {
        tesser::logError("tesserkit: no platform (on the host, fill Device::platform() before begin())");
        return false;
    }
    platform_.core->info(info_);
    if (hostname_.empty()) hostname_ = defaultHostname();
    if (version_.empty()) version_ = info_.version;

    api_.label(label_);
    // Without the Auth service the device is open: every client is Admin
    // (docs/DESIGN.md section 5.7). Auth replaces this check with its own.
    api_.authenticate([](std::string_view) { return tesser::Access::Admin; });
    sys_ = &api_.object("sys");
    sys_->label("System").doc("Network, time, firmware, backups and accounts").ui("page").ui("icon", "settings");
    for (Service* s = services_; s; s = s->next_) s->attach(*sys_);

    if (!storage_) storage_ = defaultStorage();
    if (storage_) {
        api_.persistence(*storage_, 2000);
        api_.load();
    }
    for (Service* s = services_; s; s = s->next_) s->start();
    started_ = true;
    lastNowMs_ = static_cast<uint32_t>(platform_.core->uptimeMs());
    {
        tesser::Lock lock(api_);
        updateState(lastNowMs_);
    }
    if (httpPort_ && !startServer()) return false;
    if (ownTask_ && !startTask()) return false;
    return true;
}

void Device::loop() {
    if (!started_) return;
    uint32_t now = static_cast<uint32_t>(core().uptimeMs());
    lastNowMs_ = now;
    std::vector<Event> events;
    {
        tesser::MutexGuard guard(eventsMutex_);
        events.swap(events_);
    }
    State before = state_;
    {
        tesser::Lock lock(api_);
        for (const Event& e : events) {
            for (Service* s = services_; s; s = s->next_) s->event(e);
        }
        for (Service* s = services_; s; s = s->next_) s->tick(now);
        updateState(now);
    }
    api_.poll(now);
    if (state_ != before) {
        for (auto& fn : onState_) fn(state_);
    }
    for (auto& fn : onTick_) fn(now);

    uint32_t delay = restartDelayMs_.exchange(kNoRestart);
    if (delay != kNoRestart && !restartArmed_) {
        restartArmed_ = true;
        restartAtMs_ = now + delay;
    }
    if (restartArmed_ && static_cast<int32_t>(now - restartAtMs_) >= 0) {
        restartArmed_ = false;
        api_.save();  // what the debounce still holds
        core().restart();
    }
}

void Device::post(Event e) {
    tesser::MutexGuard guard(eventsMutex_);
    if (events_.size() >= 32) {
        tesser::logWarning("tesserkit: event queue full, dropping one");
        return;
    }
    events_.push_back(std::move(e));
}

void Device::restartIn(uint32_t ms) { restartDelayMs_.store(ms == kNoRestart ? ms - 1 : ms); }

bool Device::factoryReset() {
    bool ok = storage_ ? storage_->erase() : true;
    for (Service* sv = services_; sv; sv = sv->next_) sv->factoryReset();
    // Nothing is saved any more: the running state would be written back.
    if (!erased_) erased_ = new tesser::MemoryStorage();
    api_.persistence(*erased_, 2000);
    restartIn(500);
    return ok;
}

void Device::fail(bool failed) {
    tesser::Lock lock(api_);
    failed_ = failed;
}

void Device::sleeping() {
    for (auto& fn : onSleep_) fn();
    api_.save();  // what the debounce still holds
}

void Device::networkState(State s) { netState_ = s; }

void Device::updating(bool on) { updating_ = on; }

void Device::timeSynced() {
    if (timeSynced_) return;
    timeSynced_ = true;
    for (auto& fn : onTimeSynced_) fn();
}

void Device::channel(uint8_t ch) {
    if (ch == channel_) return;
    channel_ = ch;
    for (auto& fn : onChannel_) fn(ch);
}

void Device::address(uint32_t ip, const uint8_t* mac) {
    address_ = ip;
    memcpy(addressMac_, mac, sizeof(addressMac_));
    addressMacSet_ = true;
}

const uint8_t* Device::addressMac() const { return addressMacSet_ ? addressMac_ : info().mac; }

uint32_t Device::onlineMs() const { return online_ ? lastNowMs_ - onlineSinceMs_ : 0; }

void Device::updateState(uint32_t nowMs) {
    bool online = started_ && netState_ == State::Online;
    if (online && !online_) onlineSinceMs_ = nowMs;
    online_ = online;
    State s = !started_ ? State::Booting
              : updating_ ? State::Updating
              : failed_   ? State::Error
                          : netState_;
    if (s != state_) {
        state_ = s;
        if (stateNode_) stateNode_->changed();
    }
}

#if !defined(ESP_PLATFORM)
// The host has no platform of its own: tests fill Device::platform().
bool Device::startServer() { return true; }
bool Device::startTask() {
    tesser::logWarning("tesserkit: no task on the host; call Device::loop()");
    return true;
}
tesser::Storage* Device::defaultStorage() { return nullptr; }
Core* espCore() { return nullptr; }
Wifi* espWifi() { return nullptr; }
Ethernet* espEthernet() { return nullptr; }
Mdns* espMdns() { return nullptr; }
Clock* espClock() { return nullptr; }
Ota* espOta() { return nullptr; }
HttpClient* espHttpClient() { return nullptr; }
Coredump* espCoredump() { return nullptr; }
Udp* espUdp() { return nullptr; }
MqttClient* espMqtt() { return nullptr; }
MqttClient* espMqttTls() { return nullptr; }
Ble* espBle() { return nullptr; }
NowTpRadio* espNowTp() { return nullptr; }
Filesystem* espFilesystem() { return nullptr; }
LogOutput* espLog() { return nullptr; }
Power* espPower() { return nullptr; }
Pins* espPins() { return nullptr; }
UdpApiTransport* espUdpApi() { return nullptr; }
#endif

}  // namespace tesserkit
