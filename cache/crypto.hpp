#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <sstream>
#include <iomanip>

namespace skipbuild {
namespace crypto {

// Standalone SHA-256 implementation (based on public domain/reference implementation)
class SHA256 {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];

    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    static uint32_t rotr(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (~x & z);
    }

    static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    static uint32_t sigma0(uint32_t x) {
        return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
    }

    static uint32_t sigma1(uint32_t x) {
        return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
    }

    static uint32_t sigma0_prime(uint32_t x) {
        return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
    }

    static uint32_t sigma1_prime(uint32_t x) {
        return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
    }

    void transform(const uint8_t* data) {
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        uint32_t w[64];

        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(data[i * 4]) << 24) | (uint32_t(data[i * 4 + 1]) << 16) |
                   (uint32_t(data[i * 4 + 2]) << 8) | uint32_t(data[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = sigma1_prime(w[i - 2]) + w[i - 7] + sigma0_prime(w[i - 15]) + w[i - 16];
        }

        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K[i] + w[i];
            uint32_t t2 = sigma0(a) + maj(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

public:
    SHA256() {
        reset();
    }

    void reset() {
        state[0] = 0x6a09e667;
        state[1] = 0xbb67ae85;
        state[2] = 0x3c6ef372;
        state[3] = 0xa54ff53a;
        state[4] = 0x510e527f;
        state[5] = 0x9b05688c;
        state[6] = 0x1f83d9ab;
        state[7] = 0x5be0cd19;
        count = 0;
        std::memset(buffer, 0, 64);
    }

    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t idx = count % 64;
        count += len;

        while (len > 0) {
            size_t space = 64 - idx;
            size_t copy = len < space ? len : space;
            std::memcpy(buffer + idx, p, copy);
            idx += copy;
            p += copy;
            len -= copy;
            if (idx == 64) {
                transform(buffer);
                idx = 0;
            }
        }
    }

    void update(const std::string& data) {
        update(data.data(), data.size());
    }

    std::array<uint8_t, 32> final() {
        size_t idx = count % 64;
        buffer[idx++] = 0x80;

        if (idx > 56) {
            while (idx < 64) buffer[idx++] = 0;
            transform(buffer);
            idx = 0;
        }
        while (idx < 56) buffer[idx++] = 0;

        uint64_t bits = count * 8;
        for (int i = 0; i < 8; ++i) {
            buffer[56 + i] = uint8_t(bits >> (56 - i * 8));
        }
        transform(buffer);

        std::array<uint8_t, 32> hash;
        for (int i = 0; i < 8; ++i) {
            hash[i * 4] = uint8_t(state[i] >> 24);
            hash[i * 4 + 1] = uint8_t(state[i] >> 16);
            hash[i * 4 + 2] = uint8_t(state[i] >> 8);
            hash[i * 4 + 3] = uint8_t(state[i]);
        }
        return hash;
    }

    std::string final_hex() {
        auto hash = final();
        std::ostringstream oss;
        for (uint8_t b : hash) {
            oss << std::hex << std::setw(2) << std::setfill('0') << int(b);
        }
        return oss.str();
    }
};

// HMAC-SHA256
class HMAC_SHA256 {
    SHA256 inner, outer;
    std::array<uint8_t, 64> key_pad;

public:
    HMAC_SHA256(const void* key, size_t key_len) {
        std::array<uint8_t, 32> key_hash;
        if (key_len > 64) {
            SHA256 hasher;
            hasher.update(key, key_len);
            key_hash = hasher.final();
            key = key_hash.data();
            key_len = 32;
        }
        std::memset(key_pad.data(), 0, 64);
        std::memcpy(key_pad.data(), key, key_len);
        for (size_t i = 0; i < 64; ++i) key_pad[i] ^= 0x36;
        inner.update(key_pad.data(), 64);
        for (size_t i = 0; i < 64; ++i) key_pad[i] ^= 0x36 ^ 0x5c;
        outer.update(key_pad.data(), 64);
    }

    HMAC_SHA256(const std::string& key) : HMAC_SHA256(key.data(), key.size()) {}

    void update(const void* data, size_t len) {
        inner.update(data, len);
    }

    void update(const std::string& data) {
        inner.update(data);
    }

    std::array<uint8_t, 32> final() {
        auto inner_hash = inner.final();
        outer.update(inner_hash.data(), 32);
        return outer.final();
    }

    std::string final_hex() {
        auto hash = final();
        std::ostringstream oss;
        for (uint8_t b : hash) {
            oss << std::hex << std::setw(2) << std::setfill('0') << int(b);
        }
        return oss.str();
    }
};

// Convenience functions
inline std::string sha256_hex(const std::string& data) {
    SHA256 hasher;
    hasher.update(data);
    return hasher.final_hex();
}

inline std::string sha256_hex(const void* data, size_t len) {
    SHA256 hasher;
    hasher.update(data, len);
    return hasher.final_hex();
}

inline std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    HMAC_SHA256 hmac(key);
    hmac.update(data);
    return hmac.final_hex();
}

inline std::string hmac_sha256_hex(const std::string& key, const void* data, size_t len) {
    HMAC_SHA256 hmac(key);
    hmac.update(data, len);
    return hmac.final_hex();
}

} // namespace crypto
} // namespace remake
