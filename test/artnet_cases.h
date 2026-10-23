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
inline std::string dmx(uint16_t universe, const std::vector<uint8_t>& channels, uint8_t sequence = 1) {
    std::string p("Art-Net\0", 8);
    p += '\x00';
    p += '\x50';  // OpDmx, low byte first
    p += '\x00';
    p += '\x0e';  // protocol 14
    p += static_cast<char>(sequence);
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

inline std::string artSync() {
    std::string p("Art-Net\0", 8);
    p += '\x00';
    p += '\x52';  // OpSync
    p += '\x00';
    p += '\x0e';
    p += '\x00';
    p += '\x00';
    return p;
}

// An application's sink: what it was handed.
struct Sink : tesserkit::DmxSink {
    struct Frame {
        tesserkit::DmxFrame f;
        std::vector<uint8_t> data;
    };
    std::vector<Frame> frames;
    std::vector<tesserkit::DmxSync> syncs;
    void frame(const tesserkit::DmxFrame& f) override {
        frames.push_back({f, std::vector<uint8_t>(f.data, f.data + f.length)});
        frames.back().f.data = nullptr;
    }
    void sync(const tesserkit::DmxSync& s) override { syncs.push_back(s); }
};

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
    const uint8_t ethMac[6] = {0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5};
    r.dev.address(0xC0A80142, ethMac);  // 192.168.1.66 on Ethernet, as Network sets it
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
    CHECK(memcmp(b + 201, ethMac, 6) == 0);  // the MAC of the interface whose address it gives
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

TEST(artnet_hands_its_universes_to_a_sink) {
    ArtNetRig r;
    Sink sink;
    r.artnet.sink(&sink).map(mapping("/led/brightness", 1, 0, 0, 255));
    CHECK(r.dev.begin());
    r.run(40);
    r.artnet.universes({5, 3, 4, 40000, 3});  // 40000 isn't a port-address
    CHECK(r.artnet.universes() == std::vector<uint16_t>({3, 4, 5}));
    r.fake.udp.clockUs = 123456789;
    // Handed as it arrives, by the receiving task: no period needed.
    r.fake.udp.deliver(6454, dmx(3, {10, 20, 30}, 7), 0xC0A8010A);
    CHECK_EQ(sink.frames.size(), size_t(1));
    if (!sink.frames.empty()) {
        const auto& f = sink.frames[0];
        CHECK(f.f.protocol == tesserkit::DmxProtocol::ArtNet);
        CHECK_EQ(f.f.universe, 3);
        CHECK_EQ(f.f.sequence, 7);
        CHECK_EQ(f.f.source, 0xC0A8010Au);
        CHECK_EQ(f.f.atUs, uint64_t(123456789));
        CHECK(f.data == std::vector<uint8_t>({10, 20, 30, 0}));  // ArtDmx lengths are even
    }
    r.fake.udp.deliver(6454, dmx(6, {1}));  // not asked for: only counted
    CHECK_EQ(sink.frames.size(), size_t(1));
    // The mappings' universe still goes to the tree, not to the sink.
    r.fake.udp.deliver(6454, dmx(0, {77}));
    r.run(40);
    CHECK_EQ(r.brightness, 77);
    CHECK_EQ(sink.frames.size(), size_t(1));
    CHECK_EQ(r.value("/sys/artnet/status/packets"), "3");  // counted by the run above
    r.fake.udp.deliver(6454, artSync(), 0xC0A8010B);
    CHECK_EQ(sink.syncs.size(), size_t(1));
    if (!sink.syncs.empty()) CHECK_EQ(sink.syncs[0].source, 0xC0A8010Bu);
    // Fewer universes: the others stop.
    r.artnet.universes({4});
    r.fake.udp.deliver(6454, dmx(3, {1}));
    r.fake.udp.deliver(6454, dmx(4, {1}));
    CHECK_EQ(sink.frames.size(), size_t(2));
}

TEST(artnet_sink_without_its_task_is_fed_every_period) {
    ArtNetRig r;
    Sink sink;
    r.fake.udp.listenFails = true;
    r.artnet.sink(&sink);
    r.artnet.universes({1});
    CHECK(r.dev.begin());
    r.run(40);
    r.fake.udp.deliver(6454, dmx(1, {5}));
    CHECK(sink.frames.empty());
    r.run(20);
    CHECK_EQ(sink.frames.size(), size_t(1));
}

TEST(artnet_counts_sequence_errors) {
    ArtNetRig r;
    Sink sink;
    r.artnet.sink(&sink);
    r.artnet.universes({1, 2});
    CHECK(r.dev.begin());
    r.run(40);
    for (uint8_t s : {254, 255, 1, 2}) r.fake.udp.deliver(6454, dmx(1, {0}, s));  // Art-Net skips 0
    r.run(20);  // the status shows the counters as of the last period
    CHECK_EQ(r.value("/sys/artnet/status/sequenceErrors"), "0");
    r.fake.udp.deliver(6454, dmx(1, {0}, 4));  // 3 is missing
    r.fake.udp.deliver(6454, dmx(2, {0}, 9));  // each universe counts its own
    r.fake.udp.deliver(6454, dmx(2, {0}, 10));
    r.fake.udp.deliver(6454, dmx(2, {0}, 0));  // 0: not numbered
    r.run(20);
    CHECK_EQ(r.value("/sys/artnet/status/sequenceErrors"), "1");
    CHECK_EQ(sink.frames.size(), size_t(8));  // delivered all the same
}

TEST(artnet_polls_list_the_sinks_universes) {
    ArtNetRig r;
    Sink sink;
    r.artnet.sink(&sink);
    CHECK(r.dev.begin());
    r.run(40);
    r.artnet.universes({1, 2, 3, 4, 5, 0x120});
    r.fake.udp.deliver(6454, dmx(2, {1}));
    r.fake.udp.deliver(6454, poll());
    r.run(20);
    // Four ports a reply, sharing net and sub-net; the mappings' universe
    // (0, no mappings) isn't one.
    CHECK_EQ(r.fake.udp.sent.size(), size_t(3));
    if (r.fake.udp.sent.size() != 3) return;
    auto b = [&](size_t i) { return reinterpret_cast<const uint8_t*>(r.fake.udp.sent[i].data.data()); };
    CHECK(b(0)[173] == 4 && b(0)[211] == 1);
    CHECK(b(0)[190] == 1 && b(0)[191] == 2 && b(0)[192] == 3 && b(0)[193] == 4);
    CHECK(b(0)[182] == 0 && b(0)[183] == 0x80);  // universe 2 has data
    CHECK(b(1)[173] == 1 && b(1)[211] == 2 && b(1)[190] == 5 && b(1)[174] == 0x80 && b(1)[175] == 0);
    CHECK(b(2)[173] == 1 && b(2)[211] == 3);
    CHECK(b(2)[18] == 0x01 && b(2)[19] == 0x02 && b(2)[190] == 0);  // net 1, sub-net 2, universe 0
    CHECK(b(2)[207] == b(0)[207] && b(2)[200] == 0);  // the root's address in every bind
}

}  // namespace artnet_cases
