#include "fake_platform.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

namespace fake {

using tesser::Check;
using tesser::Status;

namespace {
thread_local int t_notHeap = 0;
}  // namespace

NotHeap::NotHeap() { t_notHeap++; }
NotHeap::~NotHeap() { t_notHeap--; }
bool NotHeap::active() { return t_notHeap > 0; }

void Chaos::deliver(EventSink& sink, Event e) {
    uint64_t now = clock ? *clock : 0;
    // Behind an earlier event of its source still held: after it.
    uint64_t after = 0;
    for (const Held& h : held_) {
        if (h.sink == &sink && h.dueMs > after) after = h.dueMs;
    }
    if (!on) {
        if (after) {
            held_.push_back({after, &sink, std::move(e)});
        } else if (sink) {
            sink(std::move(e));
        }
        return;
    }
    if (chance(dropPercent)) {
        dropped++;
        return;
    }
    if (after || chance(delayPercent)) {
        delayed++;
        uint64_t due = now + 1 + below(maxDelayMs);
        held_.push_back({due > after ? due : after, &sink, std::move(e)});
        return;
    }
    if (sink) sink(std::move(e));
}

void Chaos::release() {
    uint64_t now = clock ? *clock : 0;
    std::vector<Held> due;
    for (auto it = held_.begin(); it != held_.end();) {
        if (it->dueMs <= now) {
            due.push_back(std::move(*it));
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    // Sources interleaved at random, each in its own order.
    std::stable_sort(due.begin(), due.end(), [](const Held& a, const Held& b) { return a.dueMs < b.dueMs; });
    while (!due.empty()) {
        // A random source's oldest.
        size_t pick = below(static_cast<uint32_t>(due.size()));
        EventSink* s = due[pick].sink;
        size_t first = 0;
        while (due[first].sink != s) first++;
        Held h = std::move(due[first]);
        due.erase(due.begin() + static_cast<std::ptrdiff_t>(first));
        if (*h.sink) (*h.sink)(std::move(h.e));
    }
}

Core::Core() {
    boot.chip = "ESP32-D0WD-V3 rev 3.0, 2 cores, 240 MHz";
    boot.target = "esp32";
    boot.imageChipId = 0;
    boot.flashSize = 4 * 1024 * 1024;
    boot.idf = "v5.4";
    const uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0xf4, 0xa6, 0x7c};
    memcpy(boot.mac, mac, 6);
    boot.resetReason = tesserkit::ResetReason::PowerOn;
    boot.version = "1.0.0";
    boot.project = "test";
    boot.built = "Oct 10 2026 12:00:00";
    boot.slot = "ota_0";
    boot.slotSize = 0x1F0000;
    boot.appSize = 900000;
}

bool Core::spawn(const char*, uint32_t, std::function<void()> fn) {
    if (spawnFails) return false;
    tasks.push_back(std::move(fn));
    return true;
}

void Core::runTasks() {
    std::vector<std::function<void()>> run;
    run.swap(tasks);
    for (auto& fn : run) fn();
}

bool Wifi::scan() {
    scans++;
    if (!scanStarts) return false;
    if (!scanSilent) post(Event::Type::ScanDone);
    return true;
}

std::vector<tesserkit::ScanResult> Wifi::scanResults() {
    resultsRead++;
    std::vector<tesserkit::ScanResult> out;
    for (const Air& a : air) {
        if (a.hidden) continue;
        tesserkit::ScanResult r;
        r.ssid = a.ssid;
        memcpy(r.bssid, a.bssid, sizeof(r.bssid));
        r.rssi = a.rssi;
        r.channel = a.channel;
        r.secure = !a.password.empty();
        out.push_back(r);
    }
    return out;
}

bool Wifi::connect(const tesserkit::StaConfig& c) {
    connects++;
    last = c;
    if (linked) disconnect();
    if (connectSilent) return true;
    // The strongest access point of the network, or the one asked for.
    const Air* best = nullptr;
    for (const Air& a : air) {
        if (a.ssid != c.ssid) continue;
        if (c.bssidSet && memcmp(a.bssid, c.bssid, sizeof(a.bssid)) != 0) continue;
        if (!best || a.rssi > best->rssi) best = &a;
    }
    if (best) {
        const Air& a = *best;
        if (a.password != c.password) {
            disconnected(tesserkit::StaReason::WrongPassword, 15);  // 4-way handshake timeout
            return true;
        }
        link = tesserkit::StaInfo();
        link.ssid = a.ssid;
        link.ip = c.staticIp ? c.ip : 0xC0A80132;  // 192.168.1.50
        link.rssi = a.rssi;
        link.channel = a.channel;
        memcpy(link.bssid, a.bssid, sizeof(link.bssid));
        if (ipStack) ipStack->list = 0;  // a DHCP client started, or a static address taken
        post(Event::Type::StaAssociated);
        if (!dhcpSilent) giveAddress();
        return true;
    }
    disconnected(tesserkit::StaReason::NotFound, 201);
    return true;
}

void Wifi::disconnect() {
    if (!linked) return;
    linked = false;
    if (ipStack) {
        ipStack->release(tesserkit::NetIf::Station);
        ipStack->changed();
    }
    disconnected(tesserkit::StaReason::Left, 8);
}

void Wifi::drop() {
    linked = false;
    if (ipStack) {
        ipStack->release(tesserkit::NetIf::Station);
        ipStack->changed();
    }
    disconnected(tesserkit::StaReason::Other, 200);  // beacon timeout
}

// The lease, or the static address, once the link is up.
static void lease(Ethernet& e) {
    if (!e.state.link) return;
    if (e.ip.staticIp) {
        e.state.ip = e.ip.ip;
        e.state.gateway = e.ip.gateway;
        if (e.ipStack) e.ipStack->list = 0;  // taking a static address empties it
    } else if (e.dhcp) {
        e.state.ip = 0xC0A8013C;  // 192.168.1.60
        e.state.gateway = e.leaseGateway;
        if (e.ipStack) e.ipStack->ack(tesserkit::NetIf::Ethernet);
    } else {
        if (e.autoip) {
            e.state.ip = 0xA9FE0A0B;  // 169.254.10.11, without an event
            e.state.gateway = 0;
            if (e.ipStack) e.ipStack->changed();
        }
        return;
    }
    if (e.ipStack) e.ipStack->changed();
    e.post(Event::Type::EthGotIp);
}

static void lost(Ethernet& e) {
    if (!e.ipStack) return;
    e.ipStack->release(tesserkit::NetIf::Ethernet);
    e.ipStack->changed();
}

void Ethernet::enabled(bool o) {
    on = o;
    if (!o && state.link) {
        state.link = false;
        state.ip = 0;
        state.gateway = 0;
        state.mbps = 0;
        lost(*this);
    }
}

void Ethernet::address(const tesserkit::IpConfig& c) {
    addressCalls++;
    ip = c;
    state.ip = 0;
    state.gateway = 0;
    lost(*this);
    if (ipStack && !c.staticIp) ipStack->list = 0;  // a DHCP client started
    lease(*this);
}

void Ethernet::plug() {
    if (!on) return;
    state.link = true;
    state.mbps = 100;
    state.fullDuplex = true;
    if (ipStack && !ip.staticIp) ipStack->list = 0;  // the link starts the DHCP client
    post(Event::Type::EthLink, 1);
    lease(*this);
}

void Ethernet::unplug() {
    state.link = false;
    state.ip = 0;
    state.gateway = 0;
    state.mbps = 0;
    lost(*this);
    post(Event::Type::EthLink, 0);
}

void Ethernet::serveDhcp() {
    dhcp = true;
    if (state.link && !state.ip && !ip.staticIp) lease(*this);
}

void Clock::timezone(const char* posix) {
    tz = posix;
    setenv("TZ", posix, 1);
    tzset();
}

Check Ota::open(uint32_t size) {
    if (pendingImage) return Check::fail(Status::Internal, "couldn't start the update");  // as ESP-IDF
    if (size > slot) return Check::fail(Status::TooLarge, "image larger than the slot");
    written.clear();
    open_ = true;
    return Check::ok();
}

Check Ota::write(const uint8_t* data, size_t len) {
    if (!open_) return Check::fail(Status::Internal, "not open");
    if (written.size() + len > slot) return Check::fail(Status::TooLarge, "image larger than the slot");
    NotHeap flash;
    written.append(reinterpret_cast<const char*>(data), len);
    return Check::ok();
}

Check Ota::finish() {
    if (!open_) return Check::fail(Status::Internal, "not open");
    open_ = false;
    if (verifyFails) return Check::fail(Status::InvalidValue, "image verification failed");
    NotHeap flash;
    installed = written;
    return Check::ok();
}

Check Ota::rollback() {
    if (!canRollBack) return Check::fail(Status::NotAllowed, "no previous firmware");
    rollbacks++;
    return Check::ok();
}

namespace {

struct Response : tesserkit::HttpResponse {
    int code = 200;
    const std::string* body = nullptr;
    bool announce = true;
    size_t cutAt = 0;
    size_t pos = 0;
    int status() override { return code; }
    int64_t length() override { return announce ? static_cast<int64_t>(body->size()) : -1; }
    int read(uint8_t* buf, size_t len) override {
        if (cutAt && pos >= cutAt) return -1;
        size_t n = body->size() - pos;
        if (n > len) n = len;
        if (n > 1000) n = 1000;
        if (cutAt && pos + n > cutAt) n = cutAt - pos;
        memcpy(buf, body->data() + pos, n);
        pos += n;
        return static_cast<int>(n);
    }
};

}  // namespace

Check HttpClient::get(const char* url, const std::function<Check(tesserkit::HttpResponse&)>& fn) {
    requests.push_back(url);
    if (unreachable) return Check::fail(Status::NotFound, "server unreachable");
    static const std::string notFound = "{\"message\":\"Not Found\"}";
    Response r;
    auto it = files.find(url);
    r.body = it != files.end() ? &it->second : &notFound;
    r.code = it != files.end() ? 200 : 404;
    auto st = statuses.find(url);
    if (st != statuses.end()) r.code = st->second;
    r.announce = announceLength;
    r.cutAt = cutAt;
    return fn(r);
}

int Coredump::read(uint32_t offset, uint8_t* buf, size_t len) {
    if (offset >= data.size()) return 0;
    size_t n = data.size() - offset < len ? data.size() - offset : len;
    memcpy(buf, data.data() + offset, n);
    return static_cast<int>(n);
}

std::string image(uint16_t chipId, size_t size) {
    std::string img(size < 36 ? 36 : size, '\0');
    img[0] = static_cast<char>(0xE9);
    img[12] = static_cast<char>(chipId & 0xFF);
    img[13] = static_cast<char>(chipId >> 8);
    const unsigned char magic[4] = {0x32, 0x54, 0xCD, 0xAB};
    memcpy(&img[32], magic, 4);
    for (size_t i = 36; i < img.size(); i++) img[i] = static_cast<char>(i * 7);
    return img;
}

}  // namespace fake

namespace fake {

namespace {

class UdpSocket : public tesserkit::UdpSocket {
public:
    UdpSocket(Udp& udp, uint16_t port) : udp_(udp), port_(port) { udp_.open_[port] = true; }
    ~UdpSocket() override {
        if (udp_.onClose) udp_.onClose(port_);
        udp_.open_[port_] = false;
        udp_.listeners.erase(port_);
    }
    uint16_t port() const { return port_; }
    size_t receive(uint8_t* buf, size_t cap, uint32_t& fromIp, uint16_t& fromPort) override {
        auto& q = udp_.inbox[port_];
        if (q.empty()) return 0;
        Datagram d = q.front();
        q.pop_front();
        size_t n = d.data.size() < cap ? d.data.size() : cap;
        memcpy(buf, d.data.data(), n);
        fromIp = d.ip;
        fromPort = d.port;
        return n;
    }
    bool send(uint32_t ip, uint16_t port, const uint8_t* data, size_t len) override {
        udp_.sent.push_back({ip, port, std::string(reinterpret_cast<const char*>(data), len)});
        return true;
    }
    bool listen(const char*, uint8_t, int8_t, uint32_t, tesserkit::UdpReceiver fn) override {
        if (udp_.listenFails) return false;
        udp_.listeners[port_] = std::move(fn);
        return true;
    }

private:
    Udp& udp_;
    uint16_t port_;
};

}  // namespace

bool UdpGroups::set(tesserkit::UdpSocket& socket, const std::vector<uint32_t>& list) {
    calls++;
    uint16_t port = static_cast<UdpSocket&>(socket).port();  // every socket here is the fake's
    if (list.empty()) {
        groups.erase(port);
    } else {
        groups[port] = list;
    }
    return true;
}

std::unique_ptr<tesserkit::UdpSocket> Udp::open(uint16_t port) {
    opens++;
    if (fails || open_[port]) return nullptr;
    return std::unique_ptr<tesserkit::UdpSocket>(new UdpSocket(*this, port));
}

bool UdpApi::begin(tesser::Api& api, uint16_t p, const std::string& n, size_t calls, bool own,
                   std::function<void()> q, uint8_t ifaces) {
    begins++;
    interfaces = ifaces;
    maxCalls = calls;
    ownTask = own;
    queued = std::move(q);
    if (!opens) return false;
    port = p;
    name = n;
    if (!l) {
        l.reset(new tesser::UdpLink(
            &api,
            [this](const tesser::PeerAddress& to, const uint8_t* d, size_t len) {
                out.push_back(Datagram{to, std::vector<uint8_t>(d, d + len)});
                return true;
            },
            [this] { return clock ? static_cast<uint32_t>(*clock) : 0; }, tesser::UdpLink::Options(),
            [calls] {
                tesser::DatagramEndpoint::Options o = tesser::UdpLink::endpointOptions();
                o.maxCalls = calls;
                return o;
            }()));
    }
    l->setName(n);
    const uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0xf4, 0xa6, 0x7c};
    l->setDeviceId(mac);
    l->start(nonce++);
    return true;
}

void Mqtt::stop() {
    if (!running) return;
    running = false;
    linked = false;
    stops++;
    // The will, as the ESP client publishes it before disconnecting.
    publish(config.willTopic, config.willPayload, true);
    if (lingering) {
        ending.push_back(std::move(memory));
    } else {
        memory.release();
    }
}

bool Mqtt::publish(std::string topic, std::string payload, bool retain) {
    if (full) return false;
    Message m{topic, payload, retain};
    published.push_back(m);
    if (retain) retained[topic] = m;
    return true;
}

void Mqtt::connect() {
    linked = true;
    tesserkit::Event e;
    e.type = tesserkit::Event::Type::MqttConnected;
    e.value = session;
    emit(chaos, sink, std::move(e));
}

void Mqtt::drop(const char* why) {
    linked = false;
    tesserkit::Event e;
    e.type = tesserkit::Event::Type::MqttDisconnected;
    e.value = session;
    e.text = why;
    emit(chaos, sink, std::move(e));
}

void Mqtt::message(const std::string& topic, const std::string& payload) {
    tesserkit::Event e;
    e.type = tesserkit::Event::Type::MqttMessage;
    e.value = session;
    e.text = topic;
    e.data = payload;
    emit(chaos, sink, std::move(e));
}

std::string Mqtt::last(const std::string& topic) const {
    for (auto it = published.rbegin(); it != published.rend(); ++it) {
        if (it->topic == topic) return it->payload;
    }
    return "<none>";
}

namespace {

// What `p` holds, in bytes; removes it too if `erase` (not `p` itself if
// `keep`).
uint64_t walk(const std::string& p, bool erase, bool keep) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return 0;
    uint64_t bytes = 0;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = opendir(p.c_str())) {
            std::vector<std::string> names;
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) names.push_back(e->d_name);
            }
            closedir(d);
            for (const std::string& n : names) bytes += walk(p + "/" + n, erase, false);
        }
        if (erase && !keep) rmdir(p.c_str());
    } else {
        bytes = static_cast<uint64_t>(st.st_size);
        if (erase) unlink(p.c_str());
    }
    return bytes;
}

}  // namespace

Filesystem::Filesystem() {
    char tmpl[] = "/tmp/tesserkit-fs-XXXXXX";
    if (mkdtemp(tmpl)) dir = tmpl;
}

Filesystem::~Filesystem() {
    if (!dir.empty()) walk(dir, true, false);
}

bool Filesystem::mount(const char* l, const char* r, std::string& why) {
    label = l ? l : "";
    root = r ? r : "";
    why.clear();
    if (present && unmountable) {
        why = "its LittleFS didn't mount, left as it is";
        return false;
    }
    return present && !dir.empty() && root == dir;
}

bool Filesystem::usage(uint64_t& t, uint64_t& used) {
    t = total;
    used = walk(dir, false, false);
    return true;
}

bool Filesystem::format() {
    formats++;
    walk(dir, true, true);
    return true;
}

}  // namespace fake
