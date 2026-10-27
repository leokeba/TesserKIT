#pragma once

#include <stdint.h>

#include <vector>

namespace tesser {
class Node;
}

namespace tesserkit {

// Whether any of a set of settings changed since a service last applied
// them. Each input is a generation, which only grows (a node's, or a counter
// of the device's), so their sum moves whenever one of them does. The inputs
// are listed once, as the fingerprint is built (in describe() or start()),
// rather than summed again wherever they are read: an input left out is a
// setting the service silently ignores (Mqtt and the hostname, fixed on
// 2026-10-27). A value that can go back (a flag) isn't a generation: a sum
// would miss it moving one way while a generation moves the other.
class Fingerprint {
public:
    // A node's generation; a null node adds nothing (an optional one).
    Fingerprint& add(const tesser::Node* node);
    // A generation kept elsewhere, read each time: it must outlive this.
    // Not a copy: an accessor returning one by value doesn't build.
    Fingerprint& add(const uint32_t& counter);
    Fingerprint& add(const uint32_t&& copy) = delete;
    // Every input of another fingerprint, as they are each time: it must
    // outlive this.
    Fingerprint& add(const Fingerprint& other);
    // Forgets the inputs, to list them again.
    void clear() { inputs_.clear(); }

    // The sum of the inputs now.
    uint32_t sum() const;
    // Takes the inputs as they are as applied (start()).
    void seen() { seen_ = sum(); }
    // Whether an input moved since seen() or the last changed() that said
    // so; taken as seen if it did.
    bool changed();

private:
    struct Input {
        enum class Kind : uint8_t { Node, Counter, Nested } kind;
        const void* p;
    };
    std::vector<Input> inputs_;
    uint32_t seen_ = 0;
};

}  // namespace tesserkit
