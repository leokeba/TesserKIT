#pragma once

// SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 for Auth (docs/DESIGN.md
// section 5.7): password hashes and signed tokens. Plain C++, so the host
// tests run the same code as the chip.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>

namespace tesserkit {
namespace crypto {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    void finish(uint8_t out[32]);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t bits_ = 0;
};

void sha256(const void* data, size_t len, uint8_t out[32]);

// HMAC-SHA256 with its key's padded blocks hashed once: each message then
// costs two compressions fewer (PBKDF2 runs thousands).
class HmacSha256 {
public:
    HmacSha256(const void* key, size_t keyLen);
    void compute(const void* data, size_t len, uint8_t out[32]) const;

private:
    Sha256 inner_, outer_;
};

void hmacSha256(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[32]);
void pbkdf2Sha256(std::string_view password, const uint8_t* salt, size_t saltLen, uint32_t iterations, uint8_t* out,
                  size_t outLen);

std::string toHex(const uint8_t* data, size_t len);
// False unless `hex` is exactly 2 * len hex digits.
bool fromHex(std::string_view hex, uint8_t* out, size_t len);
// Compares without leaking where they differ.
bool equal(std::string_view a, std::string_view b);

}  // namespace crypto
}  // namespace tesserkit
