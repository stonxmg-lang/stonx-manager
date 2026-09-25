package com.stonx.manager;

import com.google.firebase.FirebaseApp;
import com.google.firebase.database.DataSnapshot;
import com.google.firebase.database.DatabaseError;
import com.google.firebase.database.DatabaseReference;
import com.google.firebase.database.FirebaseDatabase;
import com.google.firebase.database.ValueEventListener;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

public class EndpointFetcher {

    private static final String TAG      = "EndpointFetcher";
    private static final String NODE     = "stonx_config";
    private static final String LOG_FILE = "endpoint_fetch.log";

    private static final SimpleDateFormat TS_FMT =
            new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);

    public interface Callback {
        void onSuccess(String host, int port);
        void onFailure(String reason);
    }

    private static synchronized void log(String level, String msg) {
        if ("ERROR".equals(level)) {
            StonxLog.e(TAG, "[" + level + "] " + msg);
        } else {
            StonxLog.d(TAG, "[" + level + "] " + msg);
        }
        try {
            File dir = FirebaseApp.getInstance()
                                  .getApplicationContext()
                                  .getFilesDir();
            File logFile = new File(dir, LOG_FILE);
            String line = TS_FMT.format(new Date()) + " | " + level + " | " + msg;
            try (BufferedWriter bw = new BufferedWriter(new FileWriter(logFile, true))) {
                bw.write(line);
                bw.newLine();
            }
        } catch (Exception e) {
            StonxLog.e(TAG, "log write failed: " + e.getMessage());
        }
    }

    public static void fetch(Callback cb) {
        log("START", "Firebase endpoint fetch started");
        DatabaseReference ref = FirebaseDatabase
                .getInstance("https://mariam-8ec9d-default-rtdb.europe-west1.firebasedatabase.app/")
                .getReference(NODE);
        log("READ", "Reading " + NODE);
        ref.addListenerForSingleValueEvent(new ValueEventListener() {
            @Override
            public void onDataChange(DataSnapshot snapshot) {
                try {
                    String ip = snapshot.child("server_ip").getValue(String.class);
                    if (ip == null || ip.trim().isEmpty()) {
                        log("ERROR", "server_ip missing or empty");
                        cb.onFailure("INVALID_HOST"); return;
                    }
                    ip = ip.trim();
                    log("DATA", "server_ip received: " + ip);

                    Object portRaw = snapshot.child("server_port").getValue();
                    if (portRaw == null) {
                        log("ERROR", "server_port missing");
                        cb.onFailure("INVALID_PORT"); return;
                    }
                    int port;
                    try {
                        port = (portRaw instanceof Long)
                                ? ((Long) portRaw).intValue()
                                : Integer.parseInt(portRaw.toString());
                    } catch (NumberFormatException e) {
                        log("ERROR", "server_port not a number: " + portRaw);
                        cb.onFailure("INVALID_PORT"); return;
                    }
                    log("DATA", "server_port received: " + port);
                    if (port < 1 || port > 65535) {
                        log("ERROR", "port out of range: " + port);
                        cb.onFailure("PORT_OUT_OF_RANGE"); return;
                    }
                    log("SUCCESS", "Endpoint validated: " + ip + ":" + port);
                    log("SUCCESS", "Calling onSuccess");
                    cb.onSuccess(ip, port);
                } catch (Exception e) {
                    log("ERROR", "Unexpected exception: " + e.getMessage());
                    cb.onFailure("EXCEPTION");
                }
            }
            @Override
            public void onCancelled(DatabaseError error) {
                log("ERROR", "Firebase cancelled: " + error.getMessage()
                            + " (code=" + error.getCode() + ")");
                cb.onFailure(error.getMessage());
            }
        });
    }
}
