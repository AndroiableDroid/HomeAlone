package com.androiabledroid.smartlock;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.os.Build;

import androidx.core.app.NotificationCompat;

import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

/**
 * SmartLockNotificationPlugin
 *
 * A thin Capacitor plugin that schedules / cancels a persistent (ongoing)
 * notification with native action buttons whose PendingIntents fire the
 * NotificationActionReceiver — NOT the MainActivity.
 *
 * Angular call:
 *   SmartLockNotification.show({ title, body })  → shows / updates notification
 *   SmartLockNotification.dismiss()              → cancels the notification
 */
@CapacitorPlugin(name = "SmartLockNotification")
public class SmartLockNotificationPlugin extends Plugin {

    private static final int NOTIFICATION_ID  = 888;
    private static final String CHANNEL_ID    = "smart_lock_persistent";
    private static final String CHANNEL_NAME  = "Smart Lock Status";

    @Override
    public void load() {
        createNotificationChannel();
    }

    // ------------------------------------------------------------------
    // show({ title: string, body: string })
    // ------------------------------------------------------------------
    @PluginMethod
    public void show(PluginCall call) {
        String title = call.getString("title", "🏠 HomeAlone Smart Lock");
        String body  = call.getString("body",  "Quick actions for door & water pump");

        Context ctx = getContext();

        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }

        // --- Action: Unlock (fires broadcast → receiver, NO app launch) ---
        Intent unlockIntent = new Intent(NotificationActionReceiver.ACTION_UNLOCK);
        unlockIntent.setClass(ctx, NotificationActionReceiver.class);
        PendingIntent unlockPending = PendingIntent.getBroadcast(ctx, 301, unlockIntent, flags);

        // --- Action: Pump toggle ---
        Intent pumpIntent = new Intent(NotificationActionReceiver.ACTION_PUMP);
        pumpIntent.setClass(ctx, NotificationActionReceiver.class);
        PendingIntent pumpPending = PendingIntent.getBroadcast(ctx, 302, pumpIntent, flags);

        // --- Tap on notification body → open app (expected behaviour for body tap) ---
        Intent openAppIntent = new Intent(ctx, MainActivity.class);
        openAppIntent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent openAppPending = PendingIntent.getActivity(ctx, 303, openAppIntent, flags);

        NotificationCompat.Builder builder = new NotificationCompat.Builder(ctx, CHANNEL_ID)
            .setSmallIcon(getSmallIconId(ctx))
            .setContentTitle(title)
            .setContentText(body)
            .setStyle(new NotificationCompat.BigTextStyle().bigText(body))
            .setOngoing(true)           // Cannot be swiped away
            .setAutoCancel(false)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setContentIntent(openAppPending)
            .addAction(0, "🔓 Unlock Front Door", unlockPending)
            .addAction(0, "🚰 Toggle Water Pump", pumpPending);

        NotificationManager mgr = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (mgr != null) {
            mgr.notify(NOTIFICATION_ID, builder.build());
        }

        call.resolve();
    }

    // ------------------------------------------------------------------
    // dismiss()
    // ------------------------------------------------------------------
    @PluginMethod
    public void dismiss(PluginCall call) {
        NotificationManager mgr = (NotificationManager)
            getContext().getSystemService(Context.NOTIFICATION_SERVICE);
        if (mgr != null) {
            mgr.cancel(NOTIFICATION_ID);
        }
        call.resolve();
    }

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------
    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                CHANNEL_NAME,
                NotificationManager.IMPORTANCE_LOW   // Silent but visible
            );
            channel.setDescription("Persistent Smart Lock status & quick-action notification");
            channel.setShowBadge(false);
            channel.enableLights(false);
            channel.enableVibration(false);

            NotificationManager mgr = getContext().getSystemService(NotificationManager.class);
            if (mgr != null) mgr.createNotificationChannel(channel);
        }
    }

    private int getSmallIconId(Context ctx) {
        // Try custom ic_stat_lock; fall back to a standard Android icon
        int id = ctx.getResources().getIdentifier("ic_stat_lock", "drawable", ctx.getPackageName());
        return id != 0 ? id : android.R.drawable.ic_lock_idle_lock;
    }
}
