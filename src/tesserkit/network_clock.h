#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "tesserkit/device.h"

namespace nowtp {
class TimeSync;
}  // namespace nowtp

namespace tesserkit {

class UdpApi;

// One network time for a fleet that mixes cables and radio (docs/DESIGN.md
// section 5.25): NowTP's TimeSync, with UDP (UdpApi, through TesserAPI's side
// channel) and ESP-NOW (NowTp) as its carriers, one election across both.
// Also the node's timed writes (section 5.26): TesserAPI's scheduler, which
// queues a set carrying "at" and applies it at that network time.
//
// Declared after the UdpApi it uses (null: no UDP carrier); a NowTp of the
// device serves it on its own (ESP-NOW then becomes its "nowtp" carrier):
//   NetworkClock clock(dev, &udp);
// Needs NowTP in the build (its TimeSync), even with ESP-NOW off; without
// it, the service says so in /sys/clock/status and refuses timed writes.
class NetworkClock : public Service {
public:
    static constexpr size_t kMaxQueued = 64;         // timed writes waiting
    static constexpr size_t kMaxQueuedBytes = 8192;  // their paths and values
    static constexpr uint32_t kMaxLateMs = 1000;

    // Counts of timed writes since boot, and the latest applied write's
    // lateness (the lock held it, or the task woke late).
    struct Writes {
        uint32_t queued = 0;   // now waiting
        uint32_t applied = 0;
        uint32_t late = 0;     // came after their time plus `late`: dropped
        uint32_t busy = 0;     // the queue was full: refused
        uint32_t failed = 0;   // refused when applied (the node, its validator)
        uint32_t lateMaxUs = 0;
    };

    NetworkClock(Device& dev, UdpApi* udp);
    ~NetworkClock() override;

    // Factory defaults, before Device::begin().
    // "auto" (a candidate: the nodes elect a reference), "this" (this node's
    // clock is the network time: an ensemble's master), or the MAC of the
    // device to follow.
    NetworkClock& reference(const char* ref);
    // The carriers it may use, in order of preference: "udp, nowtp".
    NetworkClock& carriers(const char* order);
    NetworkClock& interval(uint32_t seconds);
    // How late a timed write may come and still be applied (default 0).
    NetworkClock& late(uint32_t ms);

    // Network time in µs; the local clock while unsynchronized. From any task.
    uint64_t networkTimeUs() const;
    // The local clock's value (esp_timer_get_time()) at network time
    // `networkUs`. From any task.
    int64_t localTimeUs(uint64_t networkUs) const;
    bool timeSynced() const;
    Writes writes() const;

    // NowTP's TimeSync, which NowTp serves: null without NowTP in the build.
    nowtp::TimeSync* core();

    void describe(tesser::Object& o) override;
    NetworkClock* asNetworkClock() override { return this; }

protected:
    void loaded() override;
    void start() override;
    void tick(uint32_t nowMs) override;

private:
    struct Impl;
    // /sys/clock/status, as last read.
    struct Status_ {
        bool synced = false, isReference = false;
        std::string source, carrier, error;
        int64_t offsetUs = 0;
        uint32_t roundTripUs = 0;
        float driftPpm = 0;
        uint32_t rounds = 0, ageMs = 0;
    };
    void applyReference();
    void applyCarriers();
    void refresh();

    UdpApi* udp_;
    std::unique_ptr<Impl> impl_;

    std::string reference_ = "auto";
    std::string carriers_ = "udp, nowtp";
    uint32_t interval_ = 5;  // s
    uint32_t late_ = 0;      // ms
    Status_ status_;
    Writes writes_;

    tesser::Node* referenceNode_ = nullptr;
    tesser::Node* carriersNode_ = nullptr;
    tesser::Node* intervalNode_ = nullptr;
    tesser::Node* statusNode_ = nullptr;
    tesser::Node* writesNode_ = nullptr;
    uint32_t referenceGen_ = 0, carriersGen_ = 0, intervalGen_ = 0;
    uint32_t refreshedMs_ = 0;
};

}  // namespace tesserkit
