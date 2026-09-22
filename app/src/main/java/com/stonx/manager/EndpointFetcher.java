package com.stonx.manager;

import com.google.firebase.database.DataSnapshot;
import com.google.firebase.database.DatabaseError;
import com.google.firebase.database.DatabaseReference;
import com.google.firebase.database.FirebaseDatabase;
import com.google.firebase.database.ValueEventListener;

/**
 * يجلب server_ip وserver_port من Firebase Realtime Database.
 *
 * بنية البيانات المتوقعة في RTDB:
 *   stonx_config/
 *       server_ip:   "89.147.157.190"
 *       server_port: 4444
 *
 * لا يحتوي على Cache — سيُضاف في خطوة مستقلة.
 */
public class EndpointFetcher {

    private static final String TAG  = "EndpointFetcher";
    private static final String NODE = "stonx_config";

    public interface Callback {
        void onSuccess(String host, int port);
        void onFailure(String reason);
    }

    public static void fetch(Callback cb) {
        DatabaseReference ref = FirebaseDatabase.getInstance()
                                               .getReference(NODE);

        ref.addListenerForSingleValueEvent(new ValueEventListener() {

            @Override
            public void onDataChange(DataSnapshot snapshot) {
                // ── استخراج server_ip ─────────────────────────────────
                String ip = snapshot.child("server_ip").getValue(String.class);
                if (ip == null || ip.trim().isEmpty()) {
                    StonxLog.e(TAG, "fetch: server_ip missing or empty");
                    cb.onFailure("INVALID_HOST");
                    return;
                }
                ip = ip.trim();

                // ── استخراج server_port ───────────────────────────────
                Object portRaw = snapshot.child("server_port").getValue();
                if (portRaw == null) {
                    StonxLog.e(TAG, "fetch: server_port missing");
                    cb.onFailure("INVALID_PORT");
                    return;
                }

                int port;
                try {
                    // RTDB يُرجع Long للأرقام
                    port = (portRaw instanceof Long)
                            ? ((Long) portRaw).intValue()
                            : Integer.parseInt(portRaw.toString());
                } catch (NumberFormatException e) {
                    StonxLog.e(TAG, "fetch: server_port not a number: " + portRaw);
                    cb.onFailure("INVALID_PORT");
                    return;
                }

                // ── التحقق من النطاق ──────────────────────────────────
                if (port < 1 || port > 65535) {
                    StonxLog.e(TAG, "fetch: port out of range: " + port);
                    cb.onFailure("PORT_OUT_OF_RANGE");
                    return;
                }

                StonxLog.d(TAG, "fetch: OK → " + ip + ":" + port);
                cb.onSuccess(ip, port);
            }

            @Override
            public void onCancelled(DatabaseError error) {
                StonxLog.e(TAG, "fetch: cancelled — " + error.getMessage());
                cb.onFailure(error.getMessage());
            }
        });
    }
}
