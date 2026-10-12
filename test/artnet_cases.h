// ArtNet (docs/DESIGN.md section 5.9).
#pragma once

#include <string.h>

#include <vector>

#include "rig.h"

namespace artnet_cases {

using rig::at;
using rig::Op;
using rig::Status;

// An ArtDmx packet for `universe` (a 15-bit port-address).
inline std::string dmx(uint16_t universe, const std::vector<uint8_t>& channels) {
    std::string p("Art-Net\0", 8);
    p += '\x00';
    p += '\x50';  // OpDmx, low byte first
    p += '\x00';
    p += '\x0e';  // protocol 14
    p += '\x01';  // sequence
    p += '\x00';  // physical
    p += static_cast<char>(universe & 0xFF);
    p += static_cast<char>(universe >> 8);
    size_t len = channels.size() + (channels.size() & 1);
    p += static_cast<char>(len >> 8);
    p += static_cast<char>(len & 0xFF);
    for (uint8_t c : channels) p += static_cast<char>(c);
    if (channels.size() & 1) p += '\0';
    return p;
}

inline std::string poll() {
    std::string p("Art-Net\0", 8);
    p += '\x00';
    p += '\x20';  // OpPoll
    p += '\x00';
    p += '\x0e';
    p += '\x00';
    p += '\x00';
    return p;
}

inline tesserkit::ArtNetMapping mapping(const char* path, uint16_t coarse, uint16_t fine, double min, double max,
                                 uint8_t threshold = 0) {
    tesserkit::ArtNetMapping m;
    m.path = path;
    m.coarse = coarse;
    m.fine = fine;
    m.min = min;
    m.max = max;
    m.threshold = threshold;
    return m;
}

struct ArtNetRig : rig::Rig {
    tesserkit::ArtNet artnet{dev};
    double azimuth = 0;
    int brightness = 0;
    bool on = false;
    int writes = 0;  // to azimuth
    ArtNetRig() {
        auto& led = dev.api().object("led");
        led.value("azimuth", [this] { return azimuth; },
                  [this](double v) {
                      azimuth = v;
                      writes++;
                  });
        led.value("brightness", brightness).range(0, 255);
        led.value("on", on);
    }
    void send(const std::string& packet) {
        fake.udp.deliver(tesserkit::ArtNet::kPort, packet);
        run(60);
    }
};

TEST(artnet_maps_channels_to_nodes) {
    ArtNetRig r;
    r.artnet.map(mapping("/led/azimuth", 1, 2, 0, 360))
        .map(mapping("/led/brightness", 3, 0, 0, 255))
        .map(mapping("/led/on", 4, 0, 0, 0, 128));
    CHECK(r.dev.begin());
    r.run(40);
    CHECK(r.fake.udp.open_[6454]);
    CHECK_EQ(r.value("/sys/artnet/status/received"), "false");
    CHECK_EQ(r.value("/sys/artnet/status/age"), "-1");
    CHECK_EQ(at(r.schema("/sys/artnet/status").body, "children/age/ui/visibleWhen"), "\"received=true\"");
    r.send(dmx(0, {0x80, 0x00, 255, 200}));
    CHECK(r.azimuth > 180.0 && r.azimuth < 180.1);  // 0x8000 / 65535 of 360
    CHECK_EQ(r.brightness, 255);                     // 8 bits reach the maximum
    CHECK(r.on);
    r.send(dmx(0, {0xFF, 0xFF, 1, 127}));
    CHECK(r.azimuth == 360.0);
    CHECK_EQ(r.brightness, 1);  // rounded for an integer
    CHECK(!r.on);               // below the gate's threshold
    CHECK_EQ(r.value("/sys/artnet/status/packets"), "2");
    CHECK_EQ(r.value("/sys/artnet/status/age"), "0");
    CHECK_EQ(r.value("/sys/artnet/status/received"), "true");
}

TEST(artnet_writes_only_changes) {
    ArtNetRig r;
    r.artnet.map(mapping("/led/azimuth", 1, 0, 0, 100));
    CHECK(r.dev.begin());
    r.send(dmx(0, {10, 99}));
    r.send(dmx(0, {10, 42}));  // another channel moved
    CHECK_EQ(r.writes, 1);
    r.send(dmx(0, {11}));
    CHECK_EQ(r.writes, 2);
    // New mappings write everything again.
    CHECK_EQ(r.set("/sys/artnet/mappings", "[{\"path\":\"/led/azimuth\",\"coarse\":1,\"max\":10}]").status, Status::Ok);
    r.send(dmx(0, {11}));
    CHECK_EQ(r.writes, 3);
}

TEST(artnet_listens_to_its_universe) {
    ArtNetRig r;
    r.artnet.universe(0x123).map(mapping("/led/brightness", 1, 0, 0, 255));
    CHECK(r.dev.begin());
    r.send(dmx(0x124, {50}));
    CHECK_EQ(r.brightness, 0);
    CHECK_EQ(r.value("/sys/artnet/status/universe"), "292");
    r.send(dmx(0x123, {50}));
    CHECK_EQ(r.brightness, 50);
    r.send("Art-Net");       // too short
    r.send("Not-Art\0xxxxxxxxxxxxxxxxxx");  // another protocol
    CHECK_EQ(r.value("/sys/artnet/status/packets"), "2");
}

TEST(artnet_short_frames_leave_channels_alone) {
    ArtNetRig r;
    r.artnet.map(mapping("/led/brightness", 3, 0, 0, 255)).map(mapping("/led/azimuth", 1, 5, 0, 1));
    CHECK(r.dev.begin());
    r.send(dmx(0, {1, 2}));
    CHECK_EQ(r.brightness, 0);
    CHECK_EQ(r.writes, 0);  // its fine channel is beyond the frame
}

TEST(artnet_answers_polls) {
    ArtNetRig r;
    r.dev.version("2.5.1");
    r.artnet.universe(0x1234 & 0x7FFF);
    CHECK(r.dev.begin());
    r.dev.address(0xC0A80142);  // 192.168.1.66, as Network sets it
    r.send(poll());
    CHECK_EQ(r.fake.udp.sent.size(), size_t(1));
    const fake::Datagram& d = r.fake.udp.sent[0];
    CHECK_EQ(d.ip, 0xC0A80164u);  // back to the controller
    CHECK_EQ(d.port, 6454);
    CHECK_EQ(d.data.size(), size_t(239));
    const uint8_t* b = reinterpret_cast<const uint8_t*>(d.data.data());
    CHECK(memcmp(b, "Art-Net\0", 8) == 0);
    CHECK(b[8] == 0x00 && b[9] == 0x21);                    // OpPollReply
    CHECK(b[10] == 192 && b[11] == 168 && b[12] == 1 && b[13] == 66);
    CHECK(b[14] == 0x36 && b[15] == 0x19);                  // port 6454, low byte first
    CHECK(b[16] == 2 && b[17] == 5);                        // version
    CHECK(b[18] == 0x12 && b[19] == 0x03 && b[190] == 0x04);  // net, sub-net, universe
    CHECK_EQ(std::string(reinterpret_cast<const char*>(b + 26)), "Test Device");
    CHECK_EQ(std::string(reinterpret_cast<const char*>(b + 44)), r.dev.hostname());
    CHECK(b[173] == 1 && b[174] == 0x80);
    CHECK(b[212] & 0x08);  // 15-bit port-addresses
}

TEST(artnet_can_be_turned_off) {
    ArtNetRig r;
    r.artnet.map(mapping("/led/brightness", 1, 0, 0, 255));
    CHECK(r.dev.begin());
    r.run(40);
    CHECK_EQ(r.set("/sys/artnet/enabled", "false").status, Status::Ok);
    r.run(40);
    CHECK(!r.fake.udp.open_[6454]);
    CHECK_EQ(r.set("/sys/artnet/enabled", "true").status, Status::Ok);
    r.run(40);
    r.send(dmx(0, {9}));
    CHECK_EQ(r.brightness, 9);
}

TEST(artnet_retries_its_port) {
    ArtNetRig r;
    r.fake.udp.fails = true;
    CHECK(r.dev.begin());
    r.run(100);
    CHECK_EQ(r.fake.udp.opens, 1);
    CHECK_EQ(r.value("/sys/artnet/status/error"), "\"can't open UDP port 6454\"");
    r.fake.udp.fails = false;
    r.run(5000);
    CHECK(r.fake.udp.open_[6454]);
}

TEST(artnet_keeps_write_errors) {
    ArtNetRig r;
    r.artnet.map(mapping("/led/brightness", 1, 0, 0, 1000));  // beyond the node's range
    CHECK(r.dev.begin());
    r.send(dmx(0, {255}));
    CHECK_EQ(r.value("/sys/artnet/status/error"), "\"/led/brightness: invalid_value: out of range\"");
    r.send(dmx(0, {10}));  // 39: fits
    CHECK_EQ(r.brightness, 39);
    CHECK_EQ(r.value("/sys/artnet/status/error"), "\"\"");
}

TEST(artnet_mappings_are_checked) {
    ArtNetRig r;
    CHECK(r.dev.begin());
    CHECK_EQ(r.set("/sys/artnet/mappings", "[{\"path\":\"/led/nope\"}]").status, Status::NotFound);
    CHECK_EQ(r.set("/sys/artnet/mappings", "[{\"coarse\":513}]").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/artnet/mappings", "[{\"threshold\":256}]").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/artnet/universe", "32768").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/artnet/mappings", "[{},{},{},{},{},{},{},{},{}]").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/artnet/mappings", "[{\"path\":\"/led/on\",\"coarse\":7,\"threshold\":1}]").status, Status::Ok);
    CHECK_EQ(r.request(r.anon, Op::Set, "/sys/artnet/enabled", "false").status, Status::Unauthorized);
}

TEST(artnet_mapped_values) {
    using tesser::ValueKind;
    uint8_t f[4] = {0, 255, 128, 0};
    CHECK_EQ(tesserkit::ArtNet::mapped(mapping("", 1, 0, -1, 1), f, 4, ValueKind::Number), "-1");
    CHECK_EQ(tesserkit::ArtNet::mapped(mapping("", 2, 0, -1, 1), f, 4, ValueKind::Number), "1");
    CHECK_EQ(tesserkit::ArtNet::mapped(mapping("", 2, 0, 10, 0), f, 4, ValueKind::Integer), "0");  // reversed
    CHECK_EQ(tesserkit::ArtNet::mapped(mapping("", 3, 0, 0, 1), f, 4, ValueKind::Boolean), "true");
    CHECK_EQ(tesserkit::ArtNet::mapped(mapping("", 3, 4, 0, 65535), f, 4, ValueKind::Integer), "32768");
    CHECK_EQ(tesserkit::ArtNet::mapped(mapping("", 0, 0, 0, 1), f, 4, ValueKind::Number), "");
    CHECK_EQ(tesserkit::ArtNet::mapped(mapping("", 5, 0, 0, 1), f, 4, ValueKind::Number), "");
}

TEST(artnet_refused_writes_are_events) {
    ArtNetRig r;
    r.artnet.map(mapping("/led/brightness", 1, 0, 0, 1000));
    CHECK(r.dev.begin());
    rig::Events ev(r, "/sys/artnet/refused");
    CHECK_EQ(at(r.schema("/sys/artnet/refused").body, "ui/toast"), "\"warn\"");
    r.send(dmx(0, {255}));
    std::vector<std::string> got = ev.take();
    CHECK_EQ(got.size(), size_t(1));
    if (!got.empty()) CHECK_EQ(got[0], "\"/led/brightness: invalid_value: out of range\"");
    // The same error again isn't news; after a write that worked, it is.
    r.send(dmx(0, {200}));
    CHECK(ev.take().empty());
    r.send(dmx(0, {10}));
    r.send(dmx(0, {255}));
    CHECK_EQ(ev.take().size(), size_t(1));
}

}  // namespace artnet_cases
