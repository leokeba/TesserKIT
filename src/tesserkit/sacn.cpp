#include "tesserkit/sacn.h"

#include <string.h>

#include <algorithm>

namespace tesserkit {

namespace {

// ANSI E1.31-2018: the root layer, then the framing layer, then (data) the
// DMP layer.
constexpr uint8_t kAcnId[12] = {'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', 0, 0, 0};
constexpr uint32_t kRootData = 0x00000004;
constexpr uint32_t kRootExtended = 0x00000008;
constexpr uint32_t kFramingData = 0x00000002;
constexpr uint32_t kFramingSync = 0x00000001;
constexpr size_t kDataHeader = 126;  // up to the property values, after the start code
constexpr size_t kSyncSize = 49;
constexpr uint8_t kPreview = 0x80;
constexpr uint8_t kTerminated = 0x40;
constexpr uint8_t kMaxPriority = 200;
constexpr uint32_t kRejoinMs = 5000;  // groups asked again: interfaces come and go

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}  // namespace

void Sacn::describe(tesser::Object& o) {
    o.label("sACN")
        .ui("icon", "bulb")
        .doc("Lighting consoles drive nodes of the tree through the DMX channels of one universe, over E1.31 "
             "(streaming ACN)");
    describeDmx(o, "1 to 63999");
}

void Sacn::start() {
    Platform& p = dev_.platform();
    if (!p.udpGroups) p.udpGroups = espUdpGroups();
    groups_ = p.udpGroups;
    if (!groups_) tesser::logError("tesserkit: sACN has no multicast");
    DmxService::start();
}

void Sacn::closing() {
    if (groups_ && !joined_.empty()) groups_->set(*socket_, {});
    joined_.clear();
}

void Sacn::opened() {
    joined_.clear();
    joinedAtMs_ = nowMs_ - kRejoinMs;  // at once
    tesser::MutexGuard g(lock_);
    syncAddresses_.clear();
}

void Sacn::ticked(uint32_t nowMs) {
    std::vector<uint32_t> wanted;
    {
        tesser::MutexGuard g(lock_);
        for (const Track& t : tracks_) wanted.push_back(group(t.universe));
        for (uint16_t a : syncAddresses_) {
            if (std::find(wanted.begin(), wanted.end(), group(a)) == wanted.end()) wanted.push_back(group(a));
        }
    }
    uint32_t address = dev_.address();
    if (!groups_ || (wanted == joined_ && address == joinedAddress_ && nowMs - joinedAtMs_ < kRejoinMs)) return;
    joined_ = wanted;
    joinedAddress_ = address;
    joinedAtMs_ = nowMs;
    groups_->set(*socket_, wanted);
}

void Sacn::received(const uint8_t* buf, size_t n, uint32_t ip, uint64_t atUs) {
    if (n < 44 || be16(buf) != 0x0010 || be16(buf + 2) != 0 || memcmp(buf + 4, kAcnId, sizeof(kAcnId)) != 0) return;
    uint32_t root = be32(buf + 18);
    if (root == kRootExtended) {
        if (n < kSyncSize || be32(buf + 40) != kFramingSync) return;  // discovery: (later)
        DmxSync s;
        s.protocol = DmxProtocol::Sacn;
        s.address = be16(buf + 45);
        s.source = ip;
        s.atUs = atUs;
        sync(s);
        return;
    }
    if (root != kRootData || n < kDataHeader || be32(buf + 40) != kFramingData) return;
    if (buf[117] != 0x02 || buf[118] != 0xA1) return;  // DMP: set property, one-byte addresses
    DmxFrame f;
    f.protocol = DmxProtocol::Sacn;
    f.universe = be16(buf + 113);
    if (f.universe < 1 || f.universe > 63999) return;
    f.priority = buf[108] > kMaxPriority ? kMaxPriority : buf[108];
    f.syncAddress = be16(buf + 109);
    f.sequence = buf[111];
    uint8_t options = buf[112];
    size_t count = be16(buf + 123);  // the start code and the channels
    size_t len = count ? count - 1 : 0;
    if (len > n - kDataHeader) len = n - kDataHeader;
    if (len > 512) len = 512;
    f.data = buf + kDataHeader;
    f.length = static_cast<uint16_t>(len);
    f.source = ip;
    f.atUs = atUs;
    counted(f.universe);
    if (options & kPreview) return;  // for visualizers, not for output
    bool alt = buf[125] != 0;        // another start code (per-channel priorities, ...): not levels
    const uint8_t* cid = buf + 22;
    uint32_t nowMs = static_cast<uint32_t>(atUs / 1000);
    bool toSink;
    {
        tesser::MutexGuard g(lock_);
        Track* t = track(f.universe);
        if (!t) return;
        Source* s = sources(t);
        for (int i = t->sources - 1; i >= 0; i--) {
            if (nowMs - s[i].seenMs > kLossMs) drop(t, s, i);
        }
        int i = 0;
        while (i < t->sources && memcmp(s[i].cid, cid, 16) != 0) i++;
        if (options & kTerminated) {
            if (i < t->sources) drop(t, s, i);
            elect(t, s);
            return;
        }
        if (i == t->sources) {
            if (t->sources == kMaxSources) return;  // sources exceeded: the first ones keep it
            t->sources++;
            s[i] = Source();
            memcpy(s[i].cid, cid, 16);
            s[i].priority = f.priority;
            s[i].sequence = static_cast<uint8_t>(f.sequence - 1);
        } else {
            // E1.31 6.7.2: a frame 1 to 20 behind its source's last is out
            // of order, and dropped.
            int8_t d = static_cast<int8_t>(f.sequence - s[i].sequence);
            if (d <= 0 && d > -20) {
                sequenceErrors_++;
                return;
            }
            sequenced(s[i].sequence, f.sequence);
        }
        // Its sequence numbers count every start code's packets.
        s[i].sequence = f.sequence;
        s[i].ip = ip;
        s[i].seenMs = nowMs;
        if (alt) return;
        s[i].priority = f.priority;
        elect(t, s);
        if (t->winner != i) return;
        if (f.syncAddress && syncAddresses_.size() < kMaxSyncAddresses &&
            std::find(syncAddresses_.begin(), syncAddresses_.end(), f.syncAddress) == syncAddresses_.end()) {
            syncAddresses_.push_back(f.syncAddress);
        }
        toSink = take(t, f);
    }
    if (toSink) sink_->frame(f);
}

void Sacn::drop(Track* t, Source* s, int i) {
    if (t->winner == i) {
        t->winner = -1;
    } else if (t->winner > i) {
        t->winner--;
    }
    for (int j = i; j + 1 < t->sources; j++) s[j] = s[j + 1];
    t->sources--;
}

void Sacn::elect(Track* t, Source* s) {
    // The highest priority wins; among equals, the one winning keeps it.
    int best = t->winner;
    for (int i = 0; i < t->sources; i++) {
        if (best < 0 || s[i].priority > s[best].priority) best = i;
    }
    t->winner = static_cast<int8_t>(best);
}

}  // namespace tesserkit
