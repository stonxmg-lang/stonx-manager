#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// ════════════════════════════════════════════════════════════════════════════
// TransferEngine — محرك النقل الاحترافي
//
// الميزات:
//   • Adaptive chunking: حجم chunk يتكيف تلقائياً مع سرعة الشبكة
//   • Sliding window:    N chunks طائرة بدون انتظار ACK واحد تلو الآخر
//   • zlib compression: ضغط تكيّفي لكل chunk (يُتجاهل لو لم يُفد)
//   • CRC32 per chunk:  تحقق من سلامة كل chunk فور وصوله
//   • Resume:           يستكمل من آخر chunk مؤكد لو انقطع الاتصال
// ════════════════════════════════════════════════════════════════════════════

// ── progress callback ──────────────────────────────────────────────────────
struct TransferProgress {
    int64_t bytes_sent;     // bytes مؤكدة وصلت
    int64_t total_bytes;    // إجمالي bytes
    int     chunks_done;
    int     chunks_total;
    double  speed_bytes_sec;
    int     window_size;    // window الحالي
    int     chunk_size;     // chunk_size الحالي
};
using ProgressCb = std::function<void(const TransferProgress&)>;

// ── نتيجة النقل ────────────────────────────────────────────────────────────
enum class TransferResult {
    OK,
    CANCELLED,
    DISCONNECTED,
    IO_ERROR,
    INTEGRITY_ERROR,   // SHA256 النهائي لم يتطابق
    CHUNK_MAX_RETRIES  // chunk فشل أكثر من الحد المسموح
};

// ── Send: إرسال ملف من Agent → Controller ──────────────────────────────────
//
// الـ fd يجب أن يكون متصلاً ومُصادقاً.
// يُرسَل TRANSFER_INIT أولاً، ثم chunks مع sliding window.
// يستقبل ACK/NACK لكل chunk ويتكيف.
// عند الانتهاء يُرسَل TRANSFER_COMPLETE ثم ينتظر TRANSFER_COMPLETE_ACK.
// progress_file: مسار ملف الـ progress (للاستئناف) — "" = لا استئناف.
//
TransferResult transfer_send(
    int          fd,
    const std::string& transfer_id,
    const std::string& local_path,    // مسار الملف على الجهاز
    const std::string& remote_name,   // اسم الملف كما سيُعرَض على Controller
    const std::string& progress_dir,  // مجلد حفظ ملفات الـ progress
    const std::atomic<bool>& cancel,  // مشترك مع الـ caller لإلغاء النقل
    ProgressCb   on_progress = nullptr
);

// ── Receive: استقبال ملف من Controller → Agent ─────────────────────────────
//
// ينتظر TRANSFER_INIT ثم يستقبل chunks ويكتبها على local_path.part
// عند اكتمال SHA256 يُعاد تسمية الملف لإزالة .part
//
TransferResult transfer_recv(
    int          fd,
    const std::string& local_path,    // المسار النهائي للملف
    const std::string& progress_dir,
    const std::atomic<bool>& cancel,
    ProgressCb   on_progress = nullptr
);

// ── Compression helpers (zlib) ─────────────────────────────────────────────
bool   zlib_compress(const uint8_t* src, size_t src_len,
                     std::vector<uint8_t>& dst);
bool   zlib_decompress(const uint8_t* src, size_t src_len,
                       std::vector<uint8_t>& dst, size_t max_out);

// ── Progress persistence ───────────────────────────────────────────────────
struct ProgressRecord {
    std::string transfer_id;
    std::string remote_name;
    int64_t     total_bytes{0};
    int         total_chunks{0};
    int         chunk_size{0};
    int         last_confirmed_chunk{-1}; // -1 = لم يُؤكَّد شيء بعد
    int64_t     last_confirmed_offset{0};
    std::string sha256;
};

bool   progress_save(const std::string& dir, const ProgressRecord& rec);
bool   progress_load(const std::string& dir,
                     const std::string& transfer_id,
                     ProgressRecord& out);
void   progress_delete(const std::string& dir, const std::string& transfer_id);
