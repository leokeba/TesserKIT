// The platform in memory, for host tests (docs/DESIGN.md section 3.3): a
// Wi-Fi radio whose networks the test lays out, a clock the test sets, an OTA
// slot that is a byte vector, an HTTP client serving strings.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
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
    uint8_t seed = 1;
    void random(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = static_cast<uint8_t>(seed++ * 37 + 11);
    }
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
    // The router went away.
    void drop();
    void post(Event::Type type, int32_t value = 0) {
        Event e;
        e.type = type;
        e.value = value;
        sink(e);
    }
};

struct Mdns : tesserkit::Mdns {
    bool on = false;
    std::string host, version;
    uint16_t port = 0;
    void set(bool o, const char* h, const char* v, uint16_t p) override {
        on = o;
        host = h;
        version = v;
        port = p;
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

// Serves `files` with status 200, or the status in `statuses`; anything
// else is a 404.
struct HttpClient : tesserkit::HttpClient {
    std::map<std::string, std::string> files;
    std::map<std::string, int> statuses;
    bool announceLength = true;
    size_t cutAt = 0;      // bytes after which the transfer fails (0: never)
    bool unreachable = false;
    std::vector<std::string> requests;
    tesser::Check get(const char* url, const std::function<tesser::Check(tesserkit::HttpResponse&)>& fn) override;
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

// UDP: datagrams the test queues for a port, and those sent.
struct Datagram {
    uint32_t ip = 0;
    uint16_t port = 0;
    std::string data;
};

struct Udp : tesserkit::Udp {
    std::map<uint16_t, std::deque<Datagram>> inbox;  // by local port
    std::vector<Datagram> sent;
    std::map<uint16_t, bool> open_;  // ports with a socket
    bool fails = false;
    int opens = 0;

    std::unique_ptr<tesserkit::UdpSocket> open(uint16_t port) override;
    // A datagram from `ip`:`fromPort` to the local `port`.
    void deliver(uint16_t port, const std::string& data, uint32_t ip = 0xC0A80164, uint16_t fromPort = 6454) {
        inbox[port].push_back({ip, fromPort, data});
    }
};

// An MQTT client and its broker: the test connects it, sends messages and
// reads what was published.
struct Mqtt : tesserkit::MqttClient {
    struct Message {
        std::string topic, payload;
        bool retain = false;
    };
    tesserkit::EventSink sink;
    tesserkit::MqttConfig config;
    int32_t session = 0, sessions = 0;
    bool running = false;
    bool full = false;  // the outbox refuses messages
    int stops = 0;
    std::vector<Message> published;
    std::map<std::string, Message> retained;  // the broker's

    int32_t start(const tesserkit::MqttConfig& c, tesserkit::EventSink s) override {
        config = c;
        sink = std::move(s);
        running = true;
        return session = ++sessions;
    }
    void stop() override;
    bool publish(const std::string& topic, const std::string& payload, bool retain) override;
    // The broker accepted the connection, or lost it.
    void connect();
    void drop(const char* why = "connection lost");
    // A message on a topic the client subscribed to.
    void message(const std::string& topic, const std::string& payload);
    // What was last published to `topic` ("<none>" if nothing).
    std::string last(const std::string& topic) const;
};

// Bluetooth LE: the test connects a client and reads what was advertised.
struct Ble : tesserkit::Ble {
    bool started = false, advertising = false, connectedNow = false;
    bool runtime = true;     // advertise() and name() work while running
    bool releasable = true;  // end() gives the stack's memory back
    std::string advertised, clientAddress;
    int begins = 0, ends = 0;
    bool begin(tesser::Api&, const char* name) override {
        begins++;
        started = advertising = true;
        advertised = name;
        return true;
    }
    bool end() override {
        if (!releasable) return false;
        ends++;
        started = advertising = connectedNow = false;
        return true;
    }
    bool advertise(bool on) override {
        if (!runtime) return false;
        advertising = on;
        if (!on) connectedNow = false;
        return true;
    }
    bool name(const char* n) override {
        if (!runtime) return false;
        advertised = n;
        return true;
    }
    bool connected() override { return connectedNow; }
    std::string client() override { return clientAddress; }
};

// NowTP: the test adds nodes nearby and finishes pairings.
struct NowTp : tesserkit::NowTpRadio {
    tesserkit::EventSink sink;
    tesserkit::NowTpConfig config;
    tesserkit::NowTpStatus state;
    std::vector<tesserkit::NowTpPeer> nearby;
    tesser::Object* mountedUnder = nullptr;
    bool fixedChannel = false;  // an access point fixes the channel
    int begins = 0, ends = 0, pairings = 0, accepts = 0, forgets = 0;
    bool keyStored = false;

    bool begin(tesser::Api&, tesser::Object* peers, const tesserkit::NowTpConfig& c,
               tesserkit::EventSink s) override {
        begins++;
        config = c;
        sink = std::move(s);
        mountedUnder = peers;
        state.started = true;
        state.channel = c.channel;
        state.secured = keyStored;
        return true;
    }
    void end() override {
        ends++;
        state = tesserkit::NowTpStatus();
    }
    std::vector<tesserkit::NowTpPeer> peers() override { return nearby; }
    tesserkit::NowTpStatus status() override { return state; }
    bool channel(uint8_t ch) override {
        if (fixedChannel) return false;
        state.channel = ch;
        return true;
    }
    bool longRange(bool on) override {
        config.longRange = on;
        return true;
    }
    bool pair(uint32_t) override {
        pairings++;
        return true;
    }
    bool acceptPairing(uint32_t) override {
        accepts++;
        return true;
    }
    bool forgetKey() override {
        forgets++;
        keyStored = state.secured = false;
        return true;
    }
    // A pairing finished, with the other node's MAC, or failed.
    void paired(bool ok, const char* text) {
        if (ok) keyStored = state.secured = true;
        tesserkit::Event e;
        e.type = tesserkit::Event::Type::NowTpPairing;
        e.value = ok ? 0 : 1;
        e.text = text;
        sink(e);
    }
    void found(const char* name, const char* mac, bool tesser = true) {
        tesserkit::NowTpPeer p;
        p.name = name;
        p.mac = mac;
        p.rssi = -50;
        p.channel = state.channel;
        p.tesser = tesser;
        nearby.push_back(p);
        tesserkit::Event e;
        e.type = tesserkit::Event::Type::NowTpPeers;
        sink(e);
    }
};

// A filesystem in a temporary directory of the host, removed with it. Files
// is given that directory as its root (Files::root(fs.dir)).
struct Filesystem : tesserkit::Filesystem {
    Filesystem();
    ~Filesystem() override;
    std::string dir;
    uint64_t total = 262144;  // the partition's size; used is what the files hold
    bool present = true;      // false: no partition
    std::string label, root;  // what mount() got
    int formats = 0;
    bool mount(const char* label, const char* root) override;
    bool usage(uint64_t& total, uint64_t& used) override;
    bool format() override;
};

struct Platform {
    Core core;
    Wifi wifi;
    Mdns mdns;
    Clock clock;
    Ota ota;
    HttpClient http;
    Coredump coredump;
    Udp udp;
    Mqtt mqtt;
    Ble ble;
    NowTp nowtp;
    Filesystem fs;
    void install(tesserkit::Device& dev, bool withCoredump = true) {
        tesserkit::Platform& p = dev.platform();
        p.core = &core;
        p.wifi = &wifi;
        p.mdns = &mdns;
        p.clock = &clock;
        p.ota = &ota;
        p.http = &http;
        p.coredump = withCoredump ? &coredump : nullptr;
        p.udp = &udp;
        p.mqtt = &mqtt;
        p.ble = &ble;
        p.nowtp = &nowtp;
        p.fs = &fs;
    }
};

// An app image for `chipId` of `size` bytes (at least 36).
std::string image(uint16_t chipId, size_t size);

}  // namespace fake
