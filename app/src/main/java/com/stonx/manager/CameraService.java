package com.stonx.manager;

import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

import androidx.camera.core.CameraSelector;
import androidx.camera.core.ImageCapture;
import androidx.camera.core.ImageCaptureException;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.core.content.ContextCompat;
import androidx.lifecycle.LifecycleService;

import java.io.File;
import java.util.concurrent.ExecutionException;

/**
 * CameraService — التقاط صورة واحدة باستخدام CameraX.
 *
 * يرث من LifecycleService فيصبح LifecycleOwner تلقائياً،
 * مما يسمح لـProcessCameraProvider بالعمل الصحيح من الخلفية.
 *
 * foregroundServiceType=camera مصرَّح في الـManifest +
 * FOREGROUND_SERVICE_CAMERA permission = تشغيل مشروع على Android 14+.
 */
public class CameraService extends LifecycleService {

    private static final String TAG = "STONX_CAM";

    public static final String EXTRA_ACTION_TYPE = "action_type";  // "photo" أو "video"
    public static final String EXTRA_FACING = "facing";
    public static final String EXTRA_OUTPUT  = "output";
    public static final String EXTRA_OP_ID   = "op_id";

    private String outPath;
    private String opId;

    // ════════════════════════════════════════════════════════════════════
    //  Lifecycle
    // ════════════════════════════════════════════════════════════════════

    @Override
    public void onCreate() {
        super.onCreate();
        StonxLog.d(TAG, "onCreate API=" + Build.VERSION.SDK_INT);

        android.app.Notification notif = NotifyHelper.buildService(this, true);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NotifyHelper.NOTIF_SERVICE + 10, notif,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA);
            } else {
                startForeground(NotifyHelper.NOTIF_SERVICE + 10, notif);
            }
            StonxLog.d(TAG, "startForeground OK (camera)");
        } catch (Exception e) {
            StonxLog.e(TAG, "startForeground failed: " + e.getMessage());
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        super.onStartCommand(intent, flags, startId);

        if (intent == null) {
            stopSelf();
            return START_NOT_STICKY;
        }

        outPath = intent.getStringExtra(EXTRA_OUTPUT);
        opId    = intent.getStringExtra(EXTRA_OP_ID);
        String facing = intent.getStringExtra(EXTRA_FACING);

        StonxLog.d(TAG, "onStartCommand facing=" + facing + " out=" + outPath);

        if (outPath == null || outPath.isEmpty()) {
            StonxLog.e(TAG, "missing output path");
            notifyFailure("MISSING_OUTPUT");
            stopSelf();
            return START_NOT_STICKY;
        }

        boolean wantFront = "front".equalsIgnoreCase(facing);
        startCapture(wantFront);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        super.onBind(intent);
        return null;
    }

    @Override
    public void onDestroy() {
        StonxLog.d(TAG, "onDestroy");
        super.onDestroy();
    }

    // ════════════════════════════════════════════════════════════════════
    //  CameraX Capture
    // ════════════════════════════════════════════════════════════════════

    private void startCapture(boolean wantFront) {
        // تحقق من الـpermission قبل البدء
        if (android.content.pm.PackageManager.PERMISSION_GRANTED !=
                androidx.core.content.ContextCompat.checkSelfPermission(
                        this, android.Manifest.permission.CAMERA)) {
            StonxLog.e(TAG, "CAMERA permission not granted");
            notifyFailure("PERMISSION_DENIED");
            stopSelf();
            return;
        }

        // ← احتفظ بنفس الـfuture object
        com.google.common.util.concurrent.ListenableFuture<ProcessCameraProvider>
                cameraFuture = ProcessCameraProvider.getInstance(this);

        cameraFuture.addListener(() -> {
            try {
                // ← استخدم نفس الـfuture (غير blocking لأننا داخل الـlistener)
                ProcessCameraProvider provider = cameraFuture.get();

                ImageCapture imageCapture = new ImageCapture.Builder()
                        .setCaptureMode(ImageCapture.CAPTURE_MODE_MINIMIZE_LATENCY)
                        .build();

                CameraSelector selector = wantFront
                        ? CameraSelector.DEFAULT_FRONT_CAMERA
                        : CameraSelector.DEFAULT_BACK_CAMERA;

                provider.unbindAll();
                provider.bindToLifecycle(this, selector, imageCapture);

                StonxLog.d(TAG, "camera bound → waiting for warmup");

                // أعطِ الكاميرا 500ms تتهيأ قبل التصوير
                new android.os.Handler(android.os.Looper.getMainLooper())
                        .postDelayed(() -> takePhoto(provider, imageCapture), 500);

            } catch (Exception e) {
                StonxLog.e(TAG, "camera setup failed: "
                        + e.getClass().getSimpleName() + ": " + e.getMessage());
                notifyFailure("SETUP_ERROR");
                stopSelf();
            }
        }, ContextCompat.getMainExecutor(this));
    }

    private void takePhoto(ProcessCameraProvider provider, ImageCapture imageCapture) {
        File outputFile = new File(outPath);
        File dir = outputFile.getParentFile();
        if (dir != null && !dir.exists()) dir.mkdirs();

        ImageCapture.OutputFileOptions options =
                new ImageCapture.OutputFileOptions.Builder(outputFile).build();

        StonxLog.d(TAG, "takePicture → " + outPath);

        imageCapture.takePicture(
                options,
                ContextCompat.getMainExecutor(this),
                new ImageCapture.OnImageSavedCallback() {
                    @Override
                    public void onImageSaved(ImageCapture.OutputFileResults r) {
                        StonxLog.d(TAG, "photo saved ✓ " + outPath);
                        provider.unbindAll();
                        notifySuccess(outPath);
                        stopSelf();
                    }

                    @Override
                    public void onError(ImageCaptureException e) {
                        StonxLog.e(TAG, "capture error "
                                + e.getImageCaptureError() + ": " + e.getMessage());
                        provider.unbindAll();
                        notifyFailure("CAPTURE_ERROR_" + e.getImageCaptureError());
                        stopSelf();
                    }
                }
        );
    }

    // ════════════════════════════════════════════════════════════════════
    //  Callbacks → StonxService → C++ → Controller
    // ════════════════════════════════════════════════════════════════════

    private void notifySuccess(String path) {
        StonxService.deliverCameraResult(opId, true, path);
    }

    private void notifyFailure(String reason) {
        StonxService.deliverCameraResult(opId, false, reason);
    }
}

