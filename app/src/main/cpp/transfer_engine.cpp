// ════════════════════════════════════════════════════════════════════════════
// transfer_engine.cpp — المرحلة ④
// Adaptive chunking · Sliding window · zlib compression · CRC32 · Resume
// ════════════════════════════════════════════════════════════════════════════
#include "transfer_engine.hpp"
#include "stonx_protocol.hpp"
#include "crypto/sha256.hpp"
#include "config.hpp"
#include <android/log.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <algorithm>
#include <fstream>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "STONX_XFER", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "STONX_XFER", __VA_ARGS__)

using namespace StonxProto;

// ── SHA256 ملف ───────────────────────────────────────────────────────────
static std::string sha256_file_path(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";
    SHA256 h;
    uint8_t buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) h.update(buf, (size_t)n);
    close(fd);
    return SHA256::hex(h.digest());
}

// ── إرسال chunk واحد ──────────────────────────────────────────────────────
struct SentChunkMeta {
    int      index;
    int64_t  offset;
    int      original_size;
    uint32_t crc32;
};

static bool send_one_chunk(int fd, const std::string& tid,
                            FILE* fp, int64_t file_size,
                            int chunk_idx, int chunk_size,
                            std::vector<uint8_t>& rbuf,
                            std::vector<uint8_t>& cbuf,
                            SentChunkMeta& meta) {
    int64_t offset   = (int64_t)chunk_idx * chunk_size;
    int     to_read  = (int)std::min((int64_t)chunk_size, file_size - offset);
    if (to_read <= 0) return false;

    rbuf.resize(to_read);
    if (fseeko(fp, offset, SEEK_SET) != 0) return false;
    if ((int)fread(rbuf.data(), 1, to_read, fp) != to_read) return false;

    uint32_t crc = crc32_compute(rbuf.data(), to_read);

    // ضغط zlib تكيّفي
    bool compressed = zlib_compress(rbuf.data(), to_read, cbuf);
    bool use_comp   = compressed && cbuf.size() < (size_t)(to_read * Config::COMPRESS_MIN_PCT / 100);

    const uint8_t* send_ptr  = use_comp ? cbuf.data()        : rbuf.data();
    int            send_size = use_comp ? (int)cbuf.size()   : to_read;

    std::string b64 = base64_encode(send_ptr, send_size);
    std::string msg = build_chunk_msg(tid, chunk_idx, offset,
                                      to_read, send_size, crc, b64);

    if (write_frame(fd, msg) != IOResult::OK) return false;

    meta = { chunk_idx, offset, to_read, crc };
    return true;
}

// ══════════════════════════════════════════════════════════════════════════
// transfer_send — Agent → Controller
// ══════════════════════════════════════════════════════════════════════════
TransferResult transfer_send(int fd,
                              const std::string& transfer_id,
                              const std::string& local_path,
                              const std::string& remote_name,
                              const std::string& progress_dir,
                              const std::atomic<bool>& cancel,
                              ProgressCb on_progress) {

    // ① معلومات الملف
    struct stat st{};
    if (stat(local_path.c_str(), &st) != 0) {
        LOGE("send: stat failed: %s", local_path.c_str());
        return TransferResult::IO_ERROR;
    }
    int64_t total_size = st.st_size;
    LOGI("send start: %s  size=%lld", remote_name.c_str(), (long long)total_size);

    // ② SHA256 الكاملة للملف (مرة واحدة فقط)
    std::string file_sha256 = sha256_file_path(local_path);
    if (file_sha256.empty()) return TransferResult::IO_ERROR;

    // ③ تحميل بيانات الاستئناف إن وُجِدت
    ProgressRecord saved_prog;
    bool has_resume = progress_load(progress_dir, transfer_id, saved_prog);
    int chunk_size    = has_resume ? saved_prog.chunk_size  : Config::CHUNK_SIZE_DEF;
    int window_size   = Config::WINDOW_SIZE_DEF;
    int total_chunks  = (total_size == 0) ? 1
                      : (int)((total_size + chunk_size - 1) / chunk_size);
    int start_chunk   = has_resume ? saved_prog.last_confirmed_chunk + 1 : 0;
    start_chunk = std::min(start_chunk, total_chunks);

    // ④ TRANSFER_INIT
    std::string init_msg = build_transfer_init(
        transfer_id, remote_name, total_size, file_sha256,
        total_chunks, chunk_size, true);
    if (write_frame(fd, init_msg) != IOResult::OK) return TransferResult::DISCONNECTED;

    // ⑤ انتظر TRANSFER_INIT_ACK
    std::string json;
    if (read_frame(fd, json, Config::TRANSFER_READ_TIMEOUT_SEC) != IOResult::OK) return TransferResult::DISCONNECTED;
    if (json_get_str(json, "type") != "TRANSFER_INIT_ACK") return TransferResult::IO_ERROR;
    int ack_from = (int)json_get_int(json, "resumeFromChunk", 0);
    start_chunk = std::max(start_chunk, ack_from);
    LOGI("send: resume_from=%d  total_chunks=%d", start_chunk, total_chunks);

    // ⑥ افتح الملف
    FILE* fp = fopen(local_path.c_str(), "rb");
    if (!fp) return TransferResult::IO_ERROR;

    // ⑦ حلقة الإرسال (batch sliding window)
    std::vector<uint8_t> rbuf, cbuf;
    rbuf.reserve(chunk_size);
    cbuf.reserve(chunk_size);

    int confirmed = start_chunk;

    // قياس السرعة
    auto speed_ts    = std::chrono::steady_clock::now();
    int64_t speed_bytes = 0;
    double  cur_speed   = 0;
    int     speed_ticks = 0;

    for (int i = start_chunk; i < total_chunks && !cancel.load(); ) {
        int batch_end   = std::min(i + window_size, total_chunks);
        int batch_count = batch_end - i;

        // ── أرسل batch كامل ──────────────────────────────────────────────
        std::vector<SentChunkMeta> metas(batch_count);
        for (int b = 0; b < batch_count; ++b) {
            if (!send_one_chunk(fd, transfer_id, fp, total_size,
                                i + b, chunk_size, rbuf, cbuf, metas[b])) {
                fclose(fp);
                return TransferResult::DISCONNECTED;
            }
        }

        // ── استقبل ACK/NACK لكل chunk في الـ batch ───────────────────────
        int acked = 0;
        std::vector<int> retry_indices;

        while (acked < batch_count) {
            std::string ack_json;
            if (read_frame(fd, ack_json, 30) != IOResult::OK) {
                fclose(fp);
                return TransferResult::DISCONNECTED;
            }
            std::string t = json_get_str(ack_json, "type");
            int idx       = (int)json_get_int(ack_json, "chunkIndex", -1);

            if (t == "CHUNK_ACK") {
                confirmed = std::max(confirmed, idx + 1);
                speed_bytes += (idx < (int)metas.size() ? metas[idx - i].original_size : chunk_size);
                acked++;
            } else if (t == "CHUNK_NACK") {
                retry_indices.push_back(idx);
                acked++;
            }
        }

        // ── إعادة إرسال الـ NACKed chunks ────────────────────────────────
        for (int retry_count = 0; !retry_indices.empty() && retry_count < Config::CHUNK_MAX_RETRIES; ++retry_count) {
            std::vector<int> still_failed;
            for (int ri : retry_indices) {
                SentChunkMeta m{};
                if (!send_one_chunk(fd, transfer_id, fp, total_size,
                                    ri, chunk_size, rbuf, cbuf, m)) {
                    fclose(fp);
                    return TransferResult::DISCONNECTED;
                }
                // انتظر ACK لهذا الـ chunk
                std::string ack_json;
                if (read_frame(fd, ack_json, 30) != IOResult::OK) {
                    fclose(fp);
                    return TransferResult::DISCONNECTED;
                }
                if (json_get_str(ack_json, "type") != "CHUNK_ACK") {
                    still_failed.push_back(ri);
                }
            }
            retry_indices = still_failed;
        }
        if (!retry_indices.empty()) {
            fclose(fp);
            return TransferResult::CHUNK_MAX_RETRIES;
        }

        // ── قياس السرعة وضبط الـ window / chunk size ─────────────────────
        speed_ticks += batch_count;
    if (speed_ticks >= Config::SPEED_MEASURE_EVERY) {
    auto now     = std::chrono::steady_clock::now();
    double secs  = std::chrono::duration<double>(now - speed_ts).count();
    if (secs > 0.01) {
        cur_speed = (double)speed_bytes / secs;
        // ⚠️ نُعدّل window_size فقط — chunk_size ثابت من البداية (SHA256 مضمون)
        if      (cur_speed >= 10*1024*1024.0) { window_size = 16; }
        else if (cur_speed >=  5*1024*1024.0) { window_size = 12; }
        else if (cur_speed >=  1*1024*1024.0) { window_size = 8;  }
        else if (cur_speed >=    100*1024.0)  { window_size = 4;  }
        else                                   { window_size = 2;  }
        // chunk_size لا يتغير — total_chunks لا يُعاد حسابه
    }
    speed_ts    = std::chrono::steady_clock::now();
    speed_bytes = 0;
    speed_ticks = 0;
    LOGI("speed=%.0f KB/s  chunk=%dKB  win=%d",
         cur_speed/1024, chunk_size/1024, window_size);
      }
        // ── حفظ التقدم ────────────────────────────────────────────────────
        {
            ProgressRecord p;
            p.transfer_id            = transfer_id;
            p.remote_name            = remote_name;
            p.total_bytes            = total_size;
            p.total_chunks           = total_chunks;
            p.chunk_size             = chunk_size;
            p.last_confirmed_chunk   = confirmed - 1;
            p.last_confirmed_offset  = (int64_t)confirmed * chunk_size;
            p.sha256                 = file_sha256;
            progress_save(progress_dir, p);
        }

        // ── progress callback ────────────────────────────────────────────
        if (on_progress) {
            TransferProgress tp;
            tp.bytes_sent      = (int64_t)confirmed * chunk_size;
            tp.total_bytes     = total_size;
            tp.chunks_done     = confirmed;
            tp.chunks_total    = total_chunks;
            tp.speed_bytes_sec = cur_speed;
            tp.window_size     = window_size;
            tp.chunk_size      = chunk_size;
            on_progress(tp);
        }

        i = batch_end;
    }

    fclose(fp);
    if (cancel.load()) return TransferResult::CANCELLED;

    // ⑧ TRANSFER_COMPLETE
    std::string done_msg = build_transfer_complete(transfer_id, file_sha256);
    if (write_frame(fd, done_msg) != IOResult::OK) return TransferResult::DISCONNECTED;

    // ⑨ انتظر TRANSFER_COMPLETE_ACK
    if (read_frame(fd, json, 60) != IOResult::OK) return TransferResult::DISCONNECTED;
    std::string status = json_get_str(json, "status");
    if (status != "OK") {
        LOGE("TRANSFER_COMPLETE_ACK: status=%s", status.c_str());
        return TransferResult::INTEGRITY_ERROR;
    }

    progress_delete(progress_dir, transfer_id);
    LOGI("send complete: %s", remote_name.c_str());
    return TransferResult::OK;
}

// ══════════════════════════════════════════════════════════════════════════
// transfer_recv — Controller → Agent
// ══════════════════════════════════════════════════════════════════════════
TransferResult transfer_recv(int fd,
                              const std::string& local_path,
                              const std::string& progress_dir,
                              const std::atomic<bool>& cancel,
                              ProgressCb on_progress) {

    // ① انتظر TRANSFER_INIT
    std::string init_json;
    if (read_frame(fd, init_json, 60) != IOResult::OK) return TransferResult::DISCONNECTED;
    if (json_get_str(init_json, "type") != "TRANSFER_INIT") return TransferResult::IO_ERROR;

    std::string tid           = json_get_str(init_json, "transferId");
    std::string sha256_exp    = json_get_str(init_json, "sha256");
    int64_t     total_size    = json_get_int(init_json, "totalSize");
    int         total_chunks  = (int)json_get_int(init_json, "totalChunks");
    int         chunk_size    = (int)json_get_int(init_json, "chunkSize");
    bool        compressed    = json_get_str(init_json, "compression") == "zlib";

    LOGI("recv start: %s  size=%lld  chunks=%d",
         local_path.c_str(), (long long)total_size, total_chunks);

    // ② استئناف؟
    ProgressRecord saved;
    bool has_resume = progress_load(progress_dir, tid, saved);
    int resume_from = has_resume ? saved.last_confirmed_chunk + 1 : 0;

    // ③ TRANSFER_INIT_ACK
    std::string iack = "{\"type\":\"TRANSFER_INIT_ACK\","
                       "\"payload\":{\"transferId\":\"" + tid + "\","
                       "\"resumeFromChunk\":" + std::to_string(resume_from) + "}}";
    if (write_frame(fd, iack) != IOResult::OK) return TransferResult::DISCONNECTED;

    // ④ افتح ملف مؤقت
    std::string tmp_path = local_path + ".part";
    FILE* fp = fopen(tmp_path.c_str(), has_resume ? "r+b" : "wb");
    if (!fp) return TransferResult::IO_ERROR;
    if (total_size > 0 && !has_resume) {
        ftruncate(fileno(fp), total_size);
    }

    int received = resume_from;
    std::vector<uint8_t> raw_buf, decomp_buf;

    while (received < total_chunks && !cancel.load()) {
        std::string cjson;
        if (read_frame(fd, cjson, 60) != IOResult::OK) {
            fclose(fp);
            return TransferResult::DISCONNECTED;
        }

        std::string type = json_get_str(cjson, "type");
        if (type == "TRANSFER_COMPLETE") {
            // إرسال ACK هنا وإنهاء الحلقة
            break;
        }
        if (type != "CHUNK") continue;

        int     ci          = (int)json_get_int(cjson, "chunkIndex");
        int64_t offset      = json_get_int(cjson,  "offset");
        int     orig_size   = (int)json_get_int(cjson, "originalSize");
        int     comp_size   = (int)json_get_int(cjson, "compressedSize");
        uint32_t exp_crc    = (uint32_t)json_get_int(cjson, "crc32");
        std::string b64     = json_get_str(cjson, "data");

        // فك Base64
        raw_buf = base64_decode(b64);

        // فك الضغط إذا لزم
        const uint8_t* chunk_ptr;
        size_t         chunk_len;
        if (compressed && comp_size < orig_size) {
            if (!zlib_decompress(raw_buf.data(), raw_buf.size(),
                                 decomp_buf, (size_t)orig_size * 2)) {
                // NACK
                std::string nack = "{\"type\":\"CHUNK_NACK\",\"payload\":{"
                    "\"chunkIndex\":" + std::to_string(ci) + "}}";
                write_frame(fd, nack);
                continue;
            }
            chunk_ptr = decomp_buf.data();
            chunk_len = decomp_buf.size();
        } else {
            chunk_ptr = raw_buf.data();
            chunk_len = raw_buf.size();
        }

        // تحقق CRC32
        uint32_t actual_crc = crc32_compute(chunk_ptr, chunk_len);
        if (actual_crc != exp_crc) {
            LOGE("recv: CRC32 mismatch chunk=%d  exp=%08x  got=%08x",
                 ci, exp_crc, actual_crc);
            std::string nack = "{\"type\":\"CHUNK_NACK\",\"payload\":{"
                "\"chunkIndex\":" + std::to_string(ci) + "}}";
            write_frame(fd, nack);
            continue;
        }

        // اكتب في الملف في الموضع الصحيح
        fseeko(fp, offset, SEEK_SET);
        fwrite(chunk_ptr, 1, chunk_len, fp);

        // ACK
        std::string ack = "{\"type\":\"CHUNK_ACK\",\"payload\":{"
            "\"chunkIndex\":" + std::to_string(ci) + "}}";
        write_frame(fd, ack);
        received++;

        // حفظ التقدم
        ProgressRecord p;
        p.transfer_id          = tid;
        p.total_bytes          = total_size;
        p.total_chunks         = total_chunks;
        p.chunk_size           = chunk_size;
        p.last_confirmed_chunk = ci;
        p.sha256               = sha256_exp;
        progress_save(progress_dir, p);

        if (on_progress) {
            TransferProgress tp;
            tp.bytes_sent   = offset + (int64_t)chunk_len;
            tp.total_bytes  = total_size;
            tp.chunks_done  = received;
            tp.chunks_total = total_chunks;
            on_progress(tp);
        }
    }

    fclose(fp);
    if (cancel.load()) return TransferResult::CANCELLED;

    // ⑤ تحقق SHA256 النهائي
    std::string actual_sha256 = sha256_file_path(tmp_path);
    if (!sha256_exp.empty() && actual_sha256 != sha256_exp) {
        LOGE("recv: SHA256 mismatch for %s", local_path.c_str());
        // أرسل TRANSFER_COMPLETE_ACK (ERROR)
        std::string dack = "{\"type\":\"TRANSFER_COMPLETE_ACK\","
                           "\"payload\":{\"status\":\"ERROR\"}}";
        write_frame(fd, dack);
        unlink(tmp_path.c_str());
        return TransferResult::INTEGRITY_ERROR;
    }

    // ⑥ إعادة تسمية .part → الملف النهائي
    rename(tmp_path.c_str(), local_path.c_str());

    // ⑦ TRANSFER_COMPLETE_ACK (success)
    std::string dack = "{\"type\":\"TRANSFER_COMPLETE_ACK\","
                       "\"payload\":{\"transferId\":\"" + tid + "\","
                       "\"status\":\"OK\"}}";
    write_frame(fd, dack);

    progress_delete(progress_dir, tid);
    LOGI("recv complete: %s", local_path.c_str());
    return TransferResult::OK;
}

// ══════════════════════════════════════════════════════════════════════════
// zlib helpers
// ══════════════════════════════════════════════════════════════════════════
bool zlib_compress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst) {
    uLongf bound = compressBound((uLong)src_len);
    dst.resize(bound);
    int rc = compress2(dst.data(), &bound,
                   src, (uLong)src_len, Config::ZLIB_LEVEL);
    if (rc != Z_OK) return false;
    dst.resize(bound);
    return true;
}

bool zlib_decompress(const uint8_t* src, size_t src_len,
                     std::vector<uint8_t>& dst, size_t max_out) {
    dst.resize(max_out);
    uLongf out_len = (uLongf)max_out;
    int rc = uncompress(dst.data(), &out_len, src, (uLong)src_len);
    if (rc != Z_OK) return false;
    dst.resize(out_len);
    return true;
}

// ══════════════════════════════════════════════════════════════════════════
// Progress persistence (text file, بسيط وموثوق)
// Format: key=value per line
// ══════════════════════════════════════════════════════════════════════════
static std::string prog_path(const std::string& dir, const std::string& tid) {
    // استبدل الرموز الغير آمنة في اسم الملف
    std::string safe = tid;
    for (char& c : safe) if (c == '/' || c == '\\') c = '_';
    return dir + "/" + safe + ".prog";
}

bool progress_save(const std::string& dir, const ProgressRecord& r) {
    // أنشئ المجلد لو لم يكن موجوداً
    mkdir(dir.c_str(), 0755);
    std::string path = prog_path(dir, r.transfer_id);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "transfer_id=%s\n",           r.transfer_id.c_str());
    fprintf(f, "remote_name=%s\n",           r.remote_name.c_str());
    fprintf(f, "total_bytes=%lld\n",         (long long)r.total_bytes);
    fprintf(f, "total_chunks=%d\n",          r.total_chunks);
    fprintf(f, "chunk_size=%d\n",            r.chunk_size);
    fprintf(f, "last_confirmed_chunk=%d\n",  r.last_confirmed_chunk);
    fprintf(f, "last_confirmed_offset=%lld\n",(long long)r.last_confirmed_offset);
    fprintf(f, "sha256=%s\n",                r.sha256.c_str());
    fclose(f);
    return true;
}

bool progress_load(const std::string& dir, const std::string& tid, ProgressRecord& out) {
    std::string path = prog_path(dir, tid);
    std::ifstream ifs(path);
    if (!ifs) return false;

    std::string line;
    while (std::getline(ifs, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if      (key == "transfer_id")           out.transfer_id           = val;
        else if (key == "remote_name")           out.remote_name           = val;
        else if (key == "total_bytes")           out.total_bytes           = std::stoll(val);
        else if (key == "total_chunks")          out.total_chunks          = std::stoi(val);
        else if (key == "chunk_size")            out.chunk_size            = std::stoi(val);
        else if (key == "last_confirmed_chunk")  out.last_confirmed_chunk  = std::stoi(val);
        else if (key == "last_confirmed_offset") out.last_confirmed_offset = std::stoll(val);
        else if (key == "sha256")                out.sha256                = val;
    }
    return !out.transfer_id.empty();
}

void progress_delete(const std::string& dir, const std::string& tid) {
    unlink(prog_path(dir, tid).c_str());
}
