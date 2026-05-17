package com.androiabledroid.smartlock;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.widget.RemoteViews;
import com.androiabledroid.smartlock.R;

public class SmartLockWidgetProvider extends AppWidgetProvider {

    @Override
    public void onUpdate(Context context, AppWidgetManager appWidgetManager, int[] appWidgetIds) {
        for (int appWidgetId : appWidgetIds) {
            updateAppWidget(context, appWidgetManager, appWidgetId);
        }
    }

    static void updateAppWidget(Context context, AppWidgetManager appWidgetManager, int appWidgetId) {
        // Construct the RemoteViews object
        RemoteViews views = new RemoteViews(context.getPackageName(), R.layout.smart_lock_widget);

        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }

        // Intent for Unlock Button Click
        Intent unlockIntent = new Intent(Intent.ACTION_VIEW, Uri.parse("smartlock://unlock"));
        unlockIntent.setPackage(context.getPackageName());
        unlockIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        
        PendingIntent unlockPendingIntent = PendingIntent.getActivity(context, 101, unlockIntent, flags);
        views.setOnClickPendingIntent(R.id.btn_widget_unlock, unlockPendingIntent);

        // Intent for Water Pump Button Click
        Intent pumpIntent = new Intent(Intent.ACTION_VIEW, Uri.parse("smartlock://pump"));
        pumpIntent.setPackage(context.getPackageName());
        pumpIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        
        PendingIntent pumpPendingIntent = PendingIntent.getActivity(context, 102, pumpIntent, flags);
        views.setOnClickPendingIntent(R.id.btn_widget_pump, pumpPendingIntent);

        // Instruct the widget manager to update the widget
        appWidgetManager.updateAppWidget(appWidgetId, views);
    }
}
