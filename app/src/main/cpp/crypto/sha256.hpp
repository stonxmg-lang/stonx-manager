#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

// ════════════════════════════════════════════════════════════════════════════
// SHA-256 — تنفيذ داخلي خالص بدون مكتبات خارجية
// مطابق لـ FIPS 180-4
// ════════════════════════════════════════════════════════════════════════════

class SHA256 {
public:
    SHA256();

    void update(const uint8_t* data, size_t len);
    void update(const std::string& data);

    // يُعيد الـ digest كـ std::vector<uint8_t> (32 byte)
    std::vector<uint8_t> digest();

    // helpers: hash مباشر لـ bytes أو string
    static std::vector<uint8_t> hash(const uint8_t* data, size_t len);
    static std::vector<uint8_t> hash(const std::string& data);

    // hex string مباشر
    static std::string hex(const uint8_t* data, size_t len);
    static std::string hex(const std::string& data);
    static std::string hex(const std::vector<uint8_t>& data);

private:
    void transform(const uint8_t* block);
    void pad();

    uint32_t m_state[8];
    uint8_t  m_buf[64];
    uint64_t m_bits;
    uint32_t m_buf_len;
};

// ── HMAC-SHA256 ───────────────────────────────────────────────────────────
// hex string
std::string hmac_sha256_hex(const std::string& key, const std::string& message);
// raw bytes
std::vector<uint8_t> hmac_sha256(const std::string& key, const std::string& message);
