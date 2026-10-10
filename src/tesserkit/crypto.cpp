#include "tesserkit/crypto.h"

#include <string.h>

namespace tesserkit {
namespace crypto {

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h_, init, sizeof(h_));
}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16 |
               static_cast<uint32_t>(p[4 * i + 2]) << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bits_ += static_cast<uint64_t>(len) * 8;
    while (len) {
        size_t n = 64 - used_ < len ? 64 - used_ : len;
        memcpy(buf_ + used_, p, n);
        used_ += n;
        p += n;
        len -= n;
        if (used_ == 64) {
            block(buf_);
            used_ = 0;
        }
    }
}

void Sha256::finish(uint8_t out[32]) {
    uint64_t bits = bits_;
    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (used_ != 56) update(&pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = static_cast<uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
    }
}

void sha256(const void* data, size_t len, uint8_t out[32]) {
    Sha256 s;
    s.update(data, len);
    s.finish(out);
}

HmacSha256::HmacSha256(const void* key, size_t keyLen) {
    uint8_t k[64] = {};
    if (keyLen > 64) {
        sha256(key, keyLen, k);
    } else {
        memcpy(k, key, keyLen);
    }
    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    inner_.update(pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    outer_.update(pad, 64);
}

void HmacSha256::compute(const void* data, size_t len, uint8_t out[32]) const {
    Sha256 inner = inner_;
    inner.update(data, len);
    uint8_t ih[32];
    inner.finish(ih);
    Sha256 outer = outer_;
    outer.update(ih, 32);
    outer.finish(out);
}

void hmacSha256(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[32]) {
    HmacSha256(key, keyLen).compute(data, len, out);
}

void pbkdf2Sha256(std::string_view password, const uint8_t* salt, size_t saltLen, uint32_t iterations, uint8_t* out,
                  size_t outLen) {
    HmacSha256 hmac(password.data(), password.size());
    std::string block(reinterpret_cast<const char*>(salt), saltLen);
    block.append(4, '\0');
    for (uint32_t index = 1; outLen; index++) {
        block[saltLen] = static_cast<char>(index >> 24);
        block[saltLen + 1] = static_cast<char>(index >> 16);
        block[saltLen + 2] = static_cast<char>(index >> 8);
        block[saltLen + 3] = static_cast<char>(index);
        uint8_t u[32], t[32];
        hmac.compute(block.data(), block.size(), u);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iterations; i++) {
            hmac.compute(u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t n = outLen < 32 ? outLen : 32;
        memcpy(out, t, n);
        out += n;
        outLen -= n;
    }
}

std::string toHex(const uint8_t* data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 15]);
    }
    return s;
}

bool fromHex(std::string_view hex, uint8_t* out, size_t len) {
    if (hex.size() != len * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < len; i++) {
        int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

bool equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace crypto
}  // namespace tesserkit
