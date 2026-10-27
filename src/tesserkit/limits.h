// The longest persisted strings: every one is bounded, so the worst-case
// settings record is known and fits the NVS partition (docs/DESIGN.md
// section 11, "Settings in NVS"). TesserAPI bounds only char[N] values; a
// std::string one takes whatever a request carries (4 KB), and a list of 16
// elements with several of them could fill NVS alone.
#pragma once

#include <stddef.h>

#include <string>

#include "tesser/value_types.h"

namespace tesserkit {
namespace limits {

constexpr size_t kName = 32;         // names, user names
constexpr size_t kHost = 64;         // SNTP servers
constexpr size_t kSecret = 64;       // passwords, pairing codes, MQTT user names, client IDs and prefixes
constexpr size_t kLongSecret = 128;  // MQTT passwords (brokers' tokens)
constexpr size_t kUri = 128;         // MQTT brokers
constexpr size_t kPath = 96;         // tree paths: timers, the sequencer, MQTT, Art-Net
constexpr size_t kValue = 96;        // JSON values: timers, the sequencer
constexpr size_t kResult = 48;       // a timer's last result, cut to it

// Ok when `v` has at most `n` characters (bytes), else invalid_value with
// "at most <n> characters". Not a template: one copy of the check and its
// message for every limit (limits.cpp).
tesser::Check length(const std::string& v, size_t n);

// The same with the limit as a template argument: a call, nothing more.
template <size_t N>
tesser::Check length(const std::string& v) {
    return length(v, N);
}

// As a validator: .validate(limits::atMost<limits::kName>). Every limit is
// the same function type, so TesserAPI's validator wrapper is instantiated
// once.
template <size_t N>
tesser::Check atMost(const std::string& v) {
    return length(v, N);
}

}  // namespace limits
}  // namespace tesserkit
