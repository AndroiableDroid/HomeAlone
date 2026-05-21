import { registerPlugin } from '@capacitor/core';

export interface SmartLockNotificationPlugin {
  /**
   * Show (or update) the persistent smart-lock notification with native
   * broadcast-based action buttons that do NOT launch the app.
   */
  show(options: { title: string; body: string }): Promise<void>;

  /**
   * Dismiss / cancel the ongoing notification.
   */
  dismiss(): Promise<void>;
}

/**
 * SmartLockNotification
 *
 * Thin Angular-side wrapper that calls the native SmartLockNotificationPlugin.
 * On non-native platforms (web / browser) the calls are no-ops because
 * the plugin implementation is undefined.
 */
export const SmartLockNotification = registerPlugin<SmartLockNotificationPlugin>(
  'SmartLockNotification'
);
