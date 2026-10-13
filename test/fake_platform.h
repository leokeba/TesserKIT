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

#include "tesser/udp_link.h"
#include "tesserkit/device.h"

namespace fake {

using tesserkit::Event;
using tesserkit::EventSink;

struct Core : tesserkit::Core {
    tesserkit::BootInfo boot;
    uint64_t now = 1000;
    uint32_t heap = 180000, minHeap = 150000, maxBlock = 110000, psram = 0;
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
    uint32_t largestFreeBlock() override { return maxBlock; }
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
    uint8_t bssid[6] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
    int8_t rssi = -60;
    uint8_t channel = 6;
    bool hidden = false;
};

struct Wifi : tesserkit::Wifi {
    EventSink sink;
    std::string hostnameSet;
    int8_t tx = 0;
    tesserkit::PowerSave ps = tesserkit::PowerSave::Min;
    int psSets = 0;
    std::vector<Air> air;
    bool scanStarts = true;
    bool scanSilent = false;  // a scan that never ends
    int scans = 0;
    bool connectSilent = false;  // connections that never answer
    bool dhcpSilent = false;     // associated, but no address comes (giveAddress())
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
    void powerSave(tesserkit::PowerSave mode) override {
        ps = mode;
        psSets++;
    }
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
    // DHCP answers, late.
    void giveAddress() {
        linked = true;
        post(Event::Type::StaGotIp);
    }
    void post(Event::Type type, int32_t value = 0) {
        Event e;
        e.type = type;
        e.value = value;
        sink(e);
    }
};

// A cable the test plugs in, and a DHCP server that answers it (or not).
struct Ethernet : tesserkit::Ethernet {
    EventSink sink;
    bool starts = true;
    int begins = 0;
    tesserkit::EthernetBoard board;
    std::string hostnameSet;
    tesserkit::IpConfig ip;
    int addressCalls = 0;
    bool dhcp = true;     // a DHCP server on the cable
    bool autoip = false;  // the stack takes a link-local address without DHCP (no event)
    bool on = true;       // enabled()
    tesserkit::EthInfo state;

    bool begin(const tesserkit::EthernetBoard& b, const char* hostname, const tesserkit::IpConfig& c,
               EventSink s) override {
        begins++;
        if (!starts) return false;
        board = b;
        hostnameSet = hostname;
        ip = c;
        sink = std::move(s);
        for (int i = 0; i < 6; i++) state.mac[i] = static_cast<uint8_t>(0xe0 + i);
        return true;
    }
    void hostname(const char* name) override { hostnameSet = name; }
    void address(const tesserkit::IpConfig& c) override;
    void enabled(bool o) override;
    bool info(tesserkit::EthInfo& out) override {
        out = state;
        return static_cast<bool>(sink);
    }
    void plug();
    void unplug();
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

    // TesserAPI's endpoint, its messages kept in `sent` for a test to carry.
    struct Message {
        tesser::PeerAddress to;
        std::string text;
    };
    const uint64_t* clock = nullptr;  // the fake core's
    std::unique_ptr<tesser::DatagramEndpoint> ep;
    std::vector<Message> sent;
    tesser::DatagramEndpoint* endpoint() override { return ep.get(); }
    int polls = 0;
    void poll() override {
        polls++;
        if (ep && !config.runTask) ep->process();
    }

    bool begin(tesser::Api& api, tesser::Object* peers, const tesserkit::NowTpConfig& c,
               tesserkit::EventSink s) override {
        begins++;
        if (!ep) {
            tesser::DatagramEndpoint::Options o;
            o.maxCalls = c.maxCalls;
            ep.reset(new tesser::DatagramEndpoint(
                &api, tesser::TransportKind::NowTP,
                [this](const tesser::PeerAddress& to, const std::string& m, tesser::Delivery) {
                    sent.push_back(Message{to, m});
                    return true;
                },
                [this] { return clock ? static_cast<uint32_t>(*clock) : 0; }, o));
            if (peers) ep->mountPeers(*peers);
        }
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

// The log: the test writes lines as the firmware would.
struct Log : tesserkit::LogOutput {
    bool began = false;
    size_t capacity = 0;
    uint8_t lvl = 2, max = 5;
    std::deque<std::string> lines;
    size_t bytes = 0;
    uint32_t lost = 0;
    bool muted = false;
    int mutedLines = 0;  // written while muted: not captured

    bool begin(size_t b) override {
        began = true;
        capacity = b;
        return true;
    }
    uint8_t level() override { return lvl; }
    void level(uint8_t l) override { lvl = l; }
    uint8_t maxLevel() override { return max; }
    bool next(std::string& line) override {
        if (lines.empty()) return false;
        line = lines.front();
        bytes -= line.size();
        lines.pop_front();
        return true;
    }
    uint32_t dropped() override { return lost; }
    void mute(bool on) override { muted = on; }
    // A line the firmware logs.
    void write(const std::string& line) {
        if (muted) {
            mutedLines++;
        } else if (!began || bytes + line.size() > capacity) {
            lost++;
        } else {
            lines.push_back(line);
            bytes += line.size();
        }
    }
};

// Deep sleep that the test watches and wakes from.
struct Power : tesserkit::Power {
    tesserkit::WakeCause cause = tesserkit::WakeCause::None;
    int sleeps = 0;
    uint64_t us = 0;
    int8_t pin = -1;
    bool high = false, pull = false;
    tesserkit::WakeCause wakeCause() override { return cause; }
    bool wakePin(int8_t p) override { return p >= 0 && p <= 39 && p != 20 && p != 24; }
    void deepSleep(uint64_t u, int8_t p, bool h, bool pl) override {
        sleeps++;
        us = u;
        pin = p;
        high = h;
        pull = pl;
    }
};

// Pins whose voltages and levels the test sets.
struct Pins : tesserkit::Pins {
    std::map<int, uint32_t> mv;     // analog pins and their millivolts
    std::map<int, bool> level;      // digital inputs
    std::map<int, bool> pulled;     // inputBegin()'s
    bool analogBegin(int8_t pin) override { return mv.count(pin) > 0; }
    bool analogMv(int8_t pin, uint32_t& out) override {
        auto it = mv.find(pin);
        if (it == mv.end()) return false;
        out = it->second;
        return true;
    }
    bool inputBegin(int8_t pin, bool pullUp) override {
        pulled[pin] = pullUp;
        return level.count(pin) > 0;
    }
    bool input(int8_t pin) override { return level[pin]; }
};

// TesserAPI's UDP link over an in-memory network: what the device sends
// waits in `out` for the test to deliver (to another link), and the test
// hands it datagrams with packet() and runs it with tick(), as the
// transport's tasks do.
struct UdpApi : tesserkit::UdpApiTransport {
    struct Datagram {
        tesser::PeerAddress to;
        std::vector<uint8_t> data;
    };
    const uint64_t* clock = nullptr;  // the fake core's
    std::unique_ptr<tesser::UdpLink> l;
    std::vector<Datagram> out;
    bool opens = true;
    int begins = 0, ends = 0;
    uint16_t port = 0;
    std::string name;
    uint32_t nonce = 0x4000;

    size_t maxCalls = 0;
    bool ownTask = true;
    std::function<void()> queued;
    int polls = 0;
    uint8_t interfaces = 0;
    bool begin(tesser::Api& api, uint16_t p, const std::string& n, size_t calls, bool own,
               std::function<void()> q, uint8_t ifaces) override;
    void poll() override {
        polls++;
        if (l && !ownTask) l->tick();
    }
    void end() override {
        ends++;
        if (l) l->stop();
    }
    tesser::UdpLink* link() override { return l.get(); }
};

struct Platform {
    Core core;
    Wifi wifi;
    Ethernet ethernet;
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
    Log log;
    Power power;
    Pins pins;
    UdpApi udpApi;
    void install(tesserkit::Device& dev, bool withCoredump = true) {
        tesserkit::Platform& p = dev.platform();
        p.core = &core;
        p.wifi = &wifi;
        p.ethernet = &ethernet;
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
        p.log = &log;
        p.power = &power;
        p.pins = &pins;
        udpApi.clock = &core.now;
        nowtp.clock = &core.now;
        p.udpApi = &udpApi;
    }
};

// An app image for `chipId` of `size` bytes (at least 36).
std::string image(uint16_t chipId, size_t size);

}  // namespace fake
