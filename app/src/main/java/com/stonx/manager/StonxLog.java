package com.stonx.manager;

import android.util.Log;

import java.io.File;
import java.io.FileWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * StonxLog — يسجّل في مكانين:
 *   1. Logcat (للمطورين)
 *   2. ملف مرئي في /storage/emulated/0/STONX/stonx.log
 *      (يمكن الوصول إليه من أي مدير ملفات)
 */
final class StonxLog {

    private static final String TAG = "STONX";
    private static final String LOG_DIR  = "/storage/emulated/0/STONX";
    private static final String LOG_PATH = LOG_DIR + "/stonx.log";
    private static final SimpleDateFormat TS =
            new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US);

    private StonxLog() {}

    static void d(String tag, String msg) {
        Log.d(tag, msg);
        write("D", tag, msg);
    }

    static void e(String tag, String msg) {
        Log.e(tag, msg);
        write("E", tag, msg);
    }

    static void e(String tag, String msg, Throwable t) {
        Log.e(tag, msg, t);
        write("E", tag, msg + " :: " + (t != null ? t.toString() : ""));
    }

    private static synchronized void write(String level, String tag, String msg) {
        try {
            File dir = new File(LOG_DIR);
            if (!dir.exists()) dir.mkdirs();

            File f = new File(LOG_PATH);
            try (FileWriter w = new FileWriter(f, true)) {
                w.write(TS.format(new Date()) + "  " + level + "/" + tag
                        + "  " + msg + "\n");
            }
        } catch (Exception ignored) {
            // لا نوقف التطبيق لو فشلت الكتابة
        }
    }
}
