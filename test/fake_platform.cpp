#include "fake_platform.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

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

Check HttpClient::get(const char* url, const std::function<Check(int64_t)>& length,
                      const std::function<Check(const uint8_t*, size_t)>& data) {
    auto it = files.find(url);
    if (it == files.end()) return Check::fail(Status::NotFound, "HTTP 404");
    Check c = length(announceLength ? static_cast<int64_t>(it->second.size()) : -1);
    if (!c.isOk()) return c;
    const std::string& body = it->second;
    for (size_t off = 0; off < body.size(); off += 1000) {
        size_t n = body.size() - off < 1000 ? body.size() - off : 1000;
        c = data(reinterpret_cast<const uint8_t*>(body.data() + off), n);
        if (!c.isOk()) return c;
    }
    return Check::ok();
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

}  // namespace fake
