// ════════════════════════════════════════════════════════════════════════════
// pending_queue.cpp — المرحلة ⑥
// طابور الملفات المعلقة (صور/فيديو لم تُرسَل لانقطاع الاتصال)
// ════════════════════════════════════════════════════════════════════════════
#include "pending_queue.hpp"
#include <android/log.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <fstream>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "STONX_PQ", __VA_ARGS__)

namespace PendingQueue {

// ── مسارات ─────────────────────────────────────────────────────────────────
static std::string meta_path(const std::string& dir, const std::string& id) {
    return dir + "/" + id + ".meta";
}
static std::string data_path(const std::string& dir, const std::string& id,
                              const std::string& type) {
    std::string ext = (type == "video") ? ".mp4" : ".jpg";
    return dir + "/" + id + ext;
}

// ── توليد ID فريد ─────────────────────────────────────────────────────────
static std::string new_id() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "pq-" + std::to_string(ms);
}

// ── حفظ metadata ──────────────────────────────────────────────────────────
static bool save_meta(const std::string& dir, const PendingItem& item) {
    mkdir(dir.c_str(), 0755);
    FILE* f = fopen(meta_path(dir, item.id).c_str(), "w");
    if (!f) return false;
    fprintf(f, "id=%s\n",           item.id.c_str());
    fprintf(f, "local_path=%s\n",   item.local_path.c_str());
    fprintf(f, "remote_name=%s\n",  item.remote_name.c_str());
    fprintf(f, "sha256=%s\n",       item.sha256.c_str());
    fprintf(f, "size=%lld\n",       (long long)item.size);
    fprintf(f, "timestamp=%lld\n",  (long long)item.timestamp);
    fprintf(f, "type=%s\n",         item.type.c_str());
    fclose(f);
    return true;
}

static bool load_meta(const std::string& meta_file, PendingItem& out) {
    std::ifstream ifs(meta_file);
    if (!ifs) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if      (k=="id")          out.id          = v;
        else if (k=="local_path")  out.local_path  = v;
        else if (k=="remote_name") out.remote_name = v;
        else if (k=="sha256")      out.sha256      = v;
        else if (k=="size")        out.size        = std::stoll(v);
        else if (k=="timestamp")   out.timestamp   = std::stoll(v);
        else if (k=="type")        out.type        = v;
    }
    return !out.id.empty();
}

// ── enqueue: نسخ الملف إلى pending_dir ────────────────────────────────────
std::string enqueue(const std::string& pending_dir,
                    const std::string& src_path,
                    const std::string& remote_name,
                    const std::string& type) {
    mkdir(pending_dir.c_str(), 0755);
    std::string id  = new_id();
    std::string dst = data_path(pending_dir, id, type);

    // نسخ الملف
    FILE* src = fopen(src_path.c_str(), "rb");
    if (!src) return "";
    FILE* dstf = fopen(dst.c_str(), "wb");
    if (!dstf) { fclose(src); return ""; }
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dstf);
    fclose(src);
    fclose(dstf);

    // حجم الملف
    struct stat st{};
    stat(dst.c_str(), &st);

    PendingItem item;
    item.id          = id;
    item.local_path  = dst;
    item.remote_name = remote_name;
    item.size        = st.st_size;
    item.timestamp   = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    item.type        = type;
    save_meta(pending_dir, item);

    LOGI("enqueued: %s → %s", src_path.c_str(), dst.c_str());
    return id;
}

// ── move_to_queue: نقل (أسرع من نسخ) ─────────────────────────────────────
std::string move_to_queue(const std::string& pending_dir,
                           const std::string& src_path,
                           const std::string& remote_name,
                           const std::string& type) {
    mkdir(pending_dir.c_str(), 0755);
    std::string id  = new_id();
    std::string dst = data_path(pending_dir, id, type);

    // حاول rename أولاً (أسرع)
    if (rename(src_path.c_str(), dst.c_str()) != 0) {
        // إذا فشل (عبر أجهزة مختلفة) ارجع إلى نسخ
        return enqueue(pending_dir, src_path, remote_name, type);
    }

    struct stat st{};
    stat(dst.c_str(), &st);

    PendingItem item;
    item.id          = id;
    item.local_path  = dst;
    item.remote_name = remote_name;
    item.size        = st.st_size;
    item.timestamp   = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    item.type        = type;
    save_meta(pending_dir, item);

    LOGI("moved to queue: %s (id=%s)", remote_name.c_str(), id.c_str());
    return id;
}

// ── list_pending ────────────────────────────────────────────────────────────
std::vector<PendingItem> list_pending(const std::string& pending_dir) {
    std::vector<PendingItem> result;
    DIR* d = opendir(pending_dir.c_str());
    if (!d) return result;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() < 5 || name.substr(name.size()-5) != ".meta") continue;
        PendingItem item;
        if (load_meta(pending_dir + "/" + name, item)) {
            result.push_back(std::move(item));
        }
    }
    closedir(d);

    // ترتيب: الأقدم أولاً
    std::sort(result.begin(), result.end(),
              [](const PendingItem& a, const PendingItem& b) {
                  return a.timestamp < b.timestamp;
              });
    return result;
}

int pending_count(const std::string& pending_dir) {
    return (int)list_pending(pending_dir).size();
}

// ── remove_item: حذف البيانات والـ metadata ──────────────────────────────
void remove_item(const std::string& pending_dir, const std::string& id) {
    // حذف الـ meta أولاً
    unlink(meta_path(pending_dir, id).c_str());

    // حذف ملف البيانات (jpg أو mp4)
    for (const char* ext : {".jpg", ".mp4"}) {
        std::string p = pending_dir + "/" + id + ext;
        unlink(p.c_str());
    }
    LOGI("removed pending item: %s", id.c_str());
}

} // namespace PendingQueue
