import { Injectable, inject, NgZone } from '@angular/core';
import { Router } from '@angular/router';
import { Capacitor } from '@capacitor/core';
import { Subscription, combineLatest } from 'rxjs';
import { AuthService } from './auth-service';
import { LockService } from './lock-service';
import { LocalAuthService } from './local-auth.service';
import { SmartLockNotification } from '../plugins/smart-lock-notification.plugin';

/**
 * NotificationService
 *
 * Manages the persistent "HomeAlone Smart Lock" notification that appears in
 * the Android status bar whenever the user is logged in.
 *
 * Action buttons (Unlock / Pump) are now handled by the native
 * SmartLockNotificationPlugin which wires them to NotificationActionReceiver
 * via broadcast PendingIntents — so button taps NEVER open the app.
 *
 * The app-body-tap still navigates to the dashboard (expected UX).
 */
@Injectable({ providedIn: 'root' })
export class NotificationService {
  private ngZone    = inject(NgZone);
  private router    = inject(Router);
  private authService = inject(AuthService);
  private lockService = inject(LockService);
  private localAuth   = inject(LocalAuthService);

  private trackingSub?: Subscription;

  async init() {
    if (!Capacitor.isNativePlatform()) {
      console.log('[NotificationService] Not on a native platform. Skipping initialization.');
      return;
    }

    // Monitor Auth State & Device Status to show/hide/update notification
    this.trackingSub = combineLatest([
      this.authService.user$,
      this.lockService.getDeviceStatus()
    ]).subscribe({
      next: async ([user, deviceStatus]) => {
        if (!user) {
          console.log('[NotificationService] User logged out. Clearing notification.');
          await this.dismissOngoingNotification();
        } else {
          await this.updateOngoingNotification(deviceStatus);
        }
      },
      error: (err) => console.error('[NotificationService] Telemetry subscription error:', err)
    });
  }

  private async updateOngoingNotification(status: any) {
    let statusText = 'Status: Smart Lock Offline';
    if (status && status.online) {
      statusText = status.isLocal
        ? 'Status: Connected via BLE (Local Mode)'
        : `Status: Connected via Cloud (${status.rssi ? status.rssi + 'dBm' : 'Online'})`;
    }

    try {
      await SmartLockNotification.show({
        title: '🏠 HomeAlone Smart Lock',
        body:  `${statusText}\nQuick actions for door & water pump`
      });
      console.log('[NotificationService] Ongoing notification updated:', statusText);
    } catch (err) {
      console.error('[NotificationService] Scheduling notification failed:', err);
    }
  }

  async dismissOngoingNotification() {
    if (!Capacitor.isNativePlatform()) return;
    try {
      await SmartLockNotification.dismiss();
      console.log('[NotificationService] Ongoing notification cleared.');
    } catch (err) {
      console.error('[NotificationService] Dismissing notification failed:', err);
    }
  }
}
