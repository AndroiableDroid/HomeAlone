package com.androiabledroid.smartlock;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.widget.RemoteViews;

public class SmartLockWidgetProvider extends AppWidgetProvider {

    @Override
    public void onUpdate(Context context, AppWidgetManager appWidgetManager, int[] appWidgetIds) {
        for (int appWidgetId : appWidgetIds) {
            updateAppWidget(context, appWidgetManager, appWidgetId);
        }
    }

    static void updateAppWidget(Context context, AppWidgetManager appWidgetManager, int appWidgetId) {
        RemoteViews views = new RemoteViews(context.getPackageName(), R.layout.smart_lock_widget);

        // Use FLAG_MUTABLE on Android 12+ so the system can fill in missing extras,
        // but keep FLAG_IMMUTABLE on older versions.
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
      flags |= PendingIntent.FLAG_IMMUTABLE;

      // --- Unlock button ---
        // Fire a broadcast → NotificationActionReceiver handles it silently (no app launch)
        Intent unlockIntent = new Intent(NotificationActionReceiver.ACTION_UNLOCK);
        unlockIntent.setClass(context, NotificationActionReceiver.class);
        PendingIntent unlockPending = PendingIntent.getBroadcast(context, 201, unlockIntent, flags);
        views.setOnClickPendingIntent(R.id.btn_widget_unlock, unlockPending);

        // --- Pump toggle button ---
        Intent pumpIntent = new Intent(NotificationActionReceiver.ACTION_PUMP);
        pumpIntent.setClass(context, NotificationActionReceiver.class);
        PendingIntent pumpPending = PendingIntent.getBroadcast(context, 202, pumpIntent, flags);
        views.setOnClickPendingIntent(R.id.btn_widget_pump, pumpPending);

        appWidgetManager.updateAppWidget(appWidgetId, views);
    }
}

