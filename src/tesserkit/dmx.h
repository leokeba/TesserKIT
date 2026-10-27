#pragma once

// What ArtNet and Sacn share (docs/DESIGN.md section 5.9): DMX universes
// received over UDP, mapped onto nodes of the tree, and handed raw to an
// application's sink.

#include <stdlib.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "tesser/client.h"
#include "tesser/platform.h"
#include "tesserkit/device.h"

namespace tesserkit {

enum class DmxProtocol : uint8_t { ArtNet, Sacn };

// A DMX frame as it arrived, for a DmxSink. Its data lives only during the
// call.
struct DmxFrame {
    DmxProtocol protocol = DmxProtocol::ArtNet;
    uint16_t universe = 0;      // Art-Net: the 15-bit port-address; sACN: 1–63999
    uint8_t sequence = 0;       // the sender's; Art-Net: 0 if it doesn't number them
    uint8_t priority = 0;       // sACN: 0–200; Art-Net: 0
    uint16_t syncAddress = 0;   // sACN: the universe whose sync packet shows the frame; 0: show it now
    const uint8_t* data = nullptr;  // the channels, without the start code
    uint16_t length = 0;        // 1–512
    uint32_t source = 0;        // the sender's IPv4 address, a.b.c.d as (a << 24) | ...
    uint64_t atUs = 0;          // when it arrived: Core::uptimeMs()'s clock, in µs
};

// A synchronization packet: Art-Net's ArtSync, or E1.31's sync packet.
struct DmxSync {
    DmxProtocol protocol = DmxProtocol::ArtNet;
    uint16_t address = 0;  // sACN: the synchronization universe; Art-Net: 0
    uint32_t source = 0;
    uint64_t atUs = 0;
};

// An application's receiver of DMX frames, beside the tree's mappings.
// Called in the receiving task as datagrams arrive, not under the API lock:
// copy what is needed and return. It must not call the service back. It may
// take the API lock (tesser::Lock) for a write to the tree, briefly: every
// frame waits for it, and closing the socket (the service disabled) waits for
// the call to return, which the service does outside the lock.
class DmxSink {
public:
    virtual ~DmxSink() = default;
    // A frame of one of the service's universes(): for sACN, from the source
    // with the highest priority.
    virtual void frame(const DmxFrame& f) = 0;
    // Every synchronization packet, whichever its source.
    virtual void sync(const DmxSync& s) { (void)s; }
};

// An element of the mappings: DMX channels written to a node.
struct DmxMapping {
    std::string path;     // the node to set
    uint16_t coarse = 0;  // channel 1–512; 0: unused
    uint16_t fine = 0;    // its low byte's channel; 0: 8 bits
    double min = 0, max = 1;
    // 1–255: a gate, written as the boolean coarse >= threshold.
    uint8_t threshold = 0;
};

// The receiving task's options (DmxService::sink()).
struct DmxTask {
    uint8_t priority = 12;  // below lwIP's (18), above the device's and the HTTP server's (5)
    int8_t core = -1;       // -1: either
    uint32_t stackBytes = 3072;  // the service's own use is under 1 KB, the rest is the sink's
};

// The base of ArtNet and Sacn.
class DmxService : public Service {
public:
    static constexpr size_t kMaxMappings = 8;
    static constexpr size_t kMaxUniverses = 64;  // the sink's

    // Factory defaults, before Device::begin().
    DmxService& enable(bool on);
    DmxService& universe(uint16_t universe);  // the mappings'
    DmxService& map(const DmxMapping& m);
    // The application's sink: frames of universes() go to it as they
    // arrive, from a task of the service's (unless the task can't be
    // created: then from the device's task, every period). Before
    // Device::begin().
    DmxService& sink(DmxSink* sink, const DmxTask& task = DmxTask());

    // The universes the sink receives: any number up to kMaxUniverses
    // (beyond, and those out of the protocol's range, are left out). From
    // any task but the sink's calls.
    void universes(const std::vector<uint16_t>& list);
    std::vector<uint16_t> universes() const;

    // The value a mapping writes for a DMX frame, as JSON ("" if the frame
    // doesn't reach its channels). `kind` is the target value's.
    static std::string mapped(const DmxMapping& m, const uint8_t* dmx, size_t len, tesser::ValueKind kind);

protected:
    // `first`–`last`: the protocol's universes.
    DmxService(Device& dev, const char* name, DmxProtocol protocol, uint16_t port, uint16_t universe,
               uint16_t first, uint16_t last);

    void start() override;
    void tick(uint32_t nowMs) override;

    // A source of E1.31 data on a universe (Sacn's).
    struct Source {
        uint8_t cid[16] = {};
        uint32_t ip = 0;
        uint32_t seenMs = 0;
        uint8_t priority = 0;
        uint8_t sequence = 0;
    };
    static constexpr size_t kMaxSources = 3;

    // A universe the sink or the mappings receive.
    struct Track {
        uint16_t universe = 0;
        bool sink = false, mapped = false;
        bool sequenced = false;  // `sequence` holds the last frame's (ArtNet's)
        uint8_t sequence = 0;
        bool everFrame = false;
        uint32_t frameMs = 0;
        uint8_t sources = 0;  // Sacn's, in sources(this)
        int8_t winner = -1;
    };

    // In the receiving context (the receiving task, or the device's task
    // without one): one datagram. The protocol parses it and calls back
    // frame(), sync() or polled().
    virtual void received(const uint8_t* data, size_t len, uint32_t fromIp, uint64_t atUs) = 0;
    // In the device's task, every period while the socket is open.
    virtual void ticked(uint32_t nowMs) { (void)nowMs; }
    // The socket was just opened, or is about to close.
    virtual void opened() {}
    virtual void closing() {}

    // The nodes both services have, after the subclass's label.
    void describeDmx(tesser::Object& o, const char* universeDoc);

    // From received(), in the receiving context. A data packet of
    // `universe`, counted whatever becomes of it.
    void counted(uint16_t universe);
    // The track of `universe`, null if nothing receives it. Under lock_.
    Track* track(uint16_t universe);
    // A frame `t` takes (under lock_): kept for the mappings if theirs.
    // True if the sink takes it too, which the caller then hands it out of
    // the lock (sink_->frame()).
    bool take(Track* t, const DmxFrame& f);
    void sync(const DmxSync& s);
    // Sacn's kMaxSources sources of a track. Under lock_.
    Source* sources(const Track* t) { return &sources_[static_cast<size_t>(t - tracks_.data()) * kMaxSources]; }
    // Counts a frame whose sequence number doesn't follow `last` (Art-Net
    // skips 0 on wrapping).
    void sequenced(uint8_t last, uint8_t now);
    void failed(const std::string& error);

    // Under lock_: the tracks, the mappings' frame, ArtNet's polls, Sacn's
    // sync addresses.
    mutable tesser::Mutex lock_;
    std::vector<Track> tracks_;  // sorted by universe
    std::vector<Source> sources_;  // Sacn's: kMaxSources per track
    std::vector<uint16_t> listened_;  // the sink's universes, sorted

    DmxProtocol protocol_;
    uint16_t port_;
    uint16_t first_, last_;
    bool enabled_ = true;
    uint16_t universe_;
    std::vector<DmxMapping> mappings_;
    std::string error_;
    std::atomic<uint32_t> packets_{0};
    std::atomic<uint32_t> sequenceErrors_{0};
    std::atomic<uint16_t> lastUniverse_{0};
    uint32_t nowMs_ = 0;
    std::unique_ptr<UdpSocket> socket_;
    // A socket the service closed: destroyed by a poller of the device's
    // task, out of the API lock, since destroying it waits for its receiving
    // task, which may be in the sink, waiting for that lock.
    std::unique_ptr<UdpSocket> closed_;
    DmxSink* sink_ = nullptr;

private:
    void rebuild();  // under lock_
    // The mappings' two frames, allocated while there are mappings (a
    // warning if the heap can't, unless !tell). Under lock_.
    void frames(bool tell = true);
    void receive(uint32_t nowMs);
    void apply(const uint8_t* dmx, size_t len);

    DmxTask task_;
    // The counters and the mappings' universe as of the last period, for
    // the status.
    struct {
        uint32_t packets = 0, sequenceErrors = 0, frameMs = 0;
        uint16_t universe = 0;
        bool received = false;
    } shown_;
    bool listening_ = false;  // the receiving task runs
    tesser::Node* status_ = nullptr;
    tesser::EventNode* refused_ = nullptr;
    tesser::Node* universeNode_ = nullptr;
    tesser::Node* mappingsNode_ = nullptr;
    uint32_t settingsGen_ = 0;
    Udp* udp_ = nullptr;
    uint32_t openedAtMs_ = 0;
    bool opening_ = false;
    std::unique_ptr<tesser::LocalClient> client_;
    // The mappings' universe: two frames of 512 bytes, only while there are
    // mappings. take() writes the last one into frame_ (under lock_); tick()
    // swaps it with applied_, which it reads out of the lock: no copy.
    struct FreeBytes {
        void operator()(uint8_t* p) const { free(p); }
    };
    std::unique_ptr<uint8_t, FreeBytes> frames_;
    uint8_t* frame_ = nullptr;
    uint8_t* applied_ = nullptr;
    size_t frameLen_ = 0;
    bool fresh_ = false;  // a frame since the last apply()
    std::vector<std::string> sent_;  // per mapping: the last value written
};

}  // namespace tesserkit
