#include "tesserkit/artnet.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace tesserkit {

namespace {

constexpr uint8_t kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
constexpr uint16_t kOpPoll = 0x2000;
constexpr uint16_t kOpPollReply = 0x2100;
constexpr uint16_t kOpDmx = 0x5000;
constexpr uint16_t kOpSync = 0x5200;
constexpr size_t kDmxHeader = 18;
constexpr size_t kPollReplySize = 239;
constexpr size_t kMaxPolls = 4;           // controllers answered a period
constexpr uint32_t kGoodOutputMs = 3000;  // a frame this recent: "data being output"

void copyName(uint8_t* dst, const char* src, size_t cap) {
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
}

}  // namespace

void ArtNet::describe(tesser::Object& o) {
    o.label("Art-Net")
        .ui("icon", "bulb")
        .doc("Lighting consoles drive nodes of the tree through the DMX channels of one universe");
    describeDmx(o, "The port-address: net, sub-net and universe as one 15-bit number");
}

void ArtNet::received(const uint8_t* buf, size_t n, uint32_t ip, uint64_t atUs) {
    if (n < 12 || memcmp(buf, kId, sizeof(kId)) != 0) return;
    uint16_t op = static_cast<uint16_t>(buf[8] | buf[9] << 8);
    if (op == kOpPoll) {
        tesser::MutexGuard g(lock_);
        if (polls_.size() < kMaxPolls && std::find(polls_.begin(), polls_.end(), ip) == polls_.end()) {
            polls_.push_back(ip);
        }
        return;
    }
    if (op == kOpSync) {
        DmxSync s;
        s.protocol = DmxProtocol::ArtNet;
        s.source = ip;
        s.atUs = atUs;
        sync(s);
        return;
    }
    if (op != kOpDmx || n < kDmxHeader) return;
    DmxFrame f;
    f.protocol = DmxProtocol::ArtNet;
    f.universe = static_cast<uint16_t>(buf[14] | (buf[15] & 0x7F) << 8);
    f.sequence = buf[12];
    size_t len = static_cast<size_t>(buf[16] << 8 | buf[17]);
    if (len > n - kDmxHeader) len = n - kDmxHeader;
    if (len > 512) len = 512;
    f.data = buf + kDmxHeader;
    f.length = static_cast<uint16_t>(len);
    f.source = ip;
    f.atUs = atUs;
    counted(f.universe);
    bool toSink;
    {
        tesser::MutexGuard g(lock_);
        Track* t = track(f.universe);
        if (!t) return;
        if (f.sequence) {  // 0: the sender doesn't number its frames
            if (t->sequenced) sequenced(t->sequence, f.sequence);
            t->sequenced = true;
            t->sequence = f.sequence;
        }
        toSink = take(t, f);
    }
    if (toSink) sink_->frame(f);
}

void ArtNet::ticked(uint32_t nowMs) {
    (void)nowMs;
    std::vector<uint32_t> polls;
    {
        tesser::MutexGuard g(lock_);
        if (polls_.empty()) return;
        polls.swap(polls_);
    }
    for (uint32_t ip : polls) pollReply(ip);
}

void ArtNet::pollReply(uint32_t ip) {
    if (!ip) return;
    // The output ports: the sink's universes, and the mappings' unless only
    // the sink's are used.
    struct Port {
        uint16_t universe;
        bool good;
    };
    std::vector<Port> ports;
    {
        tesser::MutexGuard g(lock_);
        bool mappingsToo = !mappings_.empty() || listened_.empty();
        for (const Track& t : tracks_) {
            if (!t.sink && !(t.mapped && mappingsToo)) continue;
            // The receiving task's clock can be ahead of the period's.
            bool good = t.everFrame && static_cast<int32_t>(nowMs_ - t.frameMs) < static_cast<int32_t>(kGoodOutputMs);
            ports.push_back({t.universe, good});
        }
    }
    uint32_t me = dev_.address();
    uint8_t r[kPollReplySize] = {};
    memcpy(r, kId, sizeof(kId));
    r[8] = kOpPollReply & 0xFF;
    r[9] = kOpPollReply >> 8;
    for (int i = 0; i < 4; i++) r[10 + i] = r[207 + i] = static_cast<uint8_t>(me >> (24 - 8 * i));  // IP, BindIp
    r[14] = kPort & 0xFF;  // low byte first
    r[15] = kPort >> 8;
    unsigned major = 0, minor = 0;
    sscanf(dev_.version().c_str(), "%u.%u", &major, &minor);
    r[16] = static_cast<uint8_t>(major);
    r[17] = static_cast<uint8_t>(minor);
    r[23] = 0xD0;  // Status1: indicators normal, port-address set locally
    copyName(r + 26, dev_.name().c_str(), 18);
    copyName(r + 44, dev_.hostname().c_str(), 64);
    char report[64];
    snprintf(report, sizeof(report), "#0001 [%04u] %s", static_cast<unsigned>(packets_ % 10000),
             error_.empty() ? "OK" : error_.c_str());
    copyName(r + 108, report, 64);
    r[200] = 0x00;  // Style: a node
    memcpy(r + 201, dev_.addressMac(), 6);  // the interface's, whose address the reply gives
    r[212] = 0x08;  // Status2: 15-bit port-addresses
    // Art-Net 4: a reply per four ports sharing a net and sub-net, told
    // apart by their bind index (1, the root, then 2, 3...).
    size_t i = 0;
    for (uint8_t bind = 1; i < ports.size() && bind != 0; bind++) {
        uint16_t high = ports[i].universe >> 4;
        memset(r + 172, 0, 200 - 172);  // NumPorts to the spare bytes
        r[18] = static_cast<uint8_t>(high >> 4 & 0x7F);  // NetSwitch
        r[19] = static_cast<uint8_t>(high & 0x0F);       // SubSwitch
        uint8_t n = 0;
        for (; n < 4 && i < ports.size() && ports[i].universe >> 4 == high; n++, i++) {
            r[174 + n] = 0x80;                                          // PortTypes: outputs DMX512 from Art-Net
            r[182 + n] = ports[i].good ? 0x80 : 0;                      // GoodOutputA: data being output
            r[190 + n] = static_cast<uint8_t>(ports[i].universe & 0x0F);  // SwOut
        }
        r[173] = n;  // NumPorts
        r[211] = bind;
        // Unicast to the controller, on the Art-Net port (Art-Net 4).
        socket_->send(ip, kPort, r, sizeof(r));
    }
}

}  // namespace tesserkit
