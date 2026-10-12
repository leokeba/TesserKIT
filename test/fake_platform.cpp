#include "fake_platform.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace fake {

using tesser::Check;
using tesser::Status;

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
    std::vector<tesserkit::ScanResult> out;
    for (const Air& a : air) {
        if (a.hidden) continue;
        tesserkit::ScanResult r;
        r.ssid = a.ssid;
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
    for (const Air& a : air) {
        if (a.ssid != c.ssid) continue;
        if (a.password != c.password) {
            post(Event::Type::StaDisconnected, 15);  // 4-way handshake timeout
            return true;
        }
        linked = true;
        link = tesserkit::StaInfo();
        link.ssid = a.ssid;
        link.ip = c.staticIp ? c.ip : 0xC0A80132;  // 192.168.1.50
        link.rssi = a.rssi;
        link.channel = a.channel;
        for (int i = 0; i < 6; i++) link.bssid[i] = static_cast<uint8_t>(0x10 + i);
        post(Event::Type::StaGotIp);
        return true;
    }
    post(Event::Type::StaDisconnected, 201);  // no AP found
    return true;
}

void Wifi::disconnect() {
    if (!linked) return;
    linked = false;
    post(Event::Type::StaDisconnected, Event::kReasonLeft);
}

void Wifi::drop() {
    linked = false;
    post(Event::Type::StaDisconnected, 200);  // beacon timeout
}

// The lease, or the static address, once the link is up.
static void lease(Ethernet& e) {
    if (!e.state.link) return;
    if (e.ip.staticIp) {
        e.state.ip = e.ip.ip;
    } else if (e.dhcp) {
        e.state.ip = 0xC0A8013C;  // 192.168.1.60
    } else {
        if (e.autoip) e.state.ip = 0xA9FE0A0B;  // 169.254.10.11, without an event
        return;
    }
    e.post(Event::Type::EthGotIp);
}

void Ethernet::enabled(bool o) {
    on = o;
    if (!o && state.link) {
        state.link = false;
        state.ip = 0;
        state.mbps = 0;
    }
}

void Ethernet::address(const tesserkit::IpConfig& c) {
    addressCalls++;
    ip = c;
    state.ip = 0;
    lease(*this);
}

void Ethernet::plug() {
    if (!on) return;
    state.link = true;
    state.mbps = 100;
    state.fullDuplex = true;
    post(Event::Type::EthLink, 1);
    lease(*this);
}

void Ethernet::unplug() {
    state.link = false;
    state.ip = 0;
    state.mbps = 0;
    post(Event::Type::EthLink, 0);
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
    written.append(reinterpret_cast<const char*>(data), len);
    return Check::ok();
}

Check Ota::finish() {
    if (!open_) return Check::fail(Status::Internal, "not open");
    open_ = false;
    if (verifyFails) return Check::fail(Status::InvalidValue, "image verification failed");
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
    ~UdpSocket() override { udp_.open_[port_] = false; }
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

private:
    Udp& udp_;
    uint16_t port_;
};

}  // namespace

std::unique_ptr<tesserkit::UdpSocket> Udp::open(uint16_t port) {
    opens++;
    if (fails || open_[port]) return nullptr;
    return std::unique_ptr<tesserkit::UdpSocket>(new UdpSocket(*this, port));
}

void Mqtt::stop() {
    if (!running) return;
    running = false;
    stops++;
    // The will, as the ESP client publishes it before disconnecting.
    publish(config.willTopic, config.willPayload, true);
}

bool Mqtt::publish(const std::string& topic, const std::string& payload, bool retain) {
    if (full) return false;
    Message m{topic, payload, retain};
    published.push_back(m);
    if (retain) retained[topic] = m;
    return true;
}

void Mqtt::connect() {
    tesserkit::Event e;
    e.type = tesserkit::Event::Type::MqttConnected;
    e.value = session;
    sink(e);
}

void Mqtt::drop(const char* why) {
    tesserkit::Event e;
    e.type = tesserkit::Event::Type::MqttDisconnected;
    e.value = session;
    e.text = why;
    sink(e);
}

void Mqtt::message(const std::string& topic, const std::string& payload) {
    tesserkit::Event e;
    e.type = tesserkit::Event::Type::MqttMessage;
    e.value = session;
    e.text = topic;
    e.data = payload;
    sink(e);
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

bool Filesystem::mount(const char* l, const char* r) {
    label = l ? l : "";
    root = r ? r : "";
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
