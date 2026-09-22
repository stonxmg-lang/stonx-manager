#pragma once
#include <string>
#include <functional>
#include <atomic>

// ════════════════════════════════════════════════════════════════════════════
// VideoOps — تسجيل فيديو عبر Camera2 NDK + MediaCodec
// فيديو بصوت (Camera2 للصورة + AudioRecord للميكروفون + MediaCodec للترميز)
// ════════════════════════════════════════════════════════════════════════════

namespace VideoOps {

enum class VideoFacing { FRONT, BACK };
enum class RecordResult { OK, NO_CAMERA, PERMISSION_DENIED, RECORD_FAILED };

using RecordCallback = std::function<void(RecordResult result,
                                          const std::string& mp4_path)>;

// تسجيل فيديو
// - duration_sec: مدة التسجيل بالثواني
// - الفيديو يُحفظ في tmp_dir مؤقتاً
// - cancel: يُوضع true من الخارج لإيقاف التسجيل مبكراً
// - الـ callback يُستدعى عند الانتهاء (success أو cancel أو error)
void record_video(VideoFacing facing,
                  int duration_sec,
                  const std::string& tmp_dir,
                  const std::atomic<bool>& cancel,
                  RecordCallback callback);

} // namespace VideoOps
