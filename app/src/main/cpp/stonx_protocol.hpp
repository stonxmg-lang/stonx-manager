#pragma once
#include <string>
#include <cstdint>
#include <cstddef>
#include "config.hpp"

// ════════════════════════════════════════════════════════════════════════════
// STONX Protocol v2
// Frame format: [4-byte BE length][JSON payload]
// Auth: HMAC-SHA256(PSK, nonce:timestamp)
// ════════════════════════════════════════════════════════════════════════════

namespace StonxProto {

// ── أنواع الرسائل ──────────────────────────────────────────────────────────
constexpr const char* TYPE_HELLO      = "HELLO";
constexpr const char* TYPE_CHALLENGE  = "CHALLENGE";
constexpr const char* TYPE_AUTH       = "AUTH";
constexpr const char* TYPE_AUTH_OK    = "AUTH_OK";
constexpr const char* TYPE_AUTH_FAIL  = "AUTH_FAIL";
constexpr const char* TYPE_COMMAND    = "COMMAND";
constexpr const char* TYPE_RESPONSE   = "RESPONSE";
constexpr const char* TYPE_CHUNK      = "CHUNK";
constexpr const char* TYPE_CHUNK_ACK  = "CHUNK_ACK";
constexpr const char* TYPE_CHUNK_NACK = "CHUNK_NACK";
constexpr const char* TYPE_XFER_INIT  = "TRANSFER_INIT";
constexpr const char* TYPE_XFER_IACK  = "TRANSFER_INIT_ACK";
constexpr const char* TYPE_XFER_DONE  = "TRANSFER_COMPLETE";
constexpr const char* TYPE_XFER_DACK  = "TRANSFER_COMPLETE_ACK";
constexpr const char* TYPE_EVENT      = "EVENT";
constexpr const char* TYPE_PING       = "PING";
constexpr const char* TYPE_PONG       = "PONG";
constexpr const char* TYPE_ERROR      = "ERROR";

constexpr const char* STATUS_OK    = "OK";
constexpr const char* STATUS_ERROR = "ERROR";

// ── نتائج القراءة/الكتابة ────────────────────────────────────────────────
enum class IOResult {
    OK,
    TIMEOUT,
    DISCONNECTED,
    FRAME_TOO_LARGE,
    PARSE_ERROR,
    AUTH_FAILED,
    ERROR
};

// ── كتابة frame على socket ────────────────────────────────────────────────
// يُرسَل: [4-byte BE size][json]
IOResult write_frame(int fd, const std::string& json);

// ── قراءة frame من socket ─────────────────────────────────────────────────
// json يملأ الـ out_json
IOResult read_frame(int fd, std::string& out_json,
                    int timeout_sec = 60,
                    int max_size = Config::MAX_FRAME_SIZE);

// ── توليد معرفات فريدة ───────────────────────────────────────────────────
std::string new_message_id();
std::string new_nonce();
std::string new_operation_id();
std::string new_transfer_id();

// ── بناء JSON مبسط (بدون مكتبة json خارجية) ────────────────────────────
// نستخدم بناء json يدوي لتجنب dependency خارجية
std::string build_hello(const std::string& device_id,
                        const std::string& device_name);

std::string build_auth_response(const std::string& nonce,
                                int64_t timestamp,
                                const std::string& psk);

std::string build_ping();

std::string build_response_ok(const std::string& command,
                               const std::string& op_id,
                               const std::string& payload_json);

std::string build_response_error(const std::string& command,
                                  const std::string& op_id,
                                  const std::string& error_code);

std::string build_transfer_init(const std::string& transfer_id,
                                 const std::string& file_name,
                                 int64_t total_size,
                                 const std::string& sha256,
                                 int total_chunks,
                                 int chunk_size,
                                 bool compressed);

std::string build_chunk_msg(const std::string& transfer_id,
                             int chunk_index,
                             int64_t offset,
                             int original_size,
                             int compressed_size,
                             uint32_t crc32,
                             const std::string& data_b64);

std::string build_transfer_complete(const std::string& transfer_id,
                                     const std::string& sha256);

// ── قراءة حقول من JSON بسيط ─────────────────────────────────────────────
std::string json_get_str(const std::string& json, const std::string& key);
int64_t     json_get_int(const std::string& json, const std::string& key,
                          int64_t def = 0);
bool        json_get_bool(const std::string& json, const std::string& key,
                           bool def = false);

// ── Base64 ───────────────────────────────────────────────────────────────
std::string base64_encode(const uint8_t* data, size_t len);
std::vector<uint8_t> base64_decode(const std::string& b64);

// ── CRC32 (للتحقق من سلامة الـ chunk) ──────────────────────────────────
uint32_t crc32_compute(const uint8_t* data, size_t len);

} // namespace StonxProto

namespace StonxProto {
std::string build_transfer_init_ack(const std::string& transfer_id, int resume_from);
std::string build_chunk_ack(const std::string& transfer_id, int chunk_index);
std::string build_chunk_nack(const std::string& transfer_id, int chunk_index, const std::string& reason);
std::string build_transfer_complete_ack(const std::string& transfer_id, const std::string& status);
std::string build_manifest_request(const std::string& op_id, const std::string& folder_path);
} // namespace StonxProto
