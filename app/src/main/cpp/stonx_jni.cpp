// ════════════════════════════════════════════════════════════════════════════
// stonx_jni.cpp — JNI bridge
//
// Java StonxService ←→ C++ StonxCore
//
// Callbacks من C++ إلى Java عبر static method:
//   StonxService.onNativeEvent(String type, String data)
//
// Camera bridge:
//   C++ → Java:  StonxService.startCameraCapture(facing, outPath, opId, null)
//   Java → C++:  nativeCameraResult(opId, success, pathOrError)
// ════════════════════════════════════════════════════════════════════════════

#include <jni.h>
#include <android/log.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <functional>

#include "stonx_core.hpp"
#include "config.hpp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "STONX_JNI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "STONX_JNI", __VA_ARGS__)

// ── حالة عالمية ───────────────────────────────────────────────────────────
static JavaVM*         g_vm            = nullptr;
static jclass          g_service_class = nullptr;
static jmethodID       g_on_event_mid  = nullptr;
static jmethodID       g_start_cam_mid = nullptr;
static std::unique_ptr<StonxCore> g_core;
static std::mutex      g_core_mutex;

// خريطة لحفظ الـ listeners المؤقتة (opId → function)
static std::mutex g_cam_mutex;
static std::unordered_map<std::string, std::function<void(bool, std::string)>> g_cam_listeners;

// ── JNI_OnLoad ─────────────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    LOGI("JNI_OnLoad — STONX Manager v2.0.0");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNI_OnUnload(JavaVM*, void*) {
    std::lock_guard<std::mutex> lock(g_core_mutex);
    if (g_core) { g_core->stop(); g_core.reset(); }

    if (g_vm && g_service_class) {
        JNIEnv* env;
        g_vm->GetEnv((void**)&env, JNI_VERSION_1_6);
        if (env) env->DeleteGlobalRef(g_service_class);
        g_service_class = nullptr;
    }
    LOGI("JNI_OnUnload");
}

// ── helper: JNI string → std::string ─────────────────────────────────────
static std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}

// ── helper: استدعاء StonxService.onNativeEvent من أي thread ───────────────
static void fire_event(const std::string& type, const std::string& data) {
    if (!g_vm || !g_service_class || !g_on_event_mid) return;

    JNIEnv* env;
    bool need_detach = false;
    jint rc = g_vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        need_detach = true;
    } else if (rc != JNI_OK) {
        return;
    }

    jstring jtype = env->NewStringUTF(type.c_str());
    jstring jdata = env->NewStringUTF(data.c_str());
    env->CallStaticVoidMethod(g_service_class, g_on_event_mid, jtype, jdata);
    env->DeleteLocalRef(jtype);
    env->DeleteLocalRef(jdata);

    if (env->ExceptionCheck()) env->ExceptionClear();
    if (need_detach) g_vm->DetachCurrentThread();
}

// ════════════════════════════════════════════════════════════════════════════
//  JNI Methods (مرتبطة بـ StonxService.java)
// ════════════════════════════════════════════════════════════════════════════

// nativeInit(String filesDir)
extern "C"
JNIEXPORT void JNICALL
Java_com_stonx_manager_StonxService_nativeInit(
        JNIEnv* env, jclass clazz, jstring jfiles_dir) {

    if (!g_service_class) {
        g_service_class = (jclass)env->NewGlobalRef(clazz);
        g_on_event_mid  = env->GetStaticMethodID(
                g_service_class, "onNativeEvent",
                "(Ljava/lang/String;Ljava/lang/String;)V");
        if (!g_on_event_mid) {
            LOGE("onNativeEvent method not found!");
        }
        g_start_cam_mid = env->GetStaticMethodID(
                g_service_class, "startCameraCapture",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                "Lcom/stonx/manager/StonxService$CameraResultListener;)V");
        if (!g_start_cam_mid) {
            LOGE("startCameraCapture method not found!");
        }
    }

    LOGI("nativeInit filesDir=%s", env->GetStringUTFChars(jfiles_dir, nullptr));
    fire_event("init", "ready");
}
// nativeStart()
extern "C"
JNIEXPORT void JNICALL
Java_com_stonx_manager_StonxService_nativeStart(JNIEnv* env, jclass,
                                                 jstring jfiles_dir,
                                                 jstring jhost,
                                                 jint    jport) {
    std::lock_guard<std::mutex> lock(g_core_mutex);

    if (g_core && g_core->is_running()) {
        LOGI("nativeStart: already running");
        return;
    }

    std::string files_dir = jstr(env, jfiles_dir);
    std::string host      = jstr(env, jhost);

    // ── التحقق من الـ Endpoint قبل الاستخدام ────────────────────────────
    if (host.empty()) {
        LOGE("nativeStart: host is empty — aborting");
        fire_event("connection_error", "INVALID_HOST");
        return;
    }
    if (jport < 1 || jport > 65535) {
        LOGE("nativeStart: port %d out of range — aborting", (int)jport);
        fire_event("connection_error", "INVALID_PORT");
        return;
    }
    uint16_t port = static_cast<uint16_t>(jport);

    StonxCore::Callbacks cbs;

    cbs.on_connection_changed = [](bool connected) {
        fire_event("connection", connected ? "true" : "false");
    };
    cbs.on_op_start = [](const std::string& type, const std::string& detail) {
        fire_event("op_start", type + "|" + detail);
    };
    cbs.on_op_end = [](const std::string& type, bool ok) {
        fire_event("op_end", type + "|" + (ok ? "ok" : "fail"));
    };
    cbs.on_pending_changed = [](int count) {
        fire_event("pending", std::to_string(count));
    };
    cbs.on_log = [](const std::string& msg) {
        LOGI("[core] %s", msg.c_str());
    };

    g_core = std::make_unique<StonxCore>(
        files_dir,
        host,
        port,
        Config::PSK,
        std::move(cbs)
    );
    g_core->start();
    LOGI("nativeStart: core started → %s:%d", host.c_str(), (int)port);
}

// nativeStop()
extern "C"
JNIEXPORT void JNICALL
Java_com_stonx_manager_StonxService_nativeStop(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_core_mutex);
    if (g_core) {
        g_core->stop();
        g_core.reset();
    }
    LOGI("nativeStop");
}

// nativeIsRunning() → boolean
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_stonx_manager_StonxService_nativeIsRunning(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_core_mutex);
    return (g_core && g_core->is_running()) ? JNI_TRUE : JNI_FALSE;
}

// ════════════════════════════════════════════════════════════════════════════
//  JNI Camera bridge
// ════════════════════════════════════════════════════════════════════════════

// ✅ يُستدعى من C++ (StonxCore) لتشغيل Java CameraService
// ملاحظة: الـ listener يُمرَّر كـ nullptr عادةً، لأن StonxCore يحتفظ
// بـ listeners الخاصة به في m_cam_listeners. النتيجة تصل عبر
// nativeCameraResult → g_core->on_camera_result.
void jni_start_camera_capture(const std::string& facing,
                              const std::string& out_path,
                              const std::string& op_id,
                              std::function<void(bool, std::string)> listener)
{
    if (!g_vm || !g_service_class || !g_start_cam_mid) {
        LOGE("jni_start_camera_capture: not initialized");
        if (listener) listener(false, "JNI_NOT_INITIALIZED");
        return;
    }

    // إذا مُرّر listener فعلي، احفظه في g_cam_listeners (احتياطي)
    if (listener) {
        std::lock_guard<std::mutex> lock(g_cam_mutex);
        g_cam_listeners[op_id] = std::move(listener);
    }

    JNIEnv* env = nullptr;
    bool need_detach = false;
    jint rc = g_vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("jni_start_camera_capture: attach failed");
            std::lock_guard<std::mutex> lock(g_cam_mutex);
            g_cam_listeners.erase(op_id);
            return;
        }
        need_detach = true;
    } else if (rc != JNI_OK) {
        LOGE("jni_start_camera_capture: GetEnv failed");
        return;
    }

    jstring jfacing = env->NewStringUTF(facing.c_str());
    jstring jout    = env->NewStringUTF(out_path.c_str());
    jstring jop     = env->NewStringUTF(op_id.c_str());

    env->CallStaticVoidMethod(g_service_class, g_start_cam_mid,
                              jfacing, jout, jop, nullptr);

    env->DeleteLocalRef(jfacing);
    env->DeleteLocalRef(jout);
    env->DeleteLocalRef(jop);

    if (env->ExceptionCheck()) {
        LOGE("jni_start_camera_capture: exception");
        env->ExceptionDescribe();
        env->ExceptionClear();
        std::lock_guard<std::mutex> lock(g_cam_mutex);
        g_cam_listeners.erase(op_id);
    }

    if (need_detach) g_vm->DetachCurrentThread();
}

// ✅ يُستدعى من Java عند وصول نتيجة الكاميرا
extern "C"
JNIEXPORT void JNICALL
Java_com_stonx_manager_StonxService_nativeCameraResult(
        JNIEnv* env, jclass, jstring jop_id, jboolean jsuccess, jstring jval)
{
    std::string op_id   = jstr(env, jop_id);
    std::string val     = jstr(env, jval);
    bool        success = (jsuccess == JNI_TRUE);

    LOGI("nativeCameraResult: op=%s ok=%d val=%s",
         op_id.c_str(), success ? 1 : 0, val.c_str());

    // جرّب g_cam_listeners أولاً
    std::function<void(bool, std::string)> listener;
    {
        std::lock_guard<std::mutex> lock(g_cam_mutex);
        auto it = g_cam_listeners.find(op_id);
        if (it != g_cam_listeners.end()) {
            listener = std::move(it->second);
            g_cam_listeners.erase(it);
        }
    }

    if (listener) {
        listener(success, val);
        return;
    }

    // ✅ Fallback: مرّر إلى StonxCore (الذي يحتفظ بـ m_cam_listeners)
    {
        std::lock_guard<std::mutex> lock(g_core_mutex);
        if (g_core) {
            g_core->on_camera_result(op_id, success, val);
        } else {
            LOGE("nativeCameraResult: no core, op=%s", op_id.c_str());
        }
    }
}
