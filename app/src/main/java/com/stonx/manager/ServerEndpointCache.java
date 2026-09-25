package com.stonx.manager;

import android.content.Context;
import android.content.SharedPreferences;

public class ServerEndpointCache {

    private static final String TAG        = "EndpointCache";
    private static final String PREFS_NAME = "stonx_endpoint_cache";
    private static final String KEY_IP     = "server_ip";
    private static final String KEY_PORT   = "server_port";
    private static final String KEY_TS     = "last_update";
    private static final long   TTL_MS     = 24L * 60L * 60L * 1000L;
    private static final int    NO_PORT    = -1;

    public static class Entry {
        public final String host;
        public final int    port;
        private Entry(String host, int port) { this.host = host; this.port = port; }
    }

    public static Entry load(Context context) {
        SharedPreferences prefs = getPrefs(context);
        String ip   = prefs.getString(KEY_IP, null);
        int    port = prefs.getInt(KEY_PORT, NO_PORT);
        long   ts   = prefs.getLong(KEY_TS, 0L);
        if (!isValid(ip, port)) { StonxLog.d(TAG, "load: no valid cache"); return null; }
        if (ts <= 0L)           { StonxLog.d(TAG, "load: no timestamp");    return null; }
        long ageMs = System.currentTimeMillis() - ts;
        if (ageMs >= TTL_MS) {
            StonxLog.d(TAG, "load: cache expired (" + ageMs/3_600_000L + "h) → need refresh");
            return null;
        }
        StonxLog.d(TAG, "load: cache hit → " + ip + ":" + port + " (age=" + ageMs/60_000L + "min)");
        return new Entry(ip.trim(), port);
    }

    public static Entry loadStale(Context context) {
        SharedPreferences prefs = getPrefs(context);
        String ip   = prefs.getString(KEY_IP, null);
        int    port = prefs.getInt(KEY_PORT, NO_PORT);
        if (!isValid(ip, port)) { StonxLog.d(TAG, "loadStale: no cache exists"); return null; }
        StonxLog.d(TAG, "loadStale: fallback → " + ip + ":" + port);
        return new Entry(ip.trim(), port);
    }

    public static void save(Context context, String host, int port) {
        getPrefs(context).edit()
                .putString(KEY_IP,  host)
                .putInt(KEY_PORT,   port)
                .putLong(KEY_TS,    System.currentTimeMillis())
                .apply();
        StonxLog.d(TAG, "save: endpoint saved → " + host + ":" + port);
    }

    private static boolean isValid(String ip, int port) {
        return ip != null && !ip.trim().isEmpty() && port >= 1 && port <= 65535;
    }
    private static SharedPreferences getPrefs(Context context) {
        return context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }
}
