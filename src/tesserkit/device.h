#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "tesser/api.h"
#include "tesser/storage.h"
#include "tesserkit/platform.h"

#if defined(ESP_PLATFORM)
#include "tesser/transports/http_server.h"
#include "tesserui/web_ui.h"
#endif

namespace tesserkit {

class Device;
class Files;

// The device's overall state (docs/DESIGN.md section 4.3).
enum class State : uint8_t { Booting, Provisioning, Connecting, Online, Updating, Error };
const char* toString(State s);

// The node at `path` below `root` ("/a/b" or "a/b"), or null. Below a list,
// an array, a remote node or a `*` segment (every child), elements aren't
// known here: null, with `opaque` set. The root itself is "/".
const tesser::Node* findNode(const tesser::Object& root, std::string_view path, bool* opaque = nullptr);
// Whether `path` names a node below `root`, or may (below a list, an array,
// a remote node or a `*`).
bool resolves(const tesser::Object& root, std::string_view path);

// What a TLS session (HTTPS, MQTT over TLS) takes from the heap at its peak,
// its task included: about 34 KB measured on a classic ESP32 fetching a
// GitHub release, with a margin. Reserved from TesserAPI's byte budget
// before a session starts (docs/DESIGN.md section 11), which keeps its heap
// floor (8 KB on target) on top: about 44 KB free, as before the budget.
constexpr size_t kTlsBytes = 36 * 1024;
// kTlsBytes held in `held` for a session to `uri` if it uses TLS
// (https://, mqtts://, wss://); true without reserving for one that
// doesn't. False if the budget refused them. Settle `held` once the
// session's buffers are allocated, so they don't count twice.
bool reserveTls(std::string_view uri, tesser::Reservation& held);

// A service: a subtree of /sys with a platform side (docs/DESIGN.md section
// 5). Constructing one registers it for its device; Device::begin() then
// mounts it, loads the persisted state, and starts it. Registering doesn't
// touch the device: a service that is a global may be constructed before a
// Device that is a global of another translation unit.
class Service {
public:
    virtual ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    const char* name() const { return name_; }
    // Mounts the service under /sys: its own child object by default.
    virtual void attach(tesser::Object& sys) { describe(sys.object(name_)); }
    // Describes the service's tree into `o`. Public, so an application can
    // mount it elsewhere (docs/DESIGN.md section 3.2).
    virtual void describe(tesser::Object& o) = 0;

protected:
    Service(Device& dev, const char* name);
    friend class Device;

    // After the persisted state is loaded: start the platform side.
    virtual void start() {}
    // Every period of the device's task, under the API lock.
    virtual void tick(uint32_t nowMs) { (void)nowMs; }
    // A platform event, in the device's task, under the API lock.
    virtual void event(const Event& e) { (void)e; }
    // A factory reset, before the restart: erase what the service keeps
    // outside the device's storage (a radio's pairing key).
    virtual void factoryReset() {}
    // The Files service, which Device::files() returns.
    virtual Files* asFiles() { return nullptr; }
    // Settings stored under an older TesserKIT settings version `from`
    // (below Device::kSettingsVersion; 0: before versions), before they are
    // loaded: the service's own object as stored (/sys/<name>, or /<name>
    // for a service at the root), to change in place: rename keys, or drop
    // a value equal to a default that has since changed (docs/DESIGN.md
    // section 6.3).
    virtual void migrate(uint32_t from, JsonObject settings) {
        (void)from;
        (void)settings;
    }

    Device& dev_;

private:
    const char* name_;
    Service* next_ = nullptr;
};

// One per firmware: the API, its HTTP server and WebSocket, the TesserUI
// page, persistence, and the task that runs the services (docs/DESIGN.md
// section 3.1). Declare it before the services that use it.
class Device {
public:
    // `label` names the device (shown by TesserUI) and makes its hostname.
    // The string must outlive the device.
    explicit Device(const char* label);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Options, before begin().
    Device& hostname(const char* name);  // default: the label and the MAC's end
    Device& version(const char* version);  // default: the app description's
    Device& ui(bool on);                   // serve TesserUI at / (default true)
    Device& httpPort(uint16_t port);       // default 80; 0: no HTTP server
    uint16_t httpPort() const { return httpPort_; }
    Device& cors(const char* origin);      // CORS headers for other origins
    Device& task(bool own);  // false: the application calls loop() itself
    Device& storage(tesser::Storage& storage);  // default: NVS, namespace "tesserkit"
    // URI handler slots for the application's own routes (default 4).
    Device& extraHandlers(uint8_t n);
    // The version of the application's own persisted state (default 0), and
    // the migration that brings state stored under an older one up to it:
    // `migrate(record, version, value)` sees each stored top-level record
    // ("sys" included, after TesserKIT's own migration of it) with the
    // application's version it was stored under, and changes it in place;
    // false drops the record. Raise the version when a default changes
    // (docs/DESIGN.md section 6.3).
    using Migration = tesser::Api::Migration;
    Device& settingsVersion(uint16_t version, Migration migrate = nullptr);
    // TesserKIT's own settings version, for /sys. Stored with the
    // application's as (application << 16) | TesserKIT's.
    static constexpr uint16_t kSettingsVersion = 1;

    // Mounts the services, loads the persisted state, starts the services,
    // the HTTP server and the task. False if the platform is missing.
    bool begin();
    // One period of the device's task: platform events, persistence,
    // notifications, the services' ticks. Only with task(false).
    void loop();

    tesser::Api& api() { return api_; }
    // The /sys object, for an application's own system-level nodes.
    tesser::Object& sys() { return *sys_; }
    const char* label() const { return label_; }
    const std::string& hostname() const { return hostname_; }
    const std::string& version() const { return version_; }
    const BootInfo& info() const { return info_; }
    // The services' platform interfaces. Tests fill it with fakes before
    // begin(); on ESP, what is left empty is the ESP implementation.
    Platform& platform() { return platform_; }
    Core& core() { return *platform_.core; }
    // The Files service, if the application has one (docs/DESIGN.md
    // section 5.12): other services keep their files there.
    Files* files() const { return files_; }
#if defined(ESP_PLATFORM)
    tesser::HttpServer& http() { return http_; }
    tesserui::WebUi& web() { return web_; }
#endif

    State state() const { return state_; }
    // Called in the device's task when the state changes.
    void onState(std::function<void(State)> fn) { onState_.push_back(std::move(fn)); }
    // Called every period of the device's task.
    void onTick(std::function<void(uint32_t nowMs)> fn) { onTick_.push_back(std::move(fn)); }
    // A transport without a task of its own (NowTp::ownTask(false)): `fn`
    // runs in the device's task, without the API lock, every period and at
    // once after wake(). Before begin().
    void poller(std::function<void()> fn) { pollers_.push_back(std::move(fn)); }
    // Wakes the device's task to run the pollers now, from any task (a
    // transport's message arrived). Without the device's own task, the next
    // loop() runs them.
    void wake();
    // Runs the pollers: the device's task, when woken.
    void poll();
    // Called once, on the first valid time after boot (Time).
    void onTimeSynced(std::function<void()> fn) { onTimeSynced_.push_back(std::move(fn)); }
    // Called when the station's Wi-Fi channel changes (Network).
    void onChannel(std::function<void(uint8_t)> fn) { onChannel_.push_back(std::move(fn)); }
    // Called in the device's task just before it goes to deep sleep (Sleep):
    // park the hardware, hold its pins.
    void onSleep(std::function<void()> fn) { onSleep_.push_back(std::move(fn)); }
    // The application's own error state (State::Error while set).
    void fail(bool failed);

    // Restarts `ms` from now, from the device's task. Thread-safe.
    void restartIn(uint32_t ms);
    // Erases the persisted state, then restarts. Thread-safe: the erase runs
    // in the device's task at its next loop (services' factoryReset()
    // included), so an action calling this replies first.
    void factoryReset();
    // Milliseconds the device has been online (0 if it isn't).
    uint32_t onlineMs() const;
    // Milliseconds the device has been within clients' reach: online, or with
    // a station joined to its access point (0 if neither).
    uint32_t reachableMs() const;
    // The IPv4 address the device is reached at, a.b.c.d as (a << 24) | ...:
    // Ethernet's, the station's, else the access point's; 0 if none (Network).
    uint32_t address() const { return address_; }
    // The MAC of the interface address() is on (the station's on Wi-Fi):
    // six bytes, the station's until Network sets it.
    const uint8_t* addressMac() const;

    // Used by services.
    // Queues a platform event for the device's task. Thread-safe. The queue
    // holds 32 events, at most 8 of them MqttMessage (the oldest is dropped
    // for a newer one, and for any other event when the queue is full), and
    // one PeersChanged and one NowTpPeers. Drops are logged by the task.
    void post(Event e);
    // MQTT messages dropped so far because the queue was full, as of the
    // task's last period (Mqtt's status/missed).
    uint32_t mqttMessagesDropped() const { return mqttDroppedSeen_; }
    // The network's part of the state: Provisioning, Connecting or Online.
    // Without a Network service, a started device is Online.
    void networkState(State s);
    void updating(bool on);
    void timeSynced();
    void channel(uint8_t ch);
    void address(uint32_t ip, const uint8_t* mac);
    // Stations joined to the access point (Network).
    void apClients(int32_t n) { apClients_ = n; }
    // The device is about to sleep: runs the onSleep() callbacks, then saves
    // what the persistence's debounce holds.
    void sleeping();
    // The state value (/sys/status/state), marked changed with the state.
    State& stateVar() { return state_; }
    void stateNode(tesser::Node* node) { stateNode_ = node; }

private:
    bool startServer();  // platform/esp/device_esp.cpp; nothing on the host
    bool startTask();
    tesser::Storage* defaultStorage();
    void updateState(uint32_t nowMs);
    void resetNow();  // factoryReset()'s work, in the device's task under the API lock
    std::string defaultHostname() const;
    // TesserAPI's migration: TesserKIT's services, then the application's.
    bool migrate(std::string_view record, uint32_t version, JsonVariant value);

    const char* label_;
    tesser::Api api_;
    tesser::Object* sys_ = nullptr;
    Platform platform_;
    BootInfo info_;
    tesser::Storage* storage_ = nullptr;
    uint16_t appVersion_ = 0;
    Migration appMigrate_;
    Service* services_ = nullptr;
    Files* files_ = nullptr;
    std::string hostname_;
    std::string version_;
    bool ui_ = true;
    bool ownTask_ = true;
    bool started_ = false;
    bool failed_ = false;
    bool updating_ = false;
    bool timeSynced_ = false;
    uint8_t channel_ = 0;
    uint32_t address_ = 0;
    uint8_t addressMac_[6] = {};
    bool addressMacSet_ = false;
    uint8_t extraHandlers_ = 4;
    uint16_t httpPort_ = 80;
    const char* cors_ = nullptr;
    State netState_ = State::Online;
    State state_ = State::Booting;
    tesser::Node* stateNode_ = nullptr;
    uint32_t onlineSinceMs_ = 0;
    bool online_ = false;
    int32_t apClients_ = 0;
    uint32_t reachableSinceMs_ = 0;
    bool reachable_ = false;
    uint32_t lastNowMs_ = 0;
    static constexpr uint32_t kNoRestart = 0xFFFFFFFF;
    std::atomic<uint32_t> restartDelayMs_{kNoRestart};  // set by restartIn()
    bool restartArmed_ = false;
    uint32_t restartAtMs_ = 0;
    std::atomic<bool> resetWanted_{false};     // set by factoryReset()
    tesser::MemoryStorage* erased_ = nullptr;  // after the reset: saves go nowhere

    static constexpr size_t kMaxEvents = 32;
    static constexpr size_t kMaxMqttMessages = 8;
    tesser::Mutex eventsMutex_;  // never held with the API lock
    std::vector<Event> events_;  // and the counters below, under eventsMutex_
    size_t mqttQueued_ = 0;      // MqttMessage events in events_
    uint32_t dropped_ = 0, mqttDropped_ = 0;
    uint32_t droppedLogged_ = 0, mqttDroppedSeen_ = 0;  // the device's task's copies

    std::vector<std::function<void(State)>> onState_;
    std::vector<std::function<void(uint32_t)>> onTick_;
    std::vector<std::function<void()>> pollers_;
    std::atomic<void*> task_{nullptr};  // the device's task (ESP: its handle), for wake()
    std::vector<std::function<void()>> onTimeSynced_;
    std::vector<std::function<void(uint8_t)>> onChannel_;
    std::vector<std::function<void()>> onSleep_;

#if defined(ESP_PLATFORM)
    tesser::HttpServer http_;
    tesserui::WebUi web_;
#endif

    friend class Service;
};

}  // namespace tesserkit
