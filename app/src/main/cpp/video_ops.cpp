// video_ops.cpp — Camera2 NDK + MediaCodec H.264 + AAudio + MediaMuxer
// ACameraDevice_StateCallbacks: context, onDisconnected, onError ONLY (no onOpened)
// ACameraManager_openCamera returns device SYNCHRONOUSLY
#include "video_ops.hpp"
#include "config.hpp"
#include <android/log.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <media/NdkMediaError.h>
#include <aaudio/AAudio.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "STONX_VID", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "STONX_VID", __VA_ARGS__)

namespace VideoOps {

struct VidCtx {
    std::mutex mu;
    std::condition_variable cv;
    ACameraCaptureSession* cam_session = nullptr;
    bool sess_ready = false;
    bool cam_error  = false;
    std::atomic<bool> recording{false};
};

// Session callbacks only — ACameraDevice_StateCallbacks has NO onOpened
static void v_dev_disc(void* c, ACameraDevice*) {
    auto* ctx = static_cast<VidCtx*>(c);
    std::lock_guard<std::mutex> l(ctx->mu);
    ctx->cam_error = true; ctx->cv.notify_all();
}
static void v_dev_err(void* c, ACameraDevice*, int) {
    auto* ctx = static_cast<VidCtx*>(c);
    std::lock_guard<std::mutex> l(ctx->mu);
    ctx->cam_error = true; ctx->cv.notify_all();
}
static void v_sess_ready(void* c, ACameraCaptureSession*) {
    auto* ctx = static_cast<VidCtx*>(c);
    std::lock_guard<std::mutex> l(ctx->mu);
    ctx->sess_ready = true; ctx->cv.notify_all();
}
static void v_sess_close(void*, ACameraCaptureSession*) {}
static void v_sess_act(void*, ACameraCaptureSession*) {}

static std::string find_cam(ACameraManager* mgr, VideoFacing facing) {
    uint8_t want = facing == VideoFacing::FRONT
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

struct VidRes {
    ACameraManager*                 mgr       = nullptr;
    ACameraDevice*                  device    = nullptr;
    ACaptureSessionOutput*          sess_out  = nullptr;
    ACaptureSessionOutputContainer* sess_cont = nullptr;
    ACameraCaptureSession*          session   = nullptr;
    ANativeWindow*                  enc_win   = nullptr;
    AMediaCodec*                    vcodec    = nullptr;
    AMediaCodec*                    acodec    = nullptr;
    AMediaMuxer*                    muxer     = nullptr;
    int                             out_fd    = -1;

    ~VidRes() {
        if (session)   ACameraCaptureSession_close(session);
        if (sess_cont && sess_out)
            ACaptureSessionOutputContainer_remove(sess_cont, sess_out);
        if (sess_out)  ACaptureSessionOutput_free(sess_out);
        if (sess_cont) ACaptureSessionOutputContainer_free(sess_cont);
        if (device)    ACameraDevice_close(device);
        if (mgr)       ACameraManager_delete(mgr);
        if (vcodec)    { AMediaCodec_stop(vcodec); AMediaCodec_delete(vcodec); }
        if (acodec)    { AMediaCodec_stop(acodec); AMediaCodec_delete(acodec); }
        if (muxer)     AMediaMuxer_delete(muxer);
        if (enc_win)   ANativeWindow_release(enc_win);
        if (out_fd>=0) close(out_fd);
    }
};

void record_video(VideoFacing facing, int duration_sec,
                  const std::string& tmp_dir,
                  const std::atomic<bool>& cancel,
                  RecordCallback callback) {

    auto ctx = std::make_shared<VidCtx>();
    VidRes res;

    // Output file
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string out_path = tmp_dir + "/stonx_" + std::to_string(ts) + ".mp4";
    res.out_fd = open(out_path.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (res.out_fd < 0) { callback(RecordResult::RECORD_FAILED, ""); return; }

    // Video codec
    res.vcodec = AMediaCodec_createEncoderByType("video/avc");
    if (!res.vcodec) { callback(RecordResult::RECORD_FAILED, ""); return; }
    {
        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME,       "video/avc");
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_WIDTH,       1280);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_HEIGHT,       720);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_BIT_RATE,    Config::VIDEO_BITRATE_DEFAULT);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_FRAME_RATE,  Config::VIDEO_FRAME_RATE_DEFAULT);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7f000789);
        media_status_t r = AMediaCodec_configure(res.vcodec, fmt, nullptr, nullptr,
                                                 AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);
        if (r != AMEDIA_OK) { callback(RecordResult::RECORD_FAILED, ""); return; }
    }
    AMediaCodec_createInputSurface(res.vcodec, &res.enc_win);
    if (!res.enc_win) { callback(RecordResult::RECORD_FAILED, ""); return; }
    AMediaCodec_start(res.vcodec);

    // Audio codec
    constexpr int SR = 44100, CH = 1;
    bool has_audio = false;
    res.acodec = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (res.acodec) {
        AMediaFormat* af = AMediaFormat_new();
        AMediaFormat_setString(af, AMEDIAFORMAT_KEY_MIME,         "audio/mp4a-latm");
        AMediaFormat_setInt32 (af, AMEDIAFORMAT_KEY_SAMPLE_RATE,   SR);
        AMediaFormat_setInt32 (af, AMEDIAFORMAT_KEY_CHANNEL_COUNT,  CH);
        AMediaFormat_setInt32 (af, AMEDIAFORMAT_KEY_BIT_RATE,      128000);
        AMediaFormat_setInt32 (af, AMEDIAFORMAT_KEY_AAC_PROFILE,   2);
        has_audio = (AMediaCodec_configure(res.acodec, af, nullptr, nullptr,
                      AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK);
        AMediaFormat_delete(af);
        if (has_audio) AMediaCodec_start(res.acodec);
    }

    // MediaMuxer
    res.muxer = AMediaMuxer_new(res.out_fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!res.muxer) { callback(RecordResult::RECORD_FAILED, ""); return; }

    // Camera — SYNCHRONOUS open (no onOpened callback)
    res.mgr = ACameraManager_create();
    if (!res.mgr) { callback(RecordResult::NO_CAMERA, ""); return; }

    std::string cam_id = find_cam(res.mgr, facing);
    if (cam_id.empty()) { callback(RecordResult::NO_CAMERA, ""); return; }

    // ACameraDevice_StateCallbacks: ONLY context, onDisconnected, onError
    ACameraDevice_StateCallbacks dcbs{};
    dcbs.context        = ctx.get();
    dcbs.onDisconnected = v_dev_disc;
    dcbs.onError        = v_dev_err;

    // camera opens synchronously, device returned via &res.device
    camera_status_t open_st = ACameraManager_openCamera(
        res.mgr, cam_id.c_str(), &dcbs, &res.device);
    if (open_st != ACAMERA_OK || !res.device) {
        LOGE("openCamera failed: %d", open_st);
        callback(RecordResult::NO_CAMERA, "");
        return;
    }

    // Capture session → encoder surface
    ACaptureSessionOutput_create(res.enc_win, &res.sess_out);
    ACaptureSessionOutputContainer_create(&res.sess_cont);
    ACaptureSessionOutputContainer_add(res.sess_cont, res.sess_out);

    ACameraCaptureSession_stateCallbacks scbs{};
    scbs.context  = ctx.get();
    scbs.onReady  = v_sess_ready;
    scbs.onClosed = v_sess_close;
    scbs.onActive = v_sess_act;
    ACameraDevice_createCaptureSession(res.device, res.sess_cont, &scbs, &res.session);
    ctx->cam_session = res.session;

    {
        std::unique_lock<std::mutex> lk(ctx->mu);
        if (!ctx->cv.wait_for(lk, std::chrono::seconds(5),
            [&]{ return ctx->sess_ready || ctx->cam_error; })
            || !ctx->sess_ready)
            { callback(RecordResult::RECORD_FAILED, ""); return; }
    }

    // Repeating capture
    {
        ACaptureRequest* req = nullptr;
        ACameraDevice_createCaptureRequest(res.device, TEMPLATE_RECORD, &req);
        ACameraOutputTarget* tgt = nullptr;
        ACameraOutputTarget_create(res.enc_win, &tgt);
        ACaptureRequest_addTarget(req, tgt);
        ACameraCaptureSession_setRepeatingRequest(ctx->cam_session,nullptr,1,&req,nullptr);
        ACaptureRequest_removeTarget(req, tgt);
        ACameraOutputTarget_free(tgt);
        ACaptureRequest_free(req);
    }

    LOGI("recording %ds...", duration_sec);
    ctx->recording.store(true);

    ssize_t video_track = -1, audio_track = -1;
    bool muxer_started = false;

    std::thread audio_thread;
    if (has_audio) {
        audio_thread = std::thread([&]() {
            AAudioStreamBuilder* builder = nullptr;
            AAudio_createStreamBuilder(&builder);
            if (!builder) return;
            AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
            AAudioStreamBuilder_setSampleRate(builder, SR);
            AAudioStreamBuilder_setChannelCount(builder, CH);
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
            AAudioStream* stream = nullptr;
            if (AAudioStreamBuilder_openStream(builder, &stream) != AAUDIO_OK) {
                AAudioStreamBuilder_delete(builder); return;
            }
            AAudioStream_requestStart(stream);
            AAudioStreamBuilder_delete(builder);

            int16_t pcm[4096];
            int64_t audio_pts = 0;
            const int64_t pts_inc = (int64_t)4096 * 1000000LL / SR;

            while (ctx->recording.load() && !cancel.load()) {
                int32_t n = AAudioStream_read(stream, pcm, 4096, 100000000LL);
                if (n <= 0) continue;
                ssize_t in_idx = AMediaCodec_dequeueInputBuffer(res.acodec, 10000);
                if (in_idx >= 0) {
                    size_t in_sz = 0;
                    uint8_t* in_buf = AMediaCodec_getInputBuffer(res.acodec, in_idx, &in_sz);
                    size_t copy = std::min((size_t)n * 2, in_sz);
                    memcpy(in_buf, pcm, copy);
                    AMediaCodec_queueInputBuffer(res.acodec, in_idx, 0, copy, audio_pts, 0);
                    audio_pts += pts_inc;
                }
                AMediaCodecBufferInfo info{};
                ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(res.acodec, &info, 0);
                if (out_idx >= 0) {
                    if (muxer_started && audio_track >= 0
                        && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
                        size_t sz = 0;
                        uint8_t* buf = AMediaCodec_getOutputBuffer(res.acodec, out_idx, &sz);
                        info.size = (int32_t)std::min((size_t)info.size, sz);
                        AMediaMuxer_writeSampleData(res.muxer, audio_track, buf, &info);
                    } else if (audio_track < 0
                               && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
                        AMediaFormat* af = AMediaCodec_getOutputFormat(res.acodec);
                        audio_track = AMediaMuxer_addTrack(res.muxer, af);
                        AMediaFormat_delete(af);
                    }
                    AMediaCodec_releaseOutputBuffer(res.acodec, out_idx, false);
                } else if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                    AMediaFormat* af = AMediaCodec_getOutputFormat(res.acodec);
                    audio_track = AMediaMuxer_addTrack(res.muxer, af);
                    AMediaFormat_delete(af);
                }
            }
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
        });
    }

    // Video drain loop
    auto end_time = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    while (!cancel.load() && std::chrono::steady_clock::now() < end_time) {
        AMediaCodecBufferInfo info{};
        ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(res.vcodec, &info, 10000);
        if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* vf = AMediaCodec_getOutputFormat(res.vcodec);
            video_track = AMediaMuxer_addTrack(res.muxer, vf);
            AMediaFormat_delete(vf);
            if (video_track >= 0 && (!has_audio || audio_track >= 0)) {
                AMediaMuxer_start(res.muxer); muxer_started = true;
            }
        } else if (out_idx >= 0) {
            if (muxer_started && video_track >= 0
                && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
                size_t sz = 0;
                uint8_t* buf = AMediaCodec_getOutputBuffer(res.vcodec, out_idx, &sz);
                info.size = (int32_t)std::min((size_t)info.size, sz);
                AMediaMuxer_writeSampleData(res.muxer, video_track, buf, &info);
            } else if (!muxer_started && video_track < 0
                       && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
                AMediaFormat* vf = AMediaCodec_getOutputFormat(res.vcodec);
                video_track = AMediaMuxer_addTrack(res.muxer, vf);
                AMediaFormat_delete(vf);
                if (video_track >= 0 && (!has_audio || audio_track >= 0)) {
                    AMediaMuxer_start(res.muxer); muxer_started = true;
                }
            }
            AMediaCodec_releaseOutputBuffer(res.vcodec, out_idx, false);
        }
    }

    ctx->recording.store(false);
    if (audio_thread.joinable()) audio_thread.join();
    if (muxer_started) AMediaMuxer_stop(res.muxer);

    LOGI("recording done: %s", out_path.c_str());
    callback(RecordResult::OK, out_path);
}

} // namespace VideoOps
