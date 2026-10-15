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

// Ok when `v` has at most N characters (bytes).
template <size_t N>
tesser::Check length(const std::string& v) {
    if (v.size() <= N) return tesser::Check::ok();
    static const std::string message = "at most " + std::to_string(N) + " characters";
    return tesser::Check::fail(tesser::Status::InvalidValue, message.c_str());
}

// As a validator: .validate(limits::atMost<limits::kName>).
template <size_t N>
tesser::Check atMost(const std::string& v) {
    return length<N>(v);
}

}  // namespace limits
}  // namespace tesserkit
