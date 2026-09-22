// ════════════════════════════════════════════════════════════════════════════
// file_ops.cpp — المرحلة ⑤
// ls · sha256 · delete · rename · make_dirs · JSON builders
// ════════════════════════════════════════════════════════════════════════════
#include "file_ops.hpp"
#include "crypto/sha256.hpp"
#include "config.hpp"
#include <android/log.h>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <stack>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "STONX_FS", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "STONX_FS", __VA_ARGS__)

namespace FileOps {

static constexpr const char* STORAGE_ROOT = "/storage/emulated/0";

// ── resolve_path ──────────────────────────────────────────────────────────
// القاعدة الذهبية: أي مسار "منطقي" للمستخدم يجب أن ينتهي تحت /storage/emulated/0
std::string resolve_path(const std::string& input) {
    // ① فارغ أو "/" → الجذر
    if (input.empty() || input == "/")
        return std::string(STORAGE_ROOT);

    // ② مسار مطلق داخل storage → كما هو
    if (input.rfind(STORAGE_ROOT, 0) == 0)
        return input;

    // ③ اختصارات /sdcard و sdcard
    if (input == "/sdcard" || input == "sdcard")
        return std::string(STORAGE_ROOT);
    if (input.rfind("/sdcard/", 0) == 0)
        return std::string(STORAGE_ROOT) + "/" + input.substr(8);
    if (input.rfind("sdcard/", 0) == 0)
        return std::string(STORAGE_ROOT) + "/" + input.substr(7);

    // ④ "storage/..." (بدون "/" البداية) → أضف "/"
    if (input.rfind("storage/", 0) == 0)
        return "/" + input;

    // ⑤ أي مسار مطلق آخر (مثل /data/ أو /system/) → مرفوض
    if (input[0] == '/')
        return "";

    // ⑥ مسار نسبي → ضعه تحت storage
    return std::string(STORAGE_ROOT) + "/" + input;
}

// ── أمان المسار ─────────────────────────────────────────────────────────
bool is_safe_path(const std::string& path) {
    std::string real = resolve_path(path);
    if (real.empty()) return false;
    if (real.find("..") != std::string::npos) return false;
    if (real.rfind(STORAGE_ROOT, 0) != 0) return false;
    return true;
}

// ── make_dirs ─────────────────────────────────────────────────────────────
bool make_dirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            mkdir(cur.c_str(), 0755);
        }
    }
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// ── SHA256 ملف ─────────────────────────────────────────────────────────────
std::string sha256_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";
    SHA256 h;
    uint8_t buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) h.update(buf, (size_t)n);
    close(fd);
    return SHA256::hex(h.digest());
}

// ── stat entry ──────────────────────────────────────────────────────────────
bool stat_entry(const std::string& path, Entry& out) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    size_t sl  = path.rfind('/');
    out.name   = (sl == std::string::npos) ? path : path.substr(sl + 1);
    out.is_dir = S_ISDIR(st.st_mode);
    out.size   = st.st_size;
    out.mtime_sec = (int64_t)st.st_mtime;
    return true;
}

// ── list_dir — يُعيد محتوى مجلد مع SHA256 ─────────────────────────────────
ListResult list_dir(const std::string& path) {
    ListResult res;
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        LOGE("opendir failed: %s  err=%s", path.c_str(), strerror(errno));
        return res;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        std::string full = path + "/" + ent->d_name;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;

        Entry e;
        e.name      = ent->d_name;
        e.is_dir    = S_ISDIR(st.st_mode);
        e.size      = S_ISREG(st.st_mode) ? st.st_size : 0;
        e.mtime_sec = (int64_t)st.st_mtime;

        // SHA256 للملفات فقط (المجلدات لا يُحسَب لها)
        if (!e.is_dir) {
            e.sha256 = sha256_file(full);
        }
        res.entries.push_back(std::move(e));
    }
    closedir(dir);

    // ترتيب: المجلدات أولاً ثم الملفات، وبداخل كل مجموعة ترتيب أبجدي
    std::sort(res.entries.begin(), res.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.name < b.name;
    });

    res.total_count = (int64_t)res.entries.size();
    return res;
}

// ── حذف ملف ────────────────────────────────────────────────────────────────
bool delete_file(const std::string& path, std::string& err) {
    if (!is_safe_path(path)) { err = "PATH_OUTSIDE_ROOT"; return false; }
    std::string real = resolve_path(path);
    if (unlink(real.c_str()) != 0) {
        err = strerror(errno);
        return false;
    }
    return true;
}

// ── حذف مجلد (recursive) ───────────────────────────────────────────────────
static bool rmdir_recursive(const std::string& path) {
    // BFS/DFS iterative لتجنب stack overflow على المجلدات العميقة
    std::stack<std::string> dirs;
    std::vector<std::string> dir_order;
    dirs.push(path);

    while (!dirs.empty()) {
        std::string cur = dirs.top(); dirs.pop();
        dir_order.push_back(cur);

        DIR* d = opendir(cur.c_str());
        if (!d) continue;

        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            std::string full = cur + "/" + ent->d_name;
            struct stat st{};
            if (lstat(full.c_str(), &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                dirs.push(full);
            } else {
                unlink(full.c_str());
            }
        }
        closedir(d);
    }

    // احذف المجلدات بالعكس (الأعمق أولاً)
    for (auto it = dir_order.rbegin(); it != dir_order.rend(); ++it) {
        rmdir(it->c_str());
    }
    return true;
}

bool delete_folder(const std::string& path, std::string& err) {
    if (!is_safe_path(path)) { err = "PATH_OUTSIDE_ROOT"; return false; }
    std::string real = resolve_path(path);
    if (!rmdir_recursive(real)) {
        err = strerror(errno);
        return false;
    }
    return true;
}

// ── إعادة تسمية ─────────────────────────────────────────────────────────────
bool rename_entry(const std::string& old_p, const std::string& new_p, std::string& err) {
    if (!is_safe_path(old_p) || !is_safe_path(new_p)) {
        err = "PATH_OUTSIDE_ROOT";
        return false;
    }
    std::string real_old = resolve_path(old_p);
    std::string real_new = resolve_path(new_p);
    if (rename(real_old.c_str(), real_new.c_str()) != 0) {
        err = strerror(errno);
        return false;
    }
    return true;
}

// ── JSON helpers ────────────────────────────────────────────────────────────
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c < 0x20)  { char buf[8]; snprintf(buf,8,"\\u%04x",c); out+=buf; }
        else                out += (char)c;
    }
    return out;
}

std::string entry_to_json(const Entry& e) {
    return "{\"name\":\""  + json_escape(e.name) + "\","
           "\"type\":\""   + (e.is_dir ? "dir" : "file") + "\","
           "\"size\":"     + std::to_string(e.size) + ","
           "\"mtime\":"    + std::to_string(e.mtime_sec) + ","
           "\"sha256\":\"" + e.sha256 + "\"}";
}

std::string list_to_json(const ListResult& r, const std::string& path) {
    std::string s;
    s.reserve(256 + r.entries.size() * 128);
    s += "{\"path\":\"";
    s += json_escape(path);
    s += "\",\"total\":";
    s += std::to_string(r.total_count);
    s += ",\"entries\":[";
    for (size_t i = 0; i < r.entries.size(); ++i) {
        if (i) s += ',';
        s += entry_to_json(r.entries[i]);
    }
    s += "]}";
    return s;
}

} // namespace FileOps
