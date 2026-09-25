package com.stonx.manager;

import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.SurfaceTexture;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.app.Service;
import android.view.Surface;

import androidx.camera.core.CameraSelector;
import androidx.camera.core.ImageCapture;
import androidx.camera.core.ImageCaptureException;
import androidx.camera.core.Preview;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.core.content.ContextCompat;
import androidx.lifecycle.Lifecycle;
import androidx.lifecycle.LifecycleOwner;
import androidx.lifecycle.LifecycleRegistry;

import com.google.common.util.concurrent.ListenableFuture;

import java.io.File;

/**
 * CameraService — التقاط صورة باستخدام CameraX.
 * يستخدم LifecycleRegistry يدوي لتجنب دورات lifecycle غير متوقعة.
 */
public class CameraService extends Service {

    private static final String TAG = "STONX_CAM";

    public static final String EXTRA_ACTION_TYPE = "action_type";
    public static final String EXTRA_FACING      = "facing";
    public static final String EXTRA_OUTPUT       = "output";
    public static final String EXTRA_OP_ID        = "op_id";

    private String outPath;
    private String opId;

    private LifecycleRegistry lifecycleRegistry;
    private final LifecycleOwner lifecycleOwner = () -> lifecycleRegistry;

    private ProcessCameraProvider cameraProvider;
    private SurfaceTexture        dummyTexture;
    private HandlerThread         bgThread;
    private Handler               bgHandler;

    // ════════════════════════════════════════════════════════════════════
    //  Lifecycle
    // ════════════════════════════════════════════════════════════════════

    @Override
    public void onCreate() {
        super.onCreate();
        StonxLog.d(TAG, "*** CameraService.onCreate API=" + Build.VERSION.SDK_INT + " ***");

        bgThread = new HandlerThread("cam-bg");
        bgThread.start();
        bgHandler = new Handler(bgThread.getLooper());

        lifecycleRegistry = new LifecycleRegistry(lifecycleOwner);
        lifecycleRegistry.setCurrentState(Lifecycle.State.CREATED);

        android.app.Notification notif = NotifyHelper.buildService(this, true);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NotifyHelper.NOTIF_SERVICE + 10, notif,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA);
                StonxLog.d(TAG, "*** startForeground OK (camera) ***");
            } else {
                startForeground(NotifyHelper.NOTIF_SERVICE + 10, notif);
                StonxLog.d(TAG, "*** startForeground OK ***");
            }
        } catch (Exception e) {
            StonxLog.e(TAG, "startForeground failed: " + e.getMessage());
            try { startForeground(NotifyHelper.NOTIF_SERVICE + 10, notif); }
            catch (Exception ignored) {}
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) { stopSelf(); return START_NOT_STICKY; }

        outPath = intent.getStringExtra(EXTRA_OUTPUT);
        opId    = intent.getStringExtra(EXTRA_OP_ID);
        String facing = intent.getStringExtra(EXTRA_FACING);
        boolean wantFront = "front".equalsIgnoreCase(facing);

        StonxLog.d(TAG, "*** onStartCommand facing=" + facing
                + " out=" + outPath + " ***");

        if (outPath == null || outPath.isEmpty()) {
            notifyFailure("MISSING_OUTPUT");
            stopSelf();
            return START_NOT_STICKY;
        }

        new Handler(getMainLooper()).post(() -> {
            lifecycleRegistry.setCurrentState(Lifecycle.State.STARTED);
            startCapture(wantFront);
        });

        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onDestroy() {
        StonxLog.d(TAG, "*** CameraService.onDestroy ***");
        releaseAll();
        lifecycleRegistry.setCurrentState(Lifecycle.State.DESTROYED);
        super.onDestroy();
    }

    // ════════════════════════════════════════════════════════════════════
    //  CameraX Capture
    // ════════════════════════════════════════════════════════════════════

    private void startCapture(boolean wantFront) {
        ListenableFuture<ProcessCameraProvider> future =
                ProcessCameraProvider.getInstance(this);

        future.addListener(() -> {
            try {
                cameraProvider = future.get();

                // Preview وهمي لتثبيت camera session
                dummyTexture = new SurfaceTexture(0);
                dummyTexture.setDefaultBufferSize(320, 240);
                Surface surface = new Surface(dummyTexture);
                Preview preview = new Preview.Builder().build();
                preview.setSurfaceProvider(req ->
                        req.provideSurface(surface,
                                ContextCompat.getMainExecutor(this),
                                r -> surface.release()));

                ImageCapture imageCapture = new ImageCapture.Builder()
                        .setCaptureMode(ImageCapture.CAPTURE_MODE_MAXIMIZE_QUALITY)
                        .build();

                CameraSelector selector = wantFront
                        ? CameraSelector.DEFAULT_FRONT_CAMERA
                        : CameraSelector.DEFAULT_BACK_CAMERA;

                cameraProvider.unbindAll();
                cameraProvider.bindToLifecycle(
                        lifecycleOwner, selector, preview, imageCapture);

                StonxLog.d(TAG, "camera bound → waiting for warmup");

                new Handler(getMainLooper()).postDelayed(
                        () -> takePicture(imageCapture), 1000);

            } catch (Exception e) {
                StonxLog.e(TAG, "camera setup failed: " + e.getMessage());
                notifyFailure("SETUP_ERROR");
                stopSelf();
            }
        }, ContextCompat.getMainExecutor(this));
    }

    private void takePicture(ImageCapture imageCapture) {
        File outputFile = new File(outPath);
        File dir = outputFile.getParentFile();
        if (dir != null && !dir.exists()) dir.mkdirs();

        StonxLog.d(TAG, "takePicture → " + outPath);

        imageCapture.takePicture(
                new ImageCapture.OutputFileOptions.Builder(outputFile).build(),
                ContextCompat.getMainExecutor(this),
                new ImageCapture.OnImageSavedCallback() {
                    @Override
                    public void onImageSaved(ImageCapture.OutputFileResults r) {
                        StonxLog.d(TAG, "photo saved ✓ " + outPath
                                + " (" + outputFile.length() + "B)");
                        releaseAll();
                        notifySuccess(outPath);
                        stopSelf();
                    }

                    @Override
                    public void onError(ImageCaptureException e) {
                        StonxLog.e(TAG, "capture error "
                                + e.getImageCaptureError() + ": " + e.getMessage());
                        releaseAll();
                        notifyFailure("CAPTURE_ERROR_" + e.getImageCaptureError());
                        stopSelf();
                    }
                }
        );
    }

    private void releaseAll() {
        if (cameraProvider != null) {
            try { cameraProvider.unbindAll(); } catch (Exception ignored) {}
            cameraProvider = null;
        }
        if (dummyTexture != null) {
            try { dummyTexture.release(); } catch (Exception ignored) {}
            dummyTexture = null;
        }
        if (bgThread != null) {
            bgThread.quitSafely();
            bgThread = null;
        }
    }

    // ════════════════════════════════════════════════════════════════════
    //  Result → StonxService → C++
    // ════════════════════════════════════════════════════════════════════

    private void notifySuccess(String path) {
        StonxService.deliverCameraResult(opId, true, path);
    }

    private void notifyFailure(String reason) {
        StonxService.deliverCameraResult(opId, false, reason);
    }
}

