#pragma once
#include <string>
#include <vector>
#include "file_ops.hpp"

// ════════════════════════════════════════════════════════════════════════════
// SmartSync — مزامنة ذكية للمجلدات
//
// المنطق:
//   • Controller يطلب sync لمسار معين
//   • Agent يبني قائمة كل ملفاته (اسم + SHA256)
//   • يُرسَل هذا المانيفست للـ Controller
//   • Controller يقارنه بما عنده ويطلب الملفات الجديدة/المتغيرة فقط
//   • لكل ملف بنفس الاسم وSHA256 مختلف:
//       - Controller يُعيد تسمية نسخته القديمة: file_2026-09-15_14-30.ext
//       - يُنزَّل الملف الجديد باسمه الأصلي
// ════════════════════════════════════════════════════════════════════════════

namespace SmartSync {

struct FileInfo {
    std::string rel_path;   // المسار النسبي من جذر المجلد
    std::string sha256;
    int64_t     size{0};
    int64_t     mtime_sec{0};
};

// بناء مانيفست كامل لمجلد (recursive)
std::vector<FileInfo> build_manifest(const std::string& folder_path);

// تحويل مانيفست إلى JSON لإرساله عبر البروتوكول
std::string manifest_to_json(const std::vector<FileInfo>& manifest,
                              const std::string& folder_path);

// ضغط مجلد كامل إلى zip (للنقل الأول)
bool zip_folder(const std::string& src_dir,
                const std::string& dst_zip,
                std::string& err);

// فك ضغط zip إلى مجلد
bool unzip_folder(const std::string& src_zip,
                  const std::string& dst_dir,
                  std::string& err);

} // namespace SmartSync
