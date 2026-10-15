// An allocator cap for the host tests: a short heap, as a classic ESP32's
// without PSRAM. The test binary replaces the global operator new and
// delete (alloc_cap.cpp); while an alloccap::Heap lives, the bytes they hand
// out above what was live when it started count against its size, and
// TesserAPI's accountant sees what is left (its free-heap and largest-block
// probes), so its reservations are refused as on the device. An allocation
// that goes past the size is what aborts on ESP-IDF (no exceptions: a
// failed new aborts, std::nothrow included): it is recorded, with its size
// and call stack, for the test to fail on (or aborts, with ALLOCCAP_ABORT
// set). ArduinoJson's documents allocate with malloc, which isn't counted.
//
// TesserAPI may grow a helper of its own (tesserapi-ae): this one is small
// on purpose, so it can move there.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace alloccap {

struct Overrun {
    size_t size = 0;  // the allocation
    long long live = 0;  // bytes live above the start, with it
    void* frames[24] = {};
    int depth = 0;
};

class Heap {
public:
    // `bytes` of heap from now on, `floor` of them kept by TesserAPI's
    // accountant for everything else (setHeapFloor).
    explicit Heap(size_t bytes, size_t floor = 8192);
    ~Heap();
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    // Bytes live above the start, now and at the most.
    long long live() const;
    long long peak() const;
    // The allocations that went past the size (the first 8).
    std::vector<Overrun> overruns() const;
    // Them, readable: sizes and symbolized call stacks.
    std::string report() const;
};

}  // namespace alloccap
