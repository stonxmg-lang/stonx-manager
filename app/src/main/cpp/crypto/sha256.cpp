#include "sha256.hpp"
#include <cstring>
#include <cstdio>

// ── SHA-256 ثوابت (FIPS 180-4 §4.2.2) ───────────────────────────────────
static constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// ── دوال مساعدة ───────────────────────────────────────────────────────────
static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}
static inline uint32_t Ch (uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static inline uint32_t Sigma0(uint32_t x) { return rotr32(x,2)  ^ rotr32(x,13) ^ rotr32(x,22); }
static inline uint32_t Sigma1(uint32_t x) { return rotr32(x,6)  ^ rotr32(x,11) ^ rotr32(x,25); }
static inline uint32_t sigma0(uint32_t x) { return rotr32(x,7)  ^ rotr32(x,18) ^ (x >> 3);     }
static inline uint32_t sigma1(uint32_t x) { return rotr32(x,17) ^ rotr32(x,19) ^ (x >> 10);    }

static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static inline void wr32(uint8_t* p, uint32_t v) {
    p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF;
}

// ── تنفيذ SHA256 ──────────────────────────────────────────────────────────
SHA256::SHA256() {
    // قيم الحالة الابتدائية (FIPS 180-4 §5.3.3)
    m_state[0] = 0x6a09e667; m_state[1] = 0xbb67ae85;
    m_state[2] = 0x3c6ef372; m_state[3] = 0xa54ff53a;
    m_state[4] = 0x510e527f; m_state[5] = 0x9b05688c;
    m_state[6] = 0x1f83d9ab; m_state[7] = 0x5be0cd19;
    m_bits    = 0;
    m_buf_len = 0;
    memset(m_buf, 0, sizeof(m_buf));
}

void SHA256::transform(const uint8_t* block) {
    uint32_t W[64], a, b, c, d, e, f, g, h, T1, T2;

    for (int i = 0; i < 16; ++i)  W[i] = be32(block + i*4);
    for (int i = 16; i < 64; ++i) W[i] = sigma1(W[i-2]) + W[i-7] + sigma0(W[i-15]) + W[i-16];

    a = m_state[0]; b = m_state[1]; c = m_state[2]; d = m_state[3];
    e = m_state[4]; f = m_state[5]; g = m_state[6]; h = m_state[7];

    for (int i = 0; i < 64; ++i) {
        T1 = h + Sigma1(e) + Ch(e,f,g) + K[i] + W[i];
        T2 = Sigma0(a) + Maj(a,b,c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    m_state[0]+=a; m_state[1]+=b; m_state[2]+=c; m_state[3]+=d;
    m_state[4]+=e; m_state[5]+=f; m_state[6]+=g; m_state[7]+=h;
}

void SHA256::update(const uint8_t* data, size_t len) {
    m_bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t space = 64 - m_buf_len;
        size_t take  = (len < space) ? len : space;
        memcpy(m_buf + m_buf_len, data, take);
        m_buf_len += (uint32_t)take;
        data      += take;
        len       -= take;
        if (m_buf_len == 64) {
            transform(m_buf);
            m_buf_len = 0;
        }
    }
}

void SHA256::update(const std::string& data) {
    update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

void SHA256::pad() {
    // احفظ طول الرسالة الأصلية قبل إضافة الـ padding
    // (update() بتزيد m_bits، فلو حفظناها بعد الـ padding سيكون الطول غلط)
    uint64_t original_bits = m_bits;

    uint8_t pad_byte = 0x80;
    update(&pad_byte, 1);
    while (m_buf_len != 56) {
        uint8_t zero = 0;
        update(&zero, 1);
    }
    // اكتب طول الرسالة الأصلية (بدون الـ padding) كـ big-endian 64-bit
    uint8_t len_be[8];
    uint64_t bits = original_bits;
    for (int i = 7; i >= 0; --i) { len_be[i] = bits & 0xFF; bits >>= 8; }
    update(len_be, 8);
}

std::vector<uint8_t> SHA256::digest() {
    // احفظ الحالة عشان pad() تعدّلها مؤقتاً
    SHA256 copy = *this;
    copy.pad();
    std::vector<uint8_t> out(32);
    for (int i = 0; i < 8; ++i) wr32(out.data() + i*4, copy.m_state[i]);
    return out;
}

// ── helpers ───────────────────────────────────────────────────────────────
std::vector<uint8_t> SHA256::hash(const uint8_t* data, size_t len) {
    SHA256 h; h.update(data, len); return h.digest();
}
std::vector<uint8_t> SHA256::hash(const std::string& data) {
    return hash(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}
std::string SHA256::hex(const std::vector<uint8_t>& d) {
    return hex(d.data(), d.size());
}
std::string SHA256::hex(const uint8_t* data, size_t len) {
    std::string out(len*2, '\0');
    static constexpr char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i*2]   = HEX[(data[i] >> 4) & 0xF];
        out[i*2+1] = HEX[data[i] & 0xF];
    }
    return out;
}
std::string SHA256::hex(const std::string& data) {
    return hex(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// ── HMAC-SHA256 ───────────────────────────────────────────────────────────
std::vector<uint8_t> hmac_sha256(const std::string& key, const std::string& message) {
    const size_t BLOCK = 64;
    std::vector<uint8_t> k(key.begin(), key.end());

    // لو المفتاح أطول من block size نُقلّصه بـ SHA256
    if (k.size() > BLOCK) {
        auto kh = SHA256::hash(k.data(), k.size());
        k = kh;
    }
    k.resize(BLOCK, 0);

    std::vector<uint8_t> ipad(BLOCK), opad(BLOCK);
    for (size_t i = 0; i < BLOCK; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }

    // inner = SHA256(ipad || message)
    SHA256 inner;
    inner.update(ipad.data(), BLOCK);
    inner.update(reinterpret_cast<const uint8_t*>(message.data()), message.size());
    auto inner_hash = inner.digest();

    // outer = SHA256(opad || inner)
    SHA256 outer;
    outer.update(opad.data(), BLOCK);
    outer.update(inner_hash.data(), inner_hash.size());
    return outer.digest();
}

std::string hmac_sha256_hex(const std::string& key, const std::string& message) {
    auto raw = hmac_sha256(key, message);
    return SHA256::hex(raw);
}
