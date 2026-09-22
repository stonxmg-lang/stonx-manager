#pragma once
#include <string>
#include <vector>

// ════════════════════════════════════════════════════════════════════════════
// PendingQueue — طابور الملفات المعلقة
//
// عند انقطاع الاتصال أثناء إرسال صورة/فيديو:
//   1. الملف يُحفظ في PENDING_DIR (private storage، غير مرئي للمستخدم)
//   2. إشعار "⏳ ملفات معلقة — انتظار الاتصال" يظهر
//   3. عند إعادة الاتصال: Queue يُرسَل تلقائياً chunk by chunk
//   4. بعد تأكيد الاستلام (SHA256 ✓): الملف يُحذَف من PENDING_DIR
// ════════════════════════════════════════════════════════════════════════════

namespace PendingQueue {

struct PendingItem {
    std::string id;            // معرف فريد
    std::string local_path;    // المسار في PENDING_DIR
    std::string remote_name;   // اسم الملف على Controller
    std::string sha256;        // للتحقق بعد الإرسال
    int64_t     size{0};
    int64_t     timestamp{0};  // وقت الإضافة (unix epoch)
    std::string type;          // "photo" أو "video"
};

// إضافة ملف للطابور (ينسخ الملف إلى pending_dir)
// يُعيد الـ item id للمتابعة
std::string enqueue(const std::string& pending_dir,
                    const std::string& src_path,
                    const std::string& remote_name,
                    const std::string& type);

// عرض كل الملفات المعلقة
std::vector<PendingItem> list_pending(const std::string& pending_dir);

// عدد الملفات المعلقة
int pending_count(const std::string& pending_dir);

// حذف item بعد الإرسال الناجح
void remove_item(const std::string& pending_dir, const std::string& id);

// حذف ملف src ونقله للطابور في خطوة واحدة (move — أسرع من نسخ)
std::string move_to_queue(const std::string& pending_dir,
                           const std::string& src_path,
                           const std::string& remote_name,
                           const std::string& type);

} // namespace PendingQueue
