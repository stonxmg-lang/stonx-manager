// ════════════════════════════════════════════════════════════════════════════
// camera_ops.cpp — Camera2 NDK (v4)
// نهج: dummy surface للـ warmup + sleep + stopRepeating + capture على JPEG
// ════════════════════════════════════════════════════════════════════════════
#include "camera_ops.hpp"
#include "config.hpp"
#include <android/log.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "STONX_CAM", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "STONX_CAM", __VA_ARGS__)

namespace CameraOps {

// ── State ──────────────────────────────────────────────────────────────────
struct CaptureCtx {
    std::mutex              mu;
    std::condition_variable cv;
    ACameraCaptureSession*  session       = nullptr;
    AImage*                 image         = nullptr;
    bool sess_ready = false;
    bool img_ready  = false;
    bool cap_error  = false;
    int  error_code = 0;
    std::atomic<bool> captured{false};
};

// ── Device callbacks ──────────────────────────────────────────────────────
static void cb_dev_disc(void* ctx, ACameraDevice*) {
    auto* c = static_cast<CaptureCtx*>(ctx);
    std::lock_guard<std::mutex> l(c->mu);
    c->cap_error = true; c->error_code = 1001;
    c->cv.notify_all();
    LOGE("camera disconnected");
}
static void cb_dev_err(void* ctx, ACameraDevice*, int err) {
    auto* c = static_cast<CaptureCtx*>(ctx);
    std::lock_guard<std::mutex> l(c->mu);
    c->cap_error = true; c->error_code = 1000 + err;
    c->cv.notify_all();
    LOGE("camera error: %d", err);
}

// ── Session callbacks ─────────────────────────────────────────────────────
static void cb_sess_ready(void* ctx, ACameraCaptureSession*) {
    auto* c = static_cast<CaptureCtx*>(ctx);
    std::lock_guard<std::mutex> l(c->mu);
    c->sess_ready = true;
    c->cv.notify_all();
    LOGI("session onReady");
}
static void cb_sess_close(void*, ACameraCaptureSession*) { LOGI("session closed"); }
static void cb_sess_act(void* ctx, ACameraCaptureSession*) {
    auto* c = static_cast<CaptureCtx*>(ctx);
    std::lock_guard<std::mutex> l(c->mu);
    c->sess_ready = true;
    c->cv.notify_all();
    LOGI("session onActive");
}

// ── Image callback ────────────────────────────────────────────────────────
static void cb_image(void* ctx, AImageReader* r) {
    auto* c = static_cast<CaptureCtx*>(ctx);
    AImage* img = nullptr;
    media_status_t st = AImageReader_acquireLatestImage(r, &img);
    std::lock_guard<std::mutex> l(c->mu);
    if (st == AMEDIA_OK && img) {
        if (!c->image) c->image = img;
        else AImage_delete(img);  // تجاهل الصور القديمة
        c->img_ready = true;
        LOGI("image acquired");
    } else {
        c->cap_error = true;
        c->error_code = 2000 + st;
        LOGE("acquireLatestImage failed: %d", st);
    }
    c->cv.notify_all();
}

// ── find camera ───────────────────────────────────────────────────────────
static std::string find_camera(ACameraManager* mgr, CameraFacing facing) {
    uint8_t want = facing == CameraFacing::FRONT
                 ? ACAMERA_LENS_FACING_FRONT : ACAMERA_LENS_FACING_BACK;
    ACameraIdList* list = nullptr;
    if (ACameraManager_getCameraIdList(mgr, &list) != ACAMERA_OK || !list) return "";
    std::string found;
    for (int i = 0; i < list->numCameras && found.empty(); ++i) {
        const char* id = list->cameraIds[i];
        ACameraMetadata* meta = nullptr;
        ACameraManager_getCameraCharacteristics(mgr, id, &meta);
        if (!meta) continue;
        ACameraMetadata_const_entry e{};
        if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &e) == ACAMERA_OK
            && e.count > 0 && e.data.u8[0] == want) found = id;
        ACameraMetadata_free(meta);
    }
    ACameraManager_deleteCameraIdList(list);
    return found;
}

// ── RAII ──────────────────────────────────────────────────────────────────
struct CamRes {
    ACameraManager*                 mgr       = nullptr;
    ACameraDevice*                  device    = nullptr;
    AImageReader*                   jpeg_r    = nullptr;
    AImageReader*                   dummy_r   = nullptr;
    ACaptureSessionOutput*          jpeg_out  = nullptr;
    ACaptureSessionOutput*          dummy_out = nullptr;
    ACaptureSessionOutputContainer* cont      = nullptr;
    ACameraCaptureSession*          session   = nullptr;
    ACaptureRequest*                req       = nullptr;
    ACameraOutputTarget*            req_tgt   = nullptr;
    ACaptureRequest*                prev_req  = nullptr;
    ACameraOutputTarget*            prev_tgt  = nullptr;

    ~CamRes() {
        if (req && req_tgt)       ACaptureRequest_removeTarget(req, req_tgt);
        if (prev_req && prev_tgt) ACaptureRequest_removeTarget(prev_req, prev_tgt);
        if (req_tgt)  ACameraOutputTarget_free(req_tgt);
        if (prev_tgt) ACameraOutputTarget_free(prev_tgt);
        if (req)      ACaptureRequest_free(req);
        if (prev_req) ACaptureRequest_free(prev_req);
        if (session)  ACameraCaptureSession_close(session);
        if (cont && jpeg_out)  ACaptureSessionOutputContainer_remove(cont, jpeg_out);
        if (cont && dummy_out) ACaptureSessionOutputContainer_remove(cont, dummy_out);
        if (jpeg_out)  ACaptureSessionOutput_free(jpeg_out);
        if (dummy_out) ACaptureSessionOutput_free(dummy_out);
        if (cont)      ACaptureSessionOutputContainer_free(cont);
        if (jpeg_r)    AImageReader_delete(jpeg_r);
        if (dummy_r)   AImageReader_delete(dummy_r);
        if (device)    ACameraDevice_close(device);
        if (mgr)       ACameraManager_delete(mgr);
    }
};

static std::string error_code_str(const CaptureCtx& ctx) {
    return "CAPTURE_FAILED_" + std::to_string(ctx.error_code);
}

// ══════════════════════════════════════════════════════════════════════════
//  capture_photo — النسخة النهائية
// ══════════════════════════════════════════════════════════════════════════
void capture_photo(CameraFacing facing,
                   const std::string& tmp_dir,
                   CaptureCallback callback) {

    auto ctx = std::make_shared<CaptureCtx>();
    CamRes res;

    // ① Camera Manager
    res.mgr = ACameraManager_create();
    if (!res.mgr) { callback(CaptureResult::NO_CAMERA, ""); return; }

    // ② ابحث عن الكاميرا
    std::string cam_id = find_camera(res.mgr, facing);
    if (cam_id.empty()) { callback(CaptureResult::NO_CAMERA, ""); return; }
    LOGI("opening camera: %s", cam_id.c_str());

    // ③ افتح الكاميرا
    ACameraDevice_StateCallbacks dcbs{};
    dcbs.context        = ctx.get();
    dcbs.onDisconnected = cb_dev_disc;
    dcbs.onError        = cb_dev_err;

    camera_status_t open_st = ACameraManager_openCamera(
        res.mgr, cam_id.c_str(), &dcbs, &res.device);
    if (open_st != ACAMERA_OK || !res.device) {
        callback(CaptureResult::NO_CAMERA, "");
        return;
    }

    // ④ ImageReaders: JPEG (الفعلي) + YUV dummy (للـ warmup)
    media_status_t jr_st = AImageReader_new(
        Config::JPEG_WIDTH, Config::JPEG_HEIGHT,
        AIMAGE_FORMAT_JPEG, 2, &res.jpeg_r);
    if (jr_st != AMEDIA_OK || !res.jpeg_r) {
        callback(CaptureResult::CAPTURE_FAILED, "JPEG_READER_FAILED");
        return;
    }

    media_status_t dr_st = AImageReader_new(
        640, 480, AIMAGE_FORMAT_YUV_420_888, 2, &res.dummy_r);
    if (dr_st != AMEDIA_OK || !res.dummy_r) {
        callback(CaptureResult::CAPTURE_FAILED, "DUMMY_READER_FAILED");
        return;
    }

    // Listener للـ JPEG فقط
    AImageReader_ImageListener il{ctx.get(), cb_image};
    AImageReader_setImageListener(res.jpeg_r, &il);

    // dummy listener — يتجاهل كل الفريمات
    AImageReader_ImageListener dummy_il{nullptr, nullptr};
    AImageReader_setImageListener(res.dummy_r, &dummy_il);

    ANativeWindow* jpeg_win  = nullptr;
    ANativeWindow* dummy_win = nullptr;
    AImageReader_getWindow(res.jpeg_r,  &jpeg_win);
    AImageReader_getWindow(res.dummy_r, &dummy_win);
    if (!jpeg_win || !dummy_win) {
        callback(CaptureResult::CAPTURE_FAILED, "WINDOW_FAILED");
        return;
    }

    // ⑤ Session outputs
    ACaptureSessionOutput_create(jpeg_win,  &res.jpeg_out);
    ACaptureSessionOutput_create(dummy_win, &res.dummy_out);
    ACaptureSessionOutputContainer_create(&res.cont);
    ACaptureSessionOutputContainer_add(res.cont, res.jpeg_out);
    ACaptureSessionOutputContainer_add(res.cont, res.dummy_out);

    ACameraCaptureSession_stateCallbacks scbs{};
    scbs.context  = ctx.get();
    scbs.onReady  = cb_sess_ready;
    scbs.onClosed = cb_sess_close;
    scbs.onActive = cb_sess_act;

    camera_status_t cs_st = ACameraDevice_createCaptureSession(
        res.device, res.cont, &scbs, &res.session);
    if (cs_st != ACAMERA_OK || !res.session) {
        callback(CaptureResult::CAPTURE_FAILED, "CREATE_SESSION_FAILED");
        return;
    }
    ctx->session = res.session;

    // ⑥ Preview على dummy (warmup)
    camera_status_t pr_st = ACameraDevice_createCaptureRequest(
        res.device, TEMPLATE_PREVIEW, &res.prev_req);
    if (pr_st != ACAMERA_OK || !res.prev_req) {
        callback(CaptureResult::CAPTURE_FAILED, "PREVIEW_REQ_FAILED");
        return;
    }
    ACameraOutputTarget_create(dummy_win, &res.prev_tgt);
    ACaptureRequest_addTarget(res.prev_req, res.prev_tgt);
    uint8_t af_mode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    ACaptureRequest_setEntry_u8(res.prev_req, ACAMERA_CONTROL_AF_MODE, 1, &af_mode);
    // ملاحظة: أعلاه hack صغير — النية: إعداد AF_MODE

    camera_status_t sr_st = ACameraCaptureSession_setRepeatingRequest(
        res.session, nullptr, 1, &res.prev_req, nullptr);
    if (sr_st != ACAMERA_OK) {
        callback(CaptureResult::CAPTURE_FAILED, "REPEATING_FAILED");
        return;
    }
    LOGI("preview repeating started");

    // ⑦ Warmup: انتظر 2 ثانية لاستقرار AE/AF
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // ⑧ أوقف الـ preview
    camera_status_t stop_st = ACameraCaptureSession_stopRepeating(res.session);
    LOGI("stopRepeating status=%d", (int)stop_st);

    // ⑨ طلب STILL (مع fallback إلى PREVIEW)
    camera_status_t rq_st = ACameraDevice_createCaptureRequest(
        res.device, TEMPLATE_STILL_CAPTURE, &res.req);
    if (rq_st != ACAMERA_OK || !res.req) {
        LOGI("STILL_CAPTURE failed, trying PREVIEW");
        rq_st = ACameraDevice_createCaptureRequest(
            res.device, TEMPLATE_PREVIEW, &res.req);
        if (rq_st != ACAMERA_OK || !res.req) {
            callback(CaptureResult::CAPTURE_FAILED, "REQ_FAILED");
            return;
        }
    }
    ACameraOutputTarget_create(jpeg_win, &res.req_tgt);
    ACaptureRequest_addTarget(res.req, res.req_tgt);
    uint8_t quality = 95;
    ACaptureRequest_setEntry_u8(res.req, ACAMERA_JPEG_QUALITY, 1, &quality);

    // ⑩ التقاط
    camera_status_t cap_st = ACameraCaptureSession_capture(
        res.session, nullptr, 1, &res.req, nullptr);
    if (cap_st != ACAMERA_OK) {
        callback(CaptureResult::CAPTURE_FAILED, "CAPTURE_CALL_FAILED");
        return;
    }
    LOGI("capture() called");

    // ⑪ انتظر الصورة (25s)
    {
        std::unique_lock<std::mutex> lk(ctx->mu);
        ctx->cv.wait_for(lk, std::chrono::seconds(25),
            [&]{ return ctx->img_ready || ctx->cap_error; });
        if (ctx->cap_error && !ctx->img_ready) {
            std::string err = ctx->error_code ? error_code_str(*ctx) : "IMAGE_ERROR";
            callback(CaptureResult::CAPTURE_FAILED, err);
            return;
        }
    }

    if (!ctx->img_ready || !ctx->image) {
        callback(CaptureResult::CAPTURE_FAILED, "IMAGE_TIMEOUT");
        return;
    }

    // ⑫ استخراج JPEG
    uint8_t* plane = nullptr;
    int plane_len = 0;
    media_status_t pd_st = AImage_getPlaneData(ctx->image, 0, &plane, &plane_len);
    if (pd_st != AMEDIA_OK || !plane || plane_len <= 0) {
        AImage_delete(ctx->image);
        ctx->image = nullptr;
        callback(CaptureResult::CAPTURE_FAILED, "PLANE_FAILED");
        return;
    }

    // ⑬ حفظ
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string out_path = tmp_dir + "/stonx_" + std::to_string(ts) + ".jpg";
    FILE* f = fopen(out_path.c_str(), "wb");
    bool saved = false;
    if (f) {
        fwrite(plane, 1, (size_t)plane_len, f);
        fclose(f);
        saved = true;
        LOGI("photo saved: %s (%d bytes)", out_path.c_str(), plane_len);
    }

    AImage_delete(ctx->image);
    ctx->image = nullptr;

    if (saved) callback(CaptureResult::OK, out_path);
    else       callback(CaptureResult::CAPTURE_FAILED, "SAVE_FAILED");
}

} // namespace CameraOps

