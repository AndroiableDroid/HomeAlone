package com.androiabledroid.smartlock;

import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(android.os.Bundle savedInstanceState) {
        registerPlugin(SmartLockNotificationPlugin.class);
        super.onCreate(savedInstanceState);
    }
}

