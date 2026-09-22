// ════════════════════════════════════════════════════════════════════════════
// smart_sync.cpp — المرحلة ⑤
// manifest builder · zip/unzip باستخدام zlib + minizip-style
// ════════════════════════════════════════════════════════════════════════════
#include "smart_sync.hpp"
#include "file_ops.hpp"
#include "crypto/sha256.hpp"
#include <android/log.h>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
#include <cstring>
#include <ctime>
#include <stack>
#include <fstream>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "STONX_SYNC", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "STONX_SYNC", __VA_ARGS__)

namespace SmartSync {

// ── بناء المانيفست (recursive) ─────────────────────────────────────────────
static void scan_recursive(const std::string& root, const std::string& rel,
                            std::vector<FileInfo>& out) {
    std::string abs_path = root + (rel.empty() ? "" : "/" + rel);
    DIR* d = opendir(abs_path.c_str());
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        std::string child_rel  = rel.empty() ? ent->d_name : rel + "/" + ent->d_name;
        std::string child_abs  = root + "/" + child_rel;

        struct stat st{};
        if (lstat(child_abs.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_recursive(root, child_rel, out);
        } else if (S_ISREG(st.st_mode)) {
            FileInfo fi;
            fi.rel_path  = child_rel;
            fi.size      = st.st_size;
            fi.mtime_sec = (int64_t)st.st_mtime;
            fi.sha256    = FileOps::sha256_file(child_abs);
            out.push_back(std::move(fi));
        }
    }
    closedir(d);
}

std::vector<FileInfo> build_manifest(const std::string& folder_path) {
    std::vector<FileInfo> result;
    scan_recursive(folder_path, "", result);
    // ترتيب أبجدي بالمسار النسبي
    std::sort(result.begin(), result.end(),
              [](const FileInfo& a, const FileInfo& b) { return a.rel_path < b.rel_path; });
    LOGI("manifest built: %zu files in %s", result.size(), folder_path.c_str());
    return result;
}

std::string manifest_to_json(const std::vector<FileInfo>& manifest,
                              const std::string& folder_path) {
    std::string s;
    s.reserve(256 + manifest.size() * 200);
    s += "{\"folderPath\":\"";
    s += folder_path;
    s += "\",\"count\":";
    s += std::to_string(manifest.size());
    s += ",\"files\":[";
    for (size_t i = 0; i < manifest.size(); ++i) {
        if (i) s += ',';
        s += "{\"path\":\"" + manifest[i].rel_path + "\","
              "\"sha256\":\""  + manifest[i].sha256  + "\","
              "\"size\":"      + std::to_string(manifest[i].size) + ","
              "\"mtime\":"     + std::to_string(manifest[i].mtime_sec) + "}";
    }
    s += "]}";
    return s;
}

// ══════════════════════════════════════════════════════════════════════════
// ZIP writer — بسيط (DEFLATE + Local/Central directory + EOCD)
// يتعامل مع الملفات العادية فقط (لا symlinks، لا special files)
// ══════════════════════════════════════════════════════════════════════════
static void write_le16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF);
}
static void write_le32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF); buf.push_back((v >> 24) & 0xFF);
}

struct ZipEntry {
    std::string name;
    uint32_t crc32_val;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
    uint16_t compression; // 0=Store, 8=Deflate
    uint16_t last_mod_time;
    uint16_t last_mod_date;
};

static void dos_time(int64_t mtime_sec, uint16_t& t, uint16_t& d) {
    struct tm* tm_p = localtime((const time_t*)&mtime_sec);
    if (!tm_p) { t = 0; d = 0; return; }
    t = (uint16_t)(((tm_p->tm_hour) << 11) | ((tm_p->tm_min) << 5) | (tm_p->tm_sec / 2));
    d = (uint16_t)((((tm_p->tm_year + 1900 - 1980) & 0x7F) << 9)
                 | (((tm_p->tm_mon + 1) & 0x0F) << 5)
                 | (tm_p->tm_mday & 0x1F));
}

static bool deflate_file(const std::string& path,
                          std::vector<uint8_t>& out_comp,
                          uint32_t& crc_out,
                          uint32_t& orig_size_out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    z_stream zs{};
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);

    uint8_t in_buf[65536];
    uint8_t out_buf[65536];
    crc_out = crc32(0, nullptr, 0);
    orig_size_out = 0;

    ssize_t n;
    while ((n = read(fd, in_buf, sizeof(in_buf))) > 0) {
        crc_out = crc32(crc_out, in_buf, (uInt)n);
        orig_size_out += (uint32_t)n;
        zs.next_in  = in_buf;
        zs.avail_in = (uInt)n;
        do {
            zs.next_out  = out_buf;
            zs.avail_out = sizeof(out_buf);
            deflate(&zs, Z_NO_FLUSH);
            size_t produced = sizeof(out_buf) - zs.avail_out;
            out_comp.insert(out_comp.end(), out_buf, out_buf + produced);
        } while (zs.avail_in > 0);
    }
    // flush
    zs.avail_in = 0;
    int rc;
    do {
        zs.next_out  = out_buf;
        zs.avail_out = sizeof(out_buf);
        rc = deflate(&zs, Z_FINISH);
        size_t produced = sizeof(out_buf) - zs.avail_out;
        out_comp.insert(out_comp.end(), out_buf, out_buf + produced);
    } while (rc != Z_STREAM_END);
    deflateEnd(&zs);
    close(fd);
    return true;
}

bool zip_folder(const std::string& src_dir, const std::string& dst_zip, std::string& err) {
    // بناء قائمة الملفات
    std::vector<FileInfo> files = build_manifest(src_dir);

    FILE* zf = fopen(dst_zip.c_str(), "wb");
    if (!zf) { err = strerror(errno); return false; }

    std::vector<ZipEntry> entries;
    entries.reserve(files.size());

    for (const auto& fi : files) {
        std::string abs = src_dir + "/" + fi.rel_path;
        struct stat st{};
        if (stat(abs.c_str(), &st) != 0) continue;

        ZipEntry ze;
        ze.name              = fi.rel_path;
        ze.local_header_offset = (uint32_t)ftello(zf);
        dos_time(fi.mtime_sec, ze.last_mod_time, ze.last_mod_date);

        // Deflate
        std::vector<uint8_t> comp;
        uint32_t crc_v = 0, orig_size = 0;
        bool ok = deflate_file(abs, comp, crc_v, orig_size);
        if (!ok) continue;

        // إذا كان الضغط لم يُفد، استخدم Store
        bool use_deflate = comp.size() < orig_size;
        ze.compression      = use_deflate ? 8 : 0;
        ze.crc32_val        = crc_v;
        ze.uncompressed_size = orig_size;
        ze.compressed_size   = use_deflate ? (uint32_t)comp.size() : orig_size;

        // ── Local File Header ───────────────────────────────────────────
        std::vector<uint8_t> hdr;
        // Signature
        hdr.push_back(0x50); hdr.push_back(0x4B); hdr.push_back(0x03); hdr.push_back(0x04);
        write_le16(hdr, 20);                    // version needed
        write_le16(hdr, 0);                     // flags
        write_le16(hdr, ze.compression);
        write_le16(hdr, ze.last_mod_time);
        write_le16(hdr, ze.last_mod_date);
        write_le32(hdr, ze.crc32_val);
        write_le32(hdr, ze.compressed_size);
        write_le32(hdr, ze.uncompressed_size);
        write_le16(hdr, (uint16_t)ze.name.size());
        write_le16(hdr, 0);                     // extra length
        fwrite(hdr.data(), 1, hdr.size(), zf);
        fwrite(ze.name.c_str(), 1, ze.name.size(), zf);

        // ── File data ───────────────────────────────────────────────────
        if (use_deflate) {
            fwrite(comp.data(), 1, comp.size(), zf);
        } else {
            // Store: اقرأ الملف الأصلي واكتبه مباشرة
            int fd = open(abs.c_str(), O_RDONLY);
            if (fd >= 0) {
                uint8_t buf[65536];
                ssize_t n;
                while ((n = read(fd, buf, sizeof(buf))) > 0) fwrite(buf, 1, n, zf);
                close(fd);
            }
        }

        entries.push_back(ze);
    }

    // ── Central Directory ──────────────────────────────────────────────────
    uint32_t cd_offset = (uint32_t)ftello(zf);
    for (const auto& ze : entries) {
        std::vector<uint8_t> cdr;
        // Signature
        cdr.push_back(0x50); cdr.push_back(0x4B); cdr.push_back(0x01); cdr.push_back(0x02);
        write_le16(cdr, 20);                    // version made by
        write_le16(cdr, 20);                    // version needed
        write_le16(cdr, 0);                     // flags
        write_le16(cdr, ze.compression);
        write_le16(cdr, ze.last_mod_time);
        write_le16(cdr, ze.last_mod_date);
        write_le32(cdr, ze.crc32_val);
        write_le32(cdr, ze.compressed_size);
        write_le32(cdr, ze.uncompressed_size);
        write_le16(cdr, (uint16_t)ze.name.size());
        write_le16(cdr, 0); write_le16(cdr, 0); // extra, comment
        write_le16(cdr, 0);                     // disk
        write_le16(cdr, 0);                     // int attrib
        write_le32(cdr, 0);                     // ext attrib
        write_le32(cdr, ze.local_header_offset);
        fwrite(cdr.data(), 1, cdr.size(), zf);
        fwrite(ze.name.c_str(), 1, ze.name.size(), zf);
    }
    uint32_t cd_size = (uint32_t)ftello(zf) - cd_offset;

    // ── EOCD ─────────────────────────────────────────────────────────────
    std::vector<uint8_t> eocd;
    eocd.push_back(0x50); eocd.push_back(0x4B); eocd.push_back(0x05); eocd.push_back(0x06);
    write_le16(eocd, 0); write_le16(eocd, 0);            // disk numbers
    write_le16(eocd, (uint16_t)entries.size());
    write_le16(eocd, (uint16_t)entries.size());
    write_le32(eocd, cd_size);
    write_le32(eocd, cd_offset);
    write_le16(eocd, 0);                                  // comment length
    fwrite(eocd.data(), 1, eocd.size(), zf);
    fclose(zf);

    LOGI("zip created: %s  files=%zu", dst_zip.c_str(), entries.size());
    return true;
}

// ── Unzip ─────────────────────────────────────────────────────────────────
bool unzip_folder(const std::string& src_zip, const std::string& dst_dir, std::string& err) {
    FILE* zf = fopen(src_zip.c_str(), "rb");
    if (!zf) { err = strerror(errno); return false; }

    FileOps::make_dirs(dst_dir);

    // قراءة Local File Headers بشكل تسلسلي
    while (!feof(zf)) {
        uint8_t sig[4];
        if (fread(sig, 1, 4, zf) != 4) break;

        // Local file header: PK\x03\x04
        if (sig[0]!=0x50||sig[1]!=0x4B||sig[2]!=0x03||sig[3]!=0x04) break;

        uint8_t lfh[26];
        if (fread(lfh, 1, 26, zf) != 26) break;

        uint16_t compression = lfh[4] | (lfh[5] << 8);
        uint32_t comp_size   = lfh[14] | (lfh[15]<<8) | (lfh[16]<<16) | (lfh[17]<<24);
        uint16_t name_len    = lfh[22] | (lfh[23] << 8);
        uint16_t extra_len   = lfh[24] | (lfh[25] << 8);

        std::string name(name_len, '\0');
        fread(&name[0], 1, name_len, zf);
        fseek(zf, extra_len, SEEK_CUR);

        // إنشاء المسار الكامل
        std::string out_path = dst_dir + "/" + name;

        // إنشاء المجلدات الضرورية
        size_t last_slash = out_path.rfind('/');
        if (last_slash != std::string::npos) {
            FileOps::make_dirs(out_path.substr(0, last_slash));
        }

        // تخطّي إدخالات المجلدات
        if (!name.empty() && name.back() == '/') {
            fseek(zf, comp_size, SEEK_CUR);
            continue;
        }

        // فك الضغط وكتابة الملف
        FILE* of = fopen(out_path.c_str(), "wb");
        if (!of) { fseek(zf, comp_size, SEEK_CUR); continue; }

        if (compression == 0) {
            // Store
            uint8_t buf[65536];
            uint32_t left = comp_size;
            while (left > 0) {
                uint32_t to_read = std::min(left, (uint32_t)sizeof(buf));
                size_t n = fread(buf, 1, to_read, zf);
                fwrite(buf, 1, n, of);
                left -= (uint32_t)n;
            }
        } else if (compression == 8) {
            // Deflate
            std::vector<uint8_t> comp_buf(comp_size);
            fread(comp_buf.data(), 1, comp_size, zf);

            z_stream zs{};
            inflateInit2(&zs, -15);
            zs.next_in  = comp_buf.data();
            zs.avail_in = comp_size;

            uint8_t out_buf[65536];
            while (zs.avail_in > 0) {
                zs.next_out  = out_buf;
                zs.avail_out = sizeof(out_buf);
                inflate(&zs, Z_SYNC_FLUSH);
                fwrite(out_buf, 1, sizeof(out_buf) - zs.avail_out, of);
            }
            inflateEnd(&zs);
        } else {
            fseek(zf, comp_size, SEEK_CUR);
        }
        fclose(of);
    }
    fclose(zf);
    LOGI("unzip done: %s → %s", src_zip.c_str(), dst_dir.c_str());
    return true;
}

} // namespace SmartSync
