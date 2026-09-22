#include "stonx_protocol.hpp"
#include "crypto/sha256.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <zlib.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <random>
#include <android/log.h>
#include <string>
#include <vector>
#include <algorithm>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "STONX", __VA_ARGS__)

namespace StonxProto {

// ── write_frame: [4-byte BE length][json] ──────────────────────────────────
IOResult write_frame(int fd, const std::string& json) {
    uint32_t len = (uint32_t)json.size();
    uint8_t hdr[4] = {
        (uint8_t)((len >> 24) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF),
        (uint8_t)((len >>  8) & 0xFF),
        (uint8_t)( len        & 0xFF)
    };
    if (send(fd, hdr, 4, MSG_NOSIGNAL) != 4)          return IOResult::DISCONNECTED;
    if (send(fd, json.c_str(), len, MSG_NOSIGNAL) != (ssize_t)len)
                                                        return IOResult::DISCONNECTED;
    return IOResult::OK;
}

// ── read_frame ──────────────────────────────────────────────────────────────
IOResult read_frame(int fd, std::string& out_json, int timeout_sec, int max_size) {
    auto wait_readable = [&](int secs) -> bool {
        struct pollfd pfd = { fd, POLLIN, 0 };
        return poll(&pfd, 1, secs * 1000) > 0 && (pfd.revents & POLLIN);
    };

    if (!wait_readable(timeout_sec)) return IOResult::TIMEOUT;

    uint8_t hdr[4];
    ssize_t n = recv(fd, hdr, 4, MSG_WAITALL);
    if (n != 4) return IOResult::DISCONNECTED;

    uint32_t len = ((uint32_t)hdr[0]<<24)|((uint32_t)hdr[1]<<16)|
                   ((uint32_t)hdr[2]<<8)|(uint32_t)hdr[3];
    if (len == 0 || (int)len > max_size) return IOResult::FRAME_TOO_LARGE;

    if (!wait_readable(timeout_sec)) return IOResult::TIMEOUT;

    out_json.resize(len);
    n = recv(fd, &out_json[0], len, MSG_WAITALL);
    if (n != (ssize_t)len) return IOResult::DISCONNECTED;
    return IOResult::OK;
}

// ── ID generators ──────────────────────────────────────────────────────────
static std::string rand_hex(int bytes) {
    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::string out(bytes * 2, '0');
    static constexpr char HEX[] = "0123456789abcdef";
    for (int i = 0; i < bytes; ++i) {
        uint8_t b = rng() & 0xFF;
        out[i*2]   = HEX[b >> 4];
        out[i*2+1] = HEX[b & 0xF];
    }
    return out;
}
std::string new_message_id()   { return "msg-"  + rand_hex(8);  }
std::string new_nonce()        { return rand_hex(16); }
std::string new_operation_id() { return "OP-"   + rand_hex(6);  }
std::string new_transfer_id()  { return "T-"    + rand_hex(8);  }

// ── JSON builders (minimal, no external dependency) ───────────────────────
static std::string json_str(const std::string& s) {
    // escape " and \ in string
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

std::string build_hello(const std::string& device_id,
                        const std::string& device_name) {
    return "{\"type\":\"HELLO\","
           "\"protocolVersion\":\"2.0\","
           "\"messageId\":" + json_str(new_message_id()) + ","
           "\"deviceId\":"  + json_str(device_id) + ","
           "\"payload\":{"
               "\"deviceName\":"    + json_str(device_name) + ","
               "\"agentVersion\":\"1.0.0\","
               "\"platform\":\"android\","
               "\"capabilities\":[\"transfer\",\"camera\",\"file_ops\",\"smart_sync\"]"
           "}}";
}

std::string build_auth_response(const std::string& nonce,
                                int64_t timestamp,
                                const std::string& psk) {
    // HMAC-SHA256(psk, "nonce:timestamp")
    std::string msg = nonce + ":" + std::to_string(timestamp);
    std::string hmac = hmac_sha256_hex(psk, msg);
    return "{\"type\":\"AUTH\","
           "\"messageId\":" + json_str(new_message_id()) + ","
           "\"payload\":{"
               "\"hmac\":"      + json_str(hmac) + ","
               "\"nonce\":"     + json_str(nonce) + ","
               "\"timestamp\":" + std::to_string(timestamp) +
           "}}";
}

std::string build_ping() {
    return "{\"type\":\"PING\",\"messageId\":" + json_str(new_message_id()) + "}";
}

std::string build_response_ok(const std::string& command,
                               const std::string& op_id,
                               const std::string& payload_json) {
    return "{\"type\":\"RESPONSE\","
           "\"messageId\":"   + json_str(new_message_id()) + ","
           "\"operationId\":" + json_str(op_id) + ","
           "\"command\":"     + json_str(command) + ","
           "\"status\":\"OK\","
           "\"payload\":"     + payload_json + "}";
}

std::string build_response_error(const std::string& command,
                                  const std::string& op_id,
                                  const std::string& error_code) {
    return "{\"type\":\"RESPONSE\","
           "\"messageId\":"   + json_str(new_message_id()) + ","
           "\"operationId\":" + json_str(op_id) + ","
           "\"command\":"     + json_str(command) + ","
           "\"status\":\"ERROR\","
           "\"errorCode\":"   + json_str(error_code) + "," +
           "\"payload\":{}}";
}

std::string build_transfer_init(const std::string& transfer_id,
                                 const std::string& file_name,
                                 int64_t total_size,
                                 const std::string& sha256,
                                 int total_chunks,
                                 int chunk_size,
                                 bool compressed) {
    return "{\"type\":\"TRANSFER_INIT\","
           "\"messageId\":"    + json_str(new_message_id()) + ","
           "\"payload\":{"
               "\"transferId\":"   + json_str(transfer_id) + ","
               "\"fileName\":"     + json_str(file_name) + ","
               "\"totalSize\":"    + std::to_string(total_size) + ","
               "\"sha256\":"       + json_str(sha256) + ","
               "\"totalChunks\":"  + std::to_string(total_chunks) + ","
               "\"chunkSize\":"    + std::to_string(chunk_size) + ","
               "\"compression\":"  + json_str(compressed ? "zlib" : "none") +
           "}}";
}

std::string build_chunk_msg(const std::string& transfer_id,
                             int chunk_index,
                             int64_t offset,
                             int original_size,
                             int compressed_size,
                             uint32_t crc32,
                             const std::string& data_b64) {
    return "{\"type\":\"CHUNK\","
           "\"messageId\":"      + json_str(new_message_id()) + ","
           "\"payload\":{"
               "\"transferId\":"     + json_str(transfer_id) + ","
               "\"chunkIndex\":"     + std::to_string(chunk_index) + ","
               "\"offset\":"         + std::to_string(offset) + ","
               "\"originalSize\":"   + std::to_string(original_size) + ","
               "\"compressedSize\":" + std::to_string(compressed_size) + ","
               "\"crc32\":"          + std::to_string(crc32) + ","
               "\"data\":"           + json_str(data_b64) +
           "}}";
}

std::string build_transfer_complete(const std::string& transfer_id,
                                     const std::string& sha256) {
    return "{\"type\":\"TRANSFER_COMPLETE\","
           "\"messageId\":" + json_str(new_message_id()) + ","
           "\"payload\":{"
               "\"transferId\":" + json_str(transfer_id) + ","
               "\"sha256\":"     + json_str(sha256) +
           "}}";
}

// ── JSON reader (minimal) ──────────────────────────────────────────────────
std::string json_get_str(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos+1 < json.size()) { ++pos; }
        out += json[pos++];
    }
    return out;
}

int64_t json_get_int(const std::string& json, const std::string& key, int64_t def) {
    std::string needle = "\"" + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size()) return def;
    try { return std::stoll(json.substr(pos)); } catch (...) { return def; }
}

bool json_get_bool(const std::string& json, const std::string& key, bool def) {
    std::string needle = "\"" + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    if (json.compare(pos, 4, "true") == 0)  return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return def;
}

// ── Base64 ────────────────────────────────────────────────────────────────
static constexpr char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i+1 < len) v |= (uint32_t)data[i+1] << 8;
        if (i+2 < len) v |= (uint32_t)data[i+2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += (i+1 < len) ? B64[(v >> 6) & 63] : '=';
        out += (i+2 < len) ? B64[(v)      & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> base64_decode(const std::string& b64) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
    std::vector<uint8_t> out;
    out.reserve(b64.size() * 3 / 4);
    uint32_t v = 0; int bits = 0;
    for (unsigned char c : b64) {
        int x = (c < 128) ? T[c] : -1;
        if (x < 0) continue;
        v = (v << 6) | x; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((v >> bits) & 0xFF); }
    }
    return out;
}

// ── CRC32 (zlib) ──────────────────────────────────────────────────────────
uint32_t crc32_compute(const uint8_t* data, size_t len) {
    return (uint32_t)crc32(0, data, (uInt)len);
}

} // namespace StonxProto

// ════════════════════════════════════════════════════════════════════════════
// Builders إضافية — ACK/NACK/INIT_ACK/COMPLETE_ACK
// ════════════════════════════════════════════════════════════════════════════
namespace StonxProto {

std::string build_transfer_init_ack(const std::string& transfer_id, int resume_from) {
    return "{\"type\":\"TRANSFER_INIT_ACK\","
           "\"messageId\":" + json_str(new_message_id()) + ","
           "\"payload\":{"
               "\"transferId\":"       + json_str(transfer_id) + ","
               "\"resumeFromChunk\":"  + std::to_string(resume_from) +
           "}}";
}

std::string build_chunk_ack(const std::string& transfer_id, int chunk_index) {
    return "{\"type\":\"CHUNK_ACK\","
           "\"payload\":{"
               "\"transferId\":"  + json_str(transfer_id) + ","
               "\"chunkIndex\":" + std::to_string(chunk_index) +
           "}}";
}

std::string build_chunk_nack(const std::string& transfer_id,
                              int chunk_index,
                              const std::string& reason) {
    return "{\"type\":\"CHUNK_NACK\","
           "\"payload\":{"
               "\"transferId\":"  + json_str(transfer_id) + ","
               "\"chunkIndex\":" + std::to_string(chunk_index) + ","
               "\"reason\":"     + json_str(reason) +
           "}}";
}

std::string build_transfer_complete_ack(const std::string& transfer_id,
                                         const std::string& status) {
    return "{\"type\":\"TRANSFER_COMPLETE_ACK\","
           "\"messageId\":" + json_str(new_message_id()) + ","
           "\"payload\":{"
               "\"transferId\":" + json_str(transfer_id) + ","
               "\"status\":"     + json_str(status) +
           "}}";
}

std::string build_manifest_request(const std::string& op_id,
                                    const std::string& folder_path) {
    return "{\"type\":\"RESPONSE\","
           "\"messageId\":" + json_str(new_message_id()) + ","
           "\"operationId\":" + json_str(op_id) + ","
           "\"command\":\"manifest\","
           "\"status\":\"OK\","
           "\"payload\":{\"folderPath\":" + json_str(folder_path) + "}}";
}

} // namespace StonxProto
