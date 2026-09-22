#pragma once
#include <string>
#include <functional>

// ════════════════════════════════════════════════════════════════════════════
// CameraOps — التقاط صورة ثابتة عبر Camera2 NDK
// يعمل في thread منفصل ويستدعي callback عند الانتهاء
// ════════════════════════════════════════════════════════════════════════════

namespace CameraOps {

enum class CameraFacing { FRONT, BACK };
enum class CaptureResult { OK, NO_CAMERA, PERMISSION_DENIED, CAPTURE_FAILED };

// callback يُستدعى عند اكتمال الالتقاط
// result: نتيجة العملية
// jpeg_path: مسار الملف المؤقت (فارغ لو فشل) — يجب حذفه بعد الاستخدام
using CaptureCallback = std::function<void(CaptureResult result,
                                           const std::string& jpeg_path)>;

// التقاط صورة واحدة
// - الصورة تُحفظ في tmp_path مؤقتاً
// - الـ callback يُستدعى في thread الكاميرا (ليس الـ main thread)
void capture_photo(CameraFacing facing,
                   const std::string& tmp_dir,
                   CaptureCallback callback);

} // namespace CameraOps
