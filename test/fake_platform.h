// The platform in memory, for host tests (docs/DESIGN.md section 3.3): a
// Wi-Fi radio whose networks the test lays out, a clock the test sets, an OTA
// slot that is a byte vector, an HTTP client serving strings.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "tesserkit/device.h"

namespace fake {

using tesserkit::Event;
using tesserkit::EventSink;

struct Core : tesserkit::Core {
    tesserkit::BootInfo boot;
    uint64_t now = 1000;
    uint32_t heap = 180000, minHeap = 150000, psram = 0;
    bool hasSensor = false;
    float celsius = 41.5f;
    int restarts = 0;
    bool spawnFails = false;
    std::vector<std::function<void()>> tasks;  // spawned, run by runTasks()

    Core();
    void info(tesserkit::BootInfo& out) override { out = boot; }
    uint64_t uptimeMs() override { return now; }
    uint32_t freeHeap() override { return heap; }
    uint32_t minFreeHeap() override { return minHeap; }
    uint32_t freePsram() override { return psram; }
    bool temperature(float& c) override {
        c = celsius;
        return hasSensor;
    }
    void restart() override { restarts++; }
    bool spawn(const char*, uint32_t, std::function<void()> fn) override;
    void runTasks();
};

// A network on the air.
struct Air {
    std::string ssid, password;
    int8_t rssi = -60;
    uint8_t channel = 6;
    bool hidden = false;
};

struct Wifi : tesserkit::Wifi {
    EventSink sink;
    std::string hostnameSet;
    int8_t tx = 0;
    std::vector<Air> air;
    bool scanStarts = true;
    bool scanSilent = false;  // a scan that never ends
    int scans = 0;
    bool connectSilent = false;  // connections that never answer
    int connects = 0;
    tesserkit::StaConfig last;
    bool linked = false;
    tesserkit::StaInfo link;
    bool apUp = false;
    tesserkit::ApConfig ap;
    int apStarts = 0;
    bool captive = false;
    bool mdnsOn = false;
    std::string mdnsHost, mdnsVersion;
    uint16_t mdnsPort = 0;

    void begin(const char* hostname, EventSink s) override {
        hostnameSet = hostname;
        sink = std::move(s);
    }
    void hostname(const char* name) override { hostnameSet = name; }
    void txPower(int8_t dBm) override { tx = dBm; }
    bool scan() override;
    std::vector<tesserkit::ScanResult> scanResults() override;
    bool connect(const tesserkit::StaConfig& c) override;
    void disconnect() override;
    bool staInfo(tesserkit::StaInfo& out) override {
        if (linked) out = link;
        return linked;
    }
    bool startAp(const tesserkit::ApConfig& c) override {
        ap = c;
        apUp = true;
        apStarts++;
        return true;
    }
    void stopAp() override { apUp = false; }
    uint32_t apIp() override { return 0xC0A80401; }  // 192.168.4.1
    void captivePortal(bool on) override { captive = on; }
    void mdns(bool on, const char* host, const char* version, uint16_t port) override {
        mdnsOn = on;
        mdnsPort = port;
        mdnsHost = host;
        mdnsVersion = version;
    }
    // The router went away.
    void drop();
    void post(Event::Type type, int32_t value = 0) {
        Event e;
        e.type = type;
        e.value = value;
        sink(e);
    }
};

struct Clock : tesserkit::Clock {
    EventSink sink;
    int64_t epoch = 0;
    std::string tz;
    bool sntpOn = false;
    std::string server;
    int sntpCalls = 0;

    void begin(EventSink s) override { sink = std::move(s); }
    int64_t epochMs() override { return epoch; }
    void setEpochMs(int64_t ms) override { epoch = ms; }
    void timezone(const char* posix) override;
    void sntp(bool on, const char* srv) override {
        sntpOn = on;
        server = srv;
        sntpCalls++;
    }
    // SNTP answered.
    void sync(int64_t ms) {
        epoch = ms;
        Event e;
        e.type = Event::Type::TimeSynced;
        sink(e);
    }
};

struct Ota : tesserkit::Ota {
    uint32_t slot = 0x1F0000;
    std::string written;  // the slot being written
    std::string installed;  // the image set to boot
    bool open_ = false;
    int aborts = 0;
    bool verifyFails = false;
    bool pendingImage = false;
    int confirms = 0;
    bool canRollBack = true;
    int rollbacks = 0;

    uint32_t nextSlotSize() override { return slot; }
    tesser::Check open(uint32_t size) override;
    tesser::Check write(const uint8_t* data, size_t len) override;
    tesser::Check finish() override;
    void abort() override {
        open_ = false;
        aborts++;
    }
    bool pending() override { return pendingImage; }
    bool confirm() override {
        confirms++;
        pendingImage = false;
        return true;
    }
    tesser::Check rollback() override;
};

struct HttpClient : tesserkit::HttpClient {
    std::map<std::string, std::string> files;
    bool announceLength = true;
    tesser::Check get(const char* url, const std::function<tesser::Check(int64_t)>& length,
                      const std::function<tesser::Check(const uint8_t*, size_t)>& data) override;
};

struct Coredump : tesserkit::Coredump {
    std::string data;
    uint32_t size() override { return static_cast<uint32_t>(data.size()); }
    int read(uint32_t offset, uint8_t* buf, size_t len) override;
    bool erase() override {
        data.clear();
        return true;
    }
};

struct Platform {
    Core core;
    Wifi wifi;
    Clock clock;
    Ota ota;
    HttpClient http;
    Coredump coredump;
    void install(tesserkit::Device& dev, bool withCoredump = true) {
        tesserkit::Platform& p = dev.platform();
        p.core = &core;
        p.wifi = &wifi;
        p.clock = &clock;
        p.ota = &ota;
        p.http = &http;
        p.coredump = withCoredump ? &coredump : nullptr;
    }
};

// An app image for `chipId` of `size` bytes (at least 36).
std::string image(uint16_t chipId, size_t size);

}  // namespace fake
