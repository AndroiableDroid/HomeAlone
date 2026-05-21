package com.androiabledroid.smartlock;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.AsyncTask;
import android.util.Log;

import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * NotificationActionReceiver
 *
 * Handles quick-action button clicks from:
 *  1. The persistent/ongoing notification (LOCK_ACTION_UNLOCK / LOCK_ACTION_PUMP)
 *  2. The home screen widget (smartlock://unlock / smartlock://pump deep links fired as broadcasts)
 *
 * Uses BroadcastReceiver so the app is NEVER brought to the foreground — actions
 * are executed silently in the background via direct Firebase RTDB REST calls.
 *
 * Firebase RTDB REST:
 *   PUT https://<DB>.firebaseio.com/door/status.json?auth=<token>   body: "UNLOCKED"
 *   PUT https://<DB>.firebaseio.com/pump/status.json?auth=<token>   body: "ON" / "OFF"
 *
 * NOTE: The auth token used here is the Firebase Database Secret (legacy admin token).
 *       Generate one from Firebase Console → Project Settings → Service Accounts → Database secrets.
 *       Paste it in strings.xml as <string name="firebase_db_secret">YOUR_SECRET</string>
 */
public class NotificationActionReceiver extends BroadcastReceiver {

    private static final String TAG = "LockActionReceiver";

    // Intent action constants (used by both notification and widget)
    public static final String ACTION_UNLOCK = "com.androiabledroid.smartlock.ACTION_UNLOCK";
    public static final String ACTION_PUMP   = "com.androiabledroid.smartlock.ACTION_PUMP";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || intent.getAction() == null) return;

        String action = intent.getAction();
        Log.d(TAG, "Received action: " + action);

        // Retrieve Firebase config from strings.xml (injected at build time)
        String dbUrl    = context.getString(R.string.firebase_database_url);
        String dbSecret = context.getString(R.string.firebase_db_secret);

        switch (action) {
            case ACTION_UNLOCK:
                Log.d(TAG, "Performing silent unlock...");
                new FirebaseRestTask(dbUrl + "/door/status.json?auth=" + dbSecret, "\"UNLOCKED\"").execute();
                break;

            case ACTION_PUMP:
                Log.d(TAG, "Performing silent pump toggle...");
                // Read current pump status first, then toggle
                new PumpToggleTask(dbUrl, dbSecret).execute();
                break;

            default:
                Log.w(TAG, "Unknown action: " + action);
        }
    }

    // -------------------------------------------------------------------------
    // FirebaseRestTask — simple async PUT to Firebase RTDB REST API
    // -------------------------------------------------------------------------
    private static class FirebaseRestTask extends AsyncTask<Void, Void, Boolean> {
        private final String url;
        private final String jsonBody;

        FirebaseRestTask(String url, String jsonBody) {
            this.url      = url;
            this.jsonBody = jsonBody;
        }

        @Override
        protected Boolean doInBackground(Void... voids) {
            return putJson(url, jsonBody);
        }

        @Override
        protected void onPostExecute(Boolean success) {
            Log.d(TAG, success ? "Firebase PUT success" : "Firebase PUT failed");
        }
    }

    // -------------------------------------------------------------------------
    // PumpToggleTask — GET current status then PUT the toggled state
    // -------------------------------------------------------------------------
    private static class PumpToggleTask extends AsyncTask<Void, Void, Boolean> {
        private final String baseUrl;
        private final String secret;

        PumpToggleTask(String baseUrl, String secret) {
            this.baseUrl = baseUrl;
            this.secret  = secret;
        }

        @Override
        protected Boolean doInBackground(Void... voids) {
            try {
                String statusUrl = baseUrl + "/pump/status.json?auth=" + secret;

                // 1. GET current pump status
                URL url = new URL(statusUrl);
                HttpURLConnection conn = (HttpURLConnection) url.openConnection();
                conn.setRequestMethod("GET");
                conn.setConnectTimeout(5000);
                conn.setReadTimeout(5000);

                String currentStatus = "OFF";
                if (conn.getResponseCode() == 200) {
                    java.io.BufferedReader reader = new java.io.BufferedReader(
                        new java.io.InputStreamReader(conn.getInputStream()));
                    StringBuilder sb = new StringBuilder();
                    String line;
                    while ((line = reader.readLine()) != null) sb.append(line);
                    reader.close();
                    // Firebase returns "ON", "OFF", "RUNNING", "STOPPED" (with quotes)
                    currentStatus = sb.toString().replace("\"", "").trim();
                }
                conn.disconnect();

                // 2. Determine new state
                boolean isRunning = currentStatus.equals("ON") || currentStatus.equals("RUNNING");
                String newState = isRunning ? "\"OFF\"" : "\"ON\"";
                Log.d(TAG, "Pump current: " + currentStatus + " → toggling to: " + newState);

                // 3. PUT new state
                return putJson(statusUrl, newState);

            } catch (Exception e) {
                Log.e(TAG, "PumpToggleTask failed", e);
                return false;
            }
        }

        @Override
        protected void onPostExecute(Boolean success) {
            Log.d(TAG, "Pump toggle " + (success ? "success" : "failed"));
        }
    }

    // -------------------------------------------------------------------------
    // Shared HTTP PUT helper
    // -------------------------------------------------------------------------
    private static boolean putJson(String urlString, String jsonBody) {
        try {
            URL url = new URL(urlString);
            HttpURLConnection conn = (HttpURLConnection) url.openConnection();
            conn.setRequestMethod("PUT");
            conn.setRequestProperty("Content-Type", "application/json");
            conn.setDoOutput(true);
            conn.setConnectTimeout(5000);
            conn.setReadTimeout(5000);

            try (OutputStream os = conn.getOutputStream()) {
                os.write(jsonBody.getBytes("UTF-8"));
            }

            int responseCode = conn.getResponseCode();
            conn.disconnect();
            return responseCode >= 200 && responseCode < 300;
        } catch (Exception e) {
            Log.e(TAG, "putJson failed for " + urlString, e);
            return false;
        }
    }
}
