#include "tesserkit/limits.h"

#include <stdio.h>
#include <stdlib.h>

#include <atomic>

namespace tesserkit {
namespace limits {

namespace {

// The messages, made once for each limit, on the heap, and never freed: a
// Check keeps a pointer. A slot is claimed with its index, filled, then
// published by its limit, so two tasks failing at once can't read one half
// written. 8 bytes of RAM a slot until a limit is hit.
struct Message {
    std::atomic<size_t> n{0};  // 0: not published yet
    char* text = nullptr;
};
constexpr size_t kMessages = 12;  // TesserKIT has 8 limits
Message g_messages[kMessages];
std::atomic<size_t> g_claimed{0};

const char* messageFor(size_t n) {
    size_t claimed = g_claimed.load();
    for (size_t i = 0; i < claimed && i < kMessages; i++) {
        if (g_messages[i].n.load(std::memory_order_acquire) == n) return g_messages[i].text;
    }
    constexpr size_t kText = 32;
    char* text = static_cast<char*>(malloc(kText));  // a short heap: a vaguer message rather than an abort
    if (!text) return "too long";
    size_t i = g_claimed.fetch_add(1);
    if (i >= kMessages) {
        free(text);
        return "too long";
    }
    snprintf(text, kText, "at most %u characters", static_cast<unsigned>(n));
    g_messages[i].text = text;
    g_messages[i].n.store(n, std::memory_order_release);
    return text;
}

}  // namespace

tesser::Check length(const std::string& v, size_t n) {
    if (v.size() <= n) return tesser::Check::ok();
    return tesser::Check::fail(tesser::Status::InvalidValue, messageFor(n));
}

}  // namespace limits
}  // namespace tesserkit
