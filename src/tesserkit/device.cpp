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

bool reserveTls(std::string_view uri, tesser::Reservation& held) {
    held.release();
    bool tls = uri.substr(0, 8) == "https://" || uri.substr(0, 8) == "mqtts://" || uri.substr(0, 6) == "wss://";
    if (!tls) return true;
    held = tesser::reserveBytes(kTlsBytes);
    return static_cast<bool>(held);
}

namespace {
// Services constructed and not yet taken by their device's begin(), in
// construction order. Constant-initialized, so it is valid before any
// constructor runs: services are often globals, and so is their Device,
// whose own members a constructor in another translation unit can't use.
Service* g_registered = nullptr;
}  // namespace

Service::Service(Device& dev, const char* name) : dev_(dev), name_(name) {
    // Registration order is mount order.
    Service** p = &g_registered;
    while (*p) p = &(*p)->next_;
    *p = this;
}

Service::~Service() {
    // One never taken by a device (destroyed before begin()).
    for (Service** p = &g_registered; *p; p = &(*p)->next_) {
        if (*p == this) {
            *p = next_;
            break;
        }
    }
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

Device& Device::settingsVersion(uint16_t version, Migration migrate) {
    appVersion_ = version;
    appMigrate_ = std::move(migrate);
    return *this;
}

bool Device::migrate(std::string_view record, uint32_t version, JsonVariant value) {
    uint32_t kit = version & 0xFFFF, app = version >> 16;
    if (kit < kSettingsVersion) {
        // Version 1 (2026-10-14) made the records sparse. Every value stored
        // before it, every default frozen at first boot, equals today's
        // default: no persisted /sys default changed from TesserKIT's first
        // release to then (v0.1.0 to v0.1.19). So no service drops anything
        // yet, and TesserAPI drops those values as it rewrites the records.
        // A later default change raises kSettingsVersion, and its service's
        // migrate() drops the old default.
        for (Service* s = services_; s; s = s->next_) {
            JsonObject o;
            if (record == "sys") {
                o = value[s->name()].as<JsonObject>();
            } else if (record == s->name() && !sys_->child(s->name())) {
                o = value.as<JsonObject>();  // a service at the root (Sequencer)
            }
            if (!o.isNull()) s->migrate(kit, o);
        }
    }
    if (appMigrate_ && app != appVersion_) return appMigrate_(record, app, value);
    return true;
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
    // Take this device's services from the registered ones, in order.
    Service** tail = &services_;
    while (*tail) tail = &(*tail)->next_;
    for (Service** p = &g_registered; *p;) {
        Service* s = *p;
        if (&s->dev_ != this) {
            p = &s->next_;
            continue;
        }
        *p = s->next_;
        s->next_ = nullptr;
        *tail = s;
        tail = &s->next_;
        if (Files* f = s->asFiles()) files_ = f;
    }
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
        // TesserKIT's version and the application's, so each migrates its own
        // settings (docs/DESIGN.md section 6.3).
        api_.persistVersion((static_cast<uint32_t>(appVersion_) << 16) | kSettingsVersion,
                            [this](std::string_view record, uint32_t version, JsonVariant value) {
                                return migrate(record, version, value);
                            });
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
    uint32_t dropped, mqttDropped;
    {
        tesser::MutexGuard guard(eventsMutex_);
        events.swap(events_);
        mqttQueued_ = 0;
        dropped = dropped_;
        mqttDropped = mqttDropped_;
    }
    // Told here rather than in post(), which runs in the platform's tasks.
    if (dropped != droppedLogged_) {
        tesser::logWarning("tesserkit: event queue full: %u events dropped", static_cast<unsigned>(dropped - droppedLogged_));
        droppedLogged_ = dropped;
    }
    if (mqttDropped != mqttDroppedSeen_) {
        tesser::logWarning("tesserkit: event queue full: %u MQTT messages dropped",
                           static_cast<unsigned>(mqttDropped - mqttDroppedSeen_));
        mqttDroppedSeen_ = mqttDropped;
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
    poll();
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

void Device::poll() {
    for (auto& fn : pollers_) fn();
}

void Device::post(Event e) {
    tesser::MutexGuard guard(eventsMutex_);
    // A flag: one pending says it all.
    if (e.type == Event::Type::PeersChanged || e.type == Event::Type::NowTpPeers) {
        for (const Event& q : events_) {
            if (q.type == e.type) return;
        }
    }
    // MQTT's messages can come in bursts (a broker's retained commands at
    // each connection): they have a share of the queue, and give way to
    // every other event. The oldest goes: the latest command for a topic
    // is the one that counts.
    bool mqtt = e.type == Event::Type::MqttMessage;
    if (mqttQueued_ > 0 && ((mqtt && mqttQueued_ >= kMaxMqttMessages) || events_.size() >= kMaxEvents)) {
        for (auto it = events_.begin(); it != events_.end(); ++it) {
            if (it->type != Event::Type::MqttMessage) continue;
            events_.erase(it);
            mqttQueued_--;
            mqttDropped_++;
            break;
        }
    }
    if (events_.size() >= kMaxEvents) {
        // Full of other events: what keeps state reads it back from the
        // platform (Network), so a lost edge is late, not lost.
        if (mqtt) {
            mqttDropped_++;
        } else {
            dropped_++;
        }
        return;
    }
    if (mqtt) mqttQueued_++;
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

uint32_t Device::reachableMs() const { return reachable_ ? lastNowMs_ - reachableSinceMs_ : 0; }

void Device::updateState(uint32_t nowMs) {
    bool online = started_ && netState_ == State::Online;
    if (online && !online_) onlineSinceMs_ = nowMs;
    online_ = online;
    bool reachable = online || (started_ && apClients_ > 0);
    if (reachable && !reachable_) reachableSinceMs_ = nowMs;
    reachable_ = reachable;
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
void Device::wake() {}
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
