#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ════════════════════════════════════════════════════════════════════════════
// FileOps — عمليات نظام الملفات على الجهاز
// جميع المسارات يجب أن تكون ضمن /storage/emulated/0 (أو PATH_ROOT)
// ════════════════════════════════════════════════════════════════════════════

namespace FileOps {

struct Entry {
    std::string name;
    bool        is_dir{false};
    int64_t     size{0};
    int64_t     mtime_sec{0};
    std::string sha256;      // فارغ لو is_dir أو لم يُحسَب
};

struct ListResult {
    std::vector<Entry> entries;
    int64_t            total_count{0};
};

// ── حلّ المسار (Path Resolution) ───────────────────────────────────────
// يحوّل مساراً نسبياً أو مختصراً إلى مسار مطلق صحيح
//   "" أو "/"        → "/storage/emulated/0"
//   "/xyz"           → كما هو (يُرفض إن لم يكن داخل storage)
//   "storage/..."    → "/storage/..."
//   "sdcard/..."     → "/storage/emulated/0/..."
//   "/sdcard/..."    → "/storage/emulated/0/..."
//   "abc"            → "/storage/emulated/0/abc"
//   مسار مطلق مرفوض → "" (سلسلة فارغة)
std::string resolve_path(const std::string& input);

// ls — عرض محتوى مجلد مع SHA256 وحجم كل ملف
ListResult list_dir(const std::string& path);

// stat ملف أو مجلد
bool stat_entry(const std::string& path, Entry& out);

// حذف ملف
bool delete_file(const std::string& path, std::string& err);

// حذف مجلد (recursive)
bool delete_folder(const std::string& path, std::string& err);

// إعادة تسمية ملف أو مجلد
bool rename_entry(const std::string& old_path,
                  const std::string& new_path,
                  std::string& err);

// إنشاء مجلدات متداخلة (mkdir -p)
bool make_dirs(const std::string& path);

// SHA256 لملف كامل
std::string sha256_file(const std::string& path);

// هل المسار داخل /storage/emulated/0؟ (يقبل النسبي والمختصر)
bool is_safe_path(const std::string& path);

// بناء JSON entry للإرسال عبر البروتوكول
std::string entry_to_json(const Entry& e);

// بناء JSON قائمة entries
std::string list_to_json(const ListResult& r, const std::string& path);

} // namespace FileOps
