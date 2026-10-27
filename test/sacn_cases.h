// Sacn (docs/DESIGN.md section 5.22).
#pragma once

#include <string.h>

#include <vector>

#include "artnet_cases.h"
#include "rig.h"

namespace sacn_cases {

using artnet_cases::mapping;
using artnet_cases::Sink;
using rig::Status;

struct Data {
    uint16_t universe = 1;
    std::vector<uint8_t> channels;
    uint8_t sequence = 0;
    uint8_t priority = 100;
    uint8_t cid = 1;  // the source: every byte of its CID
    uint8_t options = 0;
    uint16_t sync = 0;
    uint8_t startCode = 0;
};

// An E1.31 data packet.
inline std::string data(const Data& d) {
    std::string p(126 + d.channels.size(), '\0');
    uint8_t* b = reinterpret_cast<uint8_t*>(&p[0]);
    auto length = [&](size_t at) {
        size_t n = p.size() - at;
        b[at] = static_cast<uint8_t>(0x70 | n >> 8);
        b[at + 1] = static_cast<uint8_t>(n);
    };
    b[1] = 0x10;  // preamble size
    memcpy(b + 4, "ASC-E1.17\0\0\0", 12);
    length(16);
    b[21] = 0x04;  // VECTOR_ROOT_E131_DATA
    memset(b + 22, d.cid, 16);
    length(38);
    b[43] = 0x02;  // VECTOR_E131_DATA_PACKET
    memcpy(b + 44, "console", 7);
    b[108] = d.priority;
    b[109] = static_cast<uint8_t>(d.sync >> 8);
    b[110] = static_cast<uint8_t>(d.sync);
    b[111] = d.sequence;
    b[112] = d.options;
    b[113] = static_cast<uint8_t>(d.universe >> 8);
    b[114] = static_cast<uint8_t>(d.universe);
    length(115);
    b[117] = 0x02;  // VECTOR_DMP_SET_PROPERTY
    b[118] = 0xA1;
    b[122] = 1;  // address increment
    size_t count = d.channels.size() + 1;
    b[123] = static_cast<uint8_t>(count >> 8);
    b[124] = static_cast<uint8_t>(count);
    b[125] = d.startCode;
    memcpy(b + 126, d.channels.data(), d.channels.size());
    return p;
}

// An E1.31 universe synchronization packet.
inline std::string syncPacket(uint16_t address) {
    std::string p(49, '\0');
    uint8_t* b = reinterpret_cast<uint8_t*>(&p[0]);
    b[1] = 0x10;
    memcpy(b + 4, "ASC-E1.17\0\0\0", 12);
    b[16] = 0x70;
    b[17] = 49 - 16;
    b[21] = 0x08;  // VECTOR_ROOT_E131_EXTENDED
    memset(b + 22, 1, 16);
    b[38] = 0x70;
    b[39] = 49 - 38;
    b[43] = 0x01;  // VECTOR_E131_EXTENDED_SYNCHRONIZATION
    b[45] = static_cast<uint8_t>(address >> 8);
    b[46] = static_cast<uint8_t>(address);
    return p;
}

struct SacnRig : rig::Rig {
    tesserkit::Sacn sacn{dev};
    int brightness = 0;
    Sink sink;
    SacnRig() { dev.api().object("led").value("brightness", brightness).range(0, 255); }
    void deliver(const Data& d, uint32_t ip = 0xC0A80164) { fake.udp.deliver(5568, data(d), ip); }
    // The levels of the frames handed to the sink.
    std::vector<uint8_t> firsts() const {
        std::vector<uint8_t> v;
        for (const auto& f : sink.frames) v.push_back(f.data.empty() ? 0 : f.data[0]);
        return v;
    }
};

TEST(sacn_maps_channels_to_nodes) {
    SacnRig r;
    r.sacn.map(mapping("/led/brightness", 2, 0, 0, 255));
    CHECK(r.dev.begin());
    r.run(40);
    CHECK(r.fake.udp.open_[5568]);
    Data d;
    d.channels = {0, 90};
    r.deliver(d);
    r.run(20);
    CHECK_EQ(r.brightness, 90);
    CHECK_EQ(r.value("/sys/sacn/status/packets"), "1");
    CHECK_EQ(r.value("/sys/sacn/status/received"), "true");
    d.universe = 2;
    d.channels = {0, 10};
    r.deliver(d);
    r.run(20);
    CHECK_EQ(r.brightness, 90);  // another universe
    CHECK_EQ(r.value("/sys/sacn/status/universe"), "2");
    CHECK_EQ(r.set("/sys/sacn/universe", "0").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/sacn/universe", "64000").status, Status::InvalidValue);
    CHECK_EQ(r.set("/sys/sacn/universe", "2").status, Status::Ok);
    d.sequence = 1;
    r.deliver(d);
    r.run(20);
    CHECK_EQ(r.brightness, 10);
}

TEST(sacn_joins_its_universes_groups) {
    SacnRig r;
    r.sacn.sink(&r.sink);
    CHECK(r.dev.begin());
    r.run(40);
    using tesserkit::Sacn;
    CHECK(r.fake.udpGroups.groups[5568] == std::vector<uint32_t>({Sacn::group(1)}));
    CHECK_EQ(Sacn::group(0x1234), 0xEFFF1234u);  // 239.255.18.52
    r.sacn.universes({300, 2});
    r.run(20);
    CHECK(r.fake.udpGroups.groups[5568] == std::vector<uint32_t>({Sacn::group(1), Sacn::group(2), Sacn::group(300)}));
    // A sync address in a frame of ours: its group too.
    Data d;
    d.universe = 2;
    d.channels = {1};
    d.sync = 7;
    r.deliver(d);
    r.run(20);
    CHECK_EQ(r.fake.udpGroups.groups[5568].size(), size_t(4));
    CHECK_EQ(r.fake.udpGroups.groups[5568].back(), Sacn::group(7));
    CHECK_EQ(r.sink.frames.size(), size_t(1));
    if (!r.sink.frames.empty()) CHECK_EQ(r.sink.frames[0].f.syncAddress, 7);
    // Asked again: on a new address at once, and every 5 s for interfaces
    // that appeared.
    int calls = r.fake.udpGroups.calls;
    r.run(1000);
    CHECK_EQ(r.fake.udpGroups.calls, calls);
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    r.dev.address(0xC0A80142, mac);
    r.run(20);
    CHECK_EQ(r.fake.udpGroups.calls, calls + 1);
    r.run(5000);
    CHECK_EQ(r.fake.udpGroups.calls, calls + 2);
    // Turned off: the socket and its groups go.
    CHECK_EQ(r.set("/sys/sacn/enabled", "false").status, Status::Ok);
    r.run(20);
    CHECK(!r.fake.udp.open_[5568]);
    CHECK(r.fake.udpGroups.groups.find(5568) == r.fake.udpGroups.groups.end());
}

TEST(sacn_highest_priority_wins) {
    SacnRig r;
    r.sacn.sink(&r.sink);
    r.sacn.universes({5});
    CHECK(r.dev.begin());
    r.run(40);
    Data a, b;
    a.universe = b.universe = 5;
    a.cid = 1;
    b.cid = 2;
    b.priority = 150;
    auto send = [&](Data& d, uint8_t level) {
        d.channels = {level};
        r.deliver(d);
        d.sequence++;
        r.fake.udp.clockUs += 25000;
    };
    send(a, 1);
    send(b, 2);  // higher: takes over
    send(a, 3);
    send(b, 4);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 2, 4}));
    // Its stream terminated: the other is back at once.
    b.options = 0x40;
    send(b, 5);
    send(a, 6);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 2, 4, 6}));
    // Back, then silent for 2.5 s: lost.
    b.options = 0;
    send(b, 7);
    send(a, 8);
    r.fake.udp.clockUs += 2500000;
    send(a, 9);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 2, 4, 6, 7, 9}));
    // A frame's priority is its source's.
    if (r.sink.frames.size() > 2) CHECK_EQ(r.sink.frames[1].f.priority, 150);
}

TEST(sacn_equal_priorities_keep_the_first) {
    SacnRig r;
    r.sacn.sink(&r.sink);
    r.sacn.universes({1});
    CHECK(r.dev.begin());
    r.run(40);
    for (uint8_t cid = 1; cid <= 4; cid++) {  // a fourth source exceeds the table: ignored
        Data d;
        d.cid = cid;
        d.channels = {cid};
        r.deliver(d);
    }
    CHECK(r.firsts() == std::vector<uint8_t>({1}));
}

TEST(sacn_refuses_a_fourth_source_while_three_are_live) {
    SacnRig r;
    r.sacn.sink(&r.sink);
    r.sacn.universes({1});
    CHECK(r.dev.begin());
    r.run(40);
    Data s[4];
    auto send = [&](int i, uint8_t level) {
        s[i].cid = static_cast<uint8_t>(i + 1);
        s[i].channels = {level};
        r.deliver(s[i]);
        s[i].sequence++;
        r.fake.udp.clockUs += 10000;
    };
    send(0, 1);
    send(1, 2);
    send(2, 3);
    s[3].priority = 150;  // higher than all three, but no room for it
    send(3, 4);
    send(0, 5);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 5}));
    // One of the three terminated: the fourth gets its place, and wins.
    s[1].options = 0x40;
    send(1, 6);
    send(3, 7);
    send(0, 8);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 5, 7}));
    if (r.sink.frames.size() == 3) CHECK_EQ(r.sink.frames[2].f.priority, 150);
}

TEST(sacn_drops_a_source_silent_for_the_loss_timeout) {
    SacnRig r;
    r.sacn.sink(&r.sink);
    r.sacn.universes({1});
    CHECK(r.dev.begin());
    r.run(40);
    Data a, b;
    a.cid = 1;
    a.priority = 150;
    b.cid = 2;
    a.sequence = 10;
    a.channels = {1};
    r.deliver(a);
    // 2.5 s exactly: still live, so the lower one stays out.
    r.fake.udp.clockUs += 2500000;
    b.channels = {2};
    r.deliver(b);
    b.sequence++;
    // A millisecond more: lost, checked as this packet arrives.
    r.fake.udp.clockUs += 1000;
    b.channels = {3};
    r.deliver(b);
    b.sequence++;
    CHECK(r.firsts() == std::vector<uint8_t>({1, 3}));
    // The lost source back is a new one: its sequence starts again (5 is
    // behind its last 10, which would be dropped as late), and its
    // priority takes over at once.
    r.fake.udp.clockUs += 10000;
    a.sequence = 5;
    a.channels = {4};
    r.deliver(a);
    b.channels = {5};
    r.deliver(b);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 3, 4}));
    r.run(20);
    CHECK_EQ(r.value("/sys/sacn/status/sequenceErrors"), "0");
    // Every source silent, nothing is checked until a packet arrives: the
    // first back is a new source, taken with whatever sequence it resumes at.
    r.fake.udp.clockUs += 10000000;
    a.sequence = 1;
    a.channels = {6};
    r.deliver(a);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 3, 4, 6}));
}

TEST(sacn_stream_terminated_drops_its_source) {
    SacnRig r;
    r.sacn.sink(&r.sink);
    r.sacn.universes({1});
    CHECK(r.dev.begin());
    r.run(40);
    Data a, b;
    a.cid = 1;
    b.cid = 2;
    a.sequence = 10;
    a.channels = {1};
    r.deliver(a);
    b.channels = {2};
    r.deliver(b);  // equal priority: the first keeps it
    // A loser terminating changes nothing.
    b.sequence = 1;
    b.options = 0x40;
    b.channels = {3};
    r.deliver(b);
    a.sequence = 11;
    a.channels = {4};
    r.deliver(a);
    // The winner terminating: its packet's levels aren't delivered.
    a.sequence = 12;
    a.options = 0x40;
    a.channels = {5};
    r.deliver(a);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 4}));
    // Back, it is a new source: its sequence starts again.
    a.sequence = 3;
    a.options = 0;
    a.channels = {6};
    r.deliver(a);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 4, 6}));
    // A source never heard terminating: nothing.
    Data c;
    c.cid = 3;
    c.options = 0x40;
    c.channels = {7};
    r.deliver(c);
    a.sequence = 4;
    a.channels = {8};
    r.deliver(a);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 4, 6, 8}));
    r.run(20);
    CHECK_EQ(r.value("/sys/sacn/status/sequenceErrors"), "0");
}

TEST(sacn_sequence_numbers_wrap) {
    SacnRig r;
    r.sacn.sink(&r.sink);
    r.sacn.universes({1});
    CHECK(r.dev.begin());
    r.run(40);
    Data d;
    uint8_t level = 0;
    for (int seq : {253, 254, 255, 0, 1}) {  // E1.31 numbers 0 to 255: no gap at the wrap
        d.sequence = static_cast<uint8_t>(seq);
        d.channels = {++level};
        r.deliver(d);
    }
    r.run(20);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 2, 3, 4, 5}));
    CHECK_EQ(r.value("/sys/sacn/status/sequenceErrors"), "0");
    // Late across the wrap: 250 is 7 behind 1, dropped.
    d.sequence = 250;
    d.channels = {6};
    r.deliver(d);
    // 21 behind (236, back across the wrap) is too far to be late: taken,
    // as a jump.
    d.sequence = static_cast<uint8_t>(1 - 21);
    d.channels = {7};
    r.deliver(d);
    // 20 behind is the limit: taken too.
    d.sequence = static_cast<uint8_t>(1 - 21 - 20);
    d.channels = {8};
    r.deliver(d);
    r.run(20);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 2, 3, 4, 5, 7, 8}));
    CHECK_EQ(r.value("/sys/sacn/status/sequenceErrors"), "3");  // the late one, two jumps
}

TEST(sacn_ignores_previews_other_start_codes_and_late_frames) {
    SacnRig r;
    r.sacn.sink(&r.sink);
    r.sacn.universes({1});
    CHECK(r.dev.begin());
    r.run(40);
    Data d;
    d.sequence = 10;
    d.channels = {1};
    r.deliver(d);
    Data p = d;
    p.sequence = 50;
    p.options = 0x80;  // preview: ignored, sequence and all
    p.channels = {2};
    r.deliver(p);
    Data s = d;
    s.sequence = 11;
    s.startCode = 0xDD;  // per-channel priorities: in the source's sequence, not levels
    s.channels = {3};
    r.deliver(s);
    d.sequence = 12;
    d.channels = {4};
    r.deliver(d);
    d.sequence = 9;  // late: dropped (12 - 9 is within 20)
    d.channels = {5};
    r.deliver(d);
    d.sequence = 13;
    d.channels = {6};
    r.deliver(d);
    d.sequence = 200;  // a jump: taken, and counted
    d.channels = {7};
    r.deliver(d);
    CHECK(r.firsts() == std::vector<uint8_t>({1, 4, 6, 7}));
    r.run(20);  // the status shows the counters as of the last period
    CHECK_EQ(r.value("/sys/sacn/status/packets"), "7");
    CHECK_EQ(r.value("/sys/sacn/status/sequenceErrors"), "2");  // the late one, the jump
    r.fake.udp.deliver(5568, std::string(200, 'x'));  // not E1.31
    r.run(20);
    CHECK_EQ(r.value("/sys/sacn/status/packets"), "7");
}

TEST(sacn_sync_packets_reach_the_sink) {
    SacnRig r;
    r.sacn.sink(&r.sink);
    CHECK(r.dev.begin());
    r.run(40);
    r.fake.udp.deliver(5568, syncPacket(7000), 0xC0A80165);
    CHECK_EQ(r.sink.syncs.size(), size_t(1));
    if (!r.sink.syncs.empty()) {
        CHECK(r.sink.syncs[0].protocol == tesserkit::DmxProtocol::Sacn);
        CHECK_EQ(r.sink.syncs[0].address, 7000);
        CHECK_EQ(r.sink.syncs[0].source, 0xC0A80165u);
    }
}

}  // namespace sacn_cases
