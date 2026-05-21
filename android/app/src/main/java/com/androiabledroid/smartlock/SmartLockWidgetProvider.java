package com.androiabledroid.smartlock;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.Context;
import android.content.Intent;
import android.graphics.drawable.AnimatedVectorDrawable;
import android.graphics.drawable.Drawable;
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

        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }

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

        // --- Water pump animation ---
        // Set the AnimatedVectorDrawable on the ImageView.
        // RemoteViews.setImageViewResource() sets the src; the AVD auto-starts
        // because it is marked infinite in its animators. On API 31+ we can also
        // explicitly call startAnimation via RemoteViews method, but simply
        // setting the resource is enough for the AVD to loop on its own once
        // the widget host renders it.
        views.setImageViewResource(R.id.iv_pump_animation, R.drawable.ic_pump_animated);

        appWidgetManager.updateAppWidget(appWidgetId, views);
    }
}
