// The global operator new and delete of the test binary, counting while an
// alloccap::Heap lives (alloc_cap.h).
#include "alloc_cap.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <cxxabi.h>
#include <mutex>
#include <new>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#define ALLOCCAP_SIZE(p) malloc_size(p)
#else
#include <malloc.h>
#define ALLOCCAP_SIZE(p) malloc_usable_size(p)
#endif

#include "fake_platform.h"
#include "tesser/platform.h"

namespace alloccap {

namespace {

std::atomic<bool> g_on{false};
std::atomic<long long> g_live{0};
std::atomic<long long> g_peak{0};
std::atomic<long long> g_allocs{0};
long long g_size = 0;
size_t g_floorBefore = 0;
std::mutex g_mutex;  // the overruns
Overrun g_overruns[8];
int g_count = 0;
thread_local bool t_inside = false;  // backtrace() may allocate

size_t remaining() {
    long long left = g_size - g_live.load();
    return left > 0 ? static_cast<size_t>(left) : 0;
}

// The blocks counted, so a block allocated before the heap started, or as
// a fake's flash, gives nothing back when freed: an open-addressing table
// that never allocates (it runs inside operator new).
constexpr size_t kSlots = size_t(1) << 18;
void* const kTomb = reinterpret_cast<void*>(1);
struct Block {
    void* p;
    size_t size;
};
Block g_blocks[kSlots];
std::atomic_flag g_blocksLock = ATOMIC_FLAG_INIT;

struct BlocksGuard {
    BlocksGuard() {
        while (g_blocksLock.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~BlocksGuard() { g_blocksLock.clear(std::memory_order_release); }
};

size_t slotOf(void* p) { return (reinterpret_cast<uintptr_t>(p) >> 4) * 2654435761u % kSlots; }

bool track(void* p, size_t size) {
    BlocksGuard guard;
    for (size_t i = slotOf(p), n = 0; n < kSlots; i = (i + 1) % kSlots, n++) {
        if (!g_blocks[i].p || g_blocks[i].p == kTomb) {
            g_blocks[i] = {p, size};
            return true;
        }
    }
    return false;  // full: not counted
}

// The size counted for `p`, forgotten; 0 if it wasn't.
size_t untrack(void* p) {
    BlocksGuard guard;
    for (size_t i = slotOf(p), n = 0; n < kSlots && g_blocks[i].p; i = (i + 1) % kSlots, n++) {
        if (g_blocks[i].p == p) {
            size_t size = g_blocks[i].size;
            g_blocks[i] = {kTomb, 0};
            return size;
        }
    }
    return 0;
}

void forgetAll() {
    BlocksGuard guard;
    memset(g_blocks, 0, sizeof(g_blocks));
}

void counted(void* p, size_t size) {
    if (!p || !g_on.load() || t_inside || fake::NotHeap::active()) return;
    size_t real = ALLOCCAP_SIZE(p);
    if (!track(p, real)) return;
    g_allocs.fetch_add(1);
    long long now = g_live.fetch_add(static_cast<long long>(real)) + static_cast<long long>(real);
    long long peak = g_peak.load();
    while (now > peak && !g_peak.compare_exchange_weak(peak, now)) {
    }
    if (now <= g_size) return;
    t_inside = true;
    if (getenv("ALLOCCAP_ABORT")) {
        fprintf(stderr, "alloccap: %zu bytes past the heap (%lld live of %lld): abort, as ESP-IDF\n", size, now,
                g_size);
        abort();
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_count < 8) {
            Overrun& o = g_overruns[g_count++];
            o.size = size;
            o.live = now;
            o.depth = backtrace(o.frames, 24);
        }
    }
    t_inside = false;
}

void uncounted(void* p) {
    if (!p) return;
    size_t size = untrack(p);
    if (size) g_live.fetch_sub(static_cast<long long>(size));
}

void* allocate(size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) abort();  // as ESP-IDF: new doesn't return null
    counted(p, size);
    return p;
}

void release(void* p) {
    uncounted(p);
    free(p);
}

}  // namespace

Heap::Heap(size_t bytes, size_t floor) {
    forgetAll();
    g_live = 0;
    g_peak = 0;
    g_allocs = 0;
    g_size = static_cast<long long>(bytes);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_count = 0;
    }
    tesser::setFreeHeapProbe(&remaining);
    tesser::setLargestFreeBlockProbe(&remaining);
    g_floorBefore = tesser::heapFloor();
    tesser::setHeapFloor(floor);
    g_on = true;
}

Heap::~Heap() {
    g_on = false;
    tesser::setFreeHeapProbe(nullptr);
    tesser::setLargestFreeBlockProbe(nullptr);
    tesser::setHeapFloor(g_floorBefore);
}

long long Heap::live() const { return g_live.load(); }
long long Heap::peak() const { return g_peak.load(); }
long long Heap::allocations() const { return g_allocs.load(); }

std::vector<Overrun> Heap::overruns() const {
    bool was = t_inside;
    t_inside = true;  // the test's own allocations
    std::vector<Overrun> out;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        out.assign(g_overruns, g_overruns + g_count);
    }
    t_inside = was;
    return out;
}

std::string Heap::report() const {
    bool was = t_inside;
    t_inside = true;
    std::string out;
    for (const Overrun& o : overruns()) {
        char line[160];
        snprintf(line, sizeof(line), "  %zu bytes, %lld live of %lld:\n", o.size, o.live, g_size);
        out += line;
        for (int i = 2; i < o.depth && i < 14; i++) {  // past counted() and allocate()
            Dl_info info;
            const char* name = "?";
            char* demangled = nullptr;
            if (dladdr(o.frames[i], &info) && info.dli_sname) {
                int status = 0;
                demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                name = demangled && status == 0 ? demangled : info.dli_sname;
            }
            std::string n(name);
            if (n.size() > 140) n = n.substr(0, 140) + "...";
            out += "    " + n + "\n";
            free(demangled);
        }
    }
    t_inside = was;
    return out;
}

}  // namespace alloccap

// The test binary's allocator.
void* operator new(size_t size) { return alloccap::allocate(size); }
void* operator new[](size_t size) { return alloccap::allocate(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return alloccap::allocate(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return alloccap::allocate(size); }
void operator delete(void* p) noexcept { alloccap::release(p); }
void operator delete[](void* p) noexcept { alloccap::release(p); }
void operator delete(void* p, size_t) noexcept { alloccap::release(p); }
void operator delete[](void* p, size_t) noexcept { alloccap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { alloccap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { alloccap::release(p); }
