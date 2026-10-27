#include "tesserkit/fingerprint.h"

#include "tesser/node.h"

namespace tesserkit {

Fingerprint& Fingerprint::add(const tesser::Node* node) {
    if (node) inputs_.push_back({Input::Kind::Node, node});
    return *this;
}

Fingerprint& Fingerprint::add(const uint32_t& counter) {
    inputs_.push_back({Input::Kind::Counter, &counter});
    return *this;
}

Fingerprint& Fingerprint::add(const Fingerprint& other) {
    inputs_.push_back({Input::Kind::Nested, &other});
    return *this;
}

uint32_t Fingerprint::sum() const {
    uint32_t s = 0;
    for (const Input& i : inputs_) {
        switch (i.kind) {
            case Input::Kind::Node: s += static_cast<const tesser::Node*>(i.p)->generation(); break;
            case Input::Kind::Counter: s += *static_cast<const uint32_t*>(i.p); break;
            case Input::Kind::Nested: s += static_cast<const Fingerprint*>(i.p)->sum(); break;
        }
    }
    return s;
}

bool Fingerprint::changed() {
    uint32_t s = sum();
    if (s == seen_) return false;
    seen_ = s;
    return true;
}

}  // namespace tesserkit
