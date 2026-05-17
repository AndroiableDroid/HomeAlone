import { Injectable, inject, NgZone } from '@angular/core';
import { BleClient, BleDevice, textToDataView } from '@capacitor-community/bluetooth-le';
import { Preferences } from '@capacitor/preferences';
import { BehaviorSubject } from 'rxjs';
import { LocalAuthService } from './local-auth.service';

const SERVICE_UUID = '4fafc201-1fb5-459e-8fcc-c5c9c331914b';
const CHARACTERISTIC_UUID_CTRL = 'd2b45071-34aa-4161-80a5-e3d6229505f1';

@Injectable({ providedIn: 'root' })
export class BleControlService {
  private ngZone = inject(NgZone);
  private localAuth = inject(LocalAuthService);
  
  private connectedDevice$ = new BehaviorSubject<BleDevice | null>(null);
  private isConnecting = false;
  private isAuthorized = false;
  private initialized = false;
  private reconnectInterval: any;

  constructor() {
    this.localAuth.getConnectionMode$().subscribe(mode => {
      this.ngZone.run(() => {
        if (mode === 'local') {
          console.log('[BLE] Connection mode is local. Starting background auto-connect...');
          this.connect(false).catch(() => {});
          this.startReconnectLoop();
        } else {
          console.log('[BLE] Connection mode is cloud. Stopping background reconnect...');
          this.stopReconnectLoop();
          if (this.connectedDevice$.value) {
            const dev = this.connectedDevice$.value;
            this.connectedDevice$.next(null);
            this.isAuthorized = false;
            BleClient.disconnect(dev.deviceId).catch(() => {});
          }
        }
      });
    });
  }

  private startReconnectLoop() {
    if (this.reconnectInterval) clearInterval(this.reconnectInterval);
    this.reconnectInterval = setInterval(() => {
      if (this.localAuth.getConnectionMode() === 'local' && !this.connectedDevice$.value && !this.isConnecting) {
        console.log('[BLE] Background reconnect loop checking...');
        this.connect(false).catch(() => {});
      }
    }, 6000); // Check every 6 seconds
  }

  private stopReconnectLoop() {
    if (this.reconnectInterval) {
      clearInterval(this.reconnectInterval);
      this.reconnectInterval = null;
    }
  }

  async init() {
    if (this.initialized) return;
    try {
      await BleClient.initialize({ androidNeverForLocation: true });
      this.initialized = true;
    } catch (e) {
      console.error('BLE Client init failed', e);
    }
  }

  getConnectedDevice() {
    return this.connectedDevice$.asObservable();
  }

  async connect(forcePicker = false) {
    if (this.connectedDevice$.value || this.isConnecting) return;
    this.isConnecting = true;
    this.isAuthorized = false; // Reset auth on new connection

    // Ensure BLE stack is initialized before any operation
    await this.init();

    try {
      let device: BleDevice | null = null;

      // Try auto-connect if we have a saved device ID and aren't forcing the picker
      const { value: savedDeviceId } = await Preferences.get({ key: 'last_ble_device_id' });

      if (savedDeviceId && !forcePicker) {
        console.log('Attempting silent auto-connect to:', savedDeviceId);
        try {
          // Register device ID with system cache
          try {
            await BleClient.getDevices([savedDeviceId]);
          } catch (e) {}

          await BleClient.connect(savedDeviceId, (id) => {
            console.log('BLE Disconnected', id);
            this.ngZone.run(() => {
              this.connectedDevice$.next(null);
              this.isAuthorized = false;
            });
          });

          device = {
            deviceId: savedDeviceId,
            name: 'SmartLock-BLE'
          };
          console.log('Silent auto-connect successful!');
        } catch (autoErr) {
          console.warn('Silent auto-connect failed, trying background scan fallback...', autoErr);
        }
      }

      // Fallback: If we don't have a device yet and aren't forcing the picker, do a quick silent scan in the background
      if (!device && !forcePicker) {
        console.log('Starting silent background scan fallback...');
        try {
          let foundDevice: BleDevice | null = null;
          
          await BleClient.requestLEScan(
            { allowDuplicates: false },
            (result) => {
              const name = result.device.name ?? result.localName ?? '';
              if (name.startsWith('SmartLock')) {
                foundDevice = result.device;
                BleClient.stopLEScan().catch(() => {});
              }
            }
          );

          // Wait up to 4 seconds for scan to find the device
          for (let i = 0; i < 8; i++) {
            if (foundDevice) break;
            await new Promise(resolve => setTimeout(resolve, 500));
          }

          await BleClient.stopLEScan().catch(() => {});

          if (foundDevice) {
            const dev = foundDevice as BleDevice;
            console.log('Silent scan found device, connecting to:', dev.deviceId);
            await BleClient.connect(dev.deviceId, (id) => {
              console.log('BLE Disconnected', id);
              this.ngZone.run(() => {
                this.connectedDevice$.next(null);
                this.isAuthorized = false;
              });
            });

            device = dev;
            await Preferences.set({ key: 'last_ble_device_id', value: dev.deviceId });
            console.log('Background scan connection successful!');
          } else {
            console.log('Silent background scan did not find any SmartLock device.');
          }
        } catch (scanErr) {
          console.error('Silent background scan failed:', scanErr);
        }
      }

      // If we still don't have a device (e.g. no saved ID, or forcePicker is true)
      if (!device) {
        // Only show picker if explicitly forced
        if (!forcePicker) {
          console.log('No saved device found and silent scan empty. Waiting for user tap.');
          this.isConnecting = false;
          return;
        }

        console.log('Opening native device picker dialog...');
        device = await BleClient.requestDevice({
          namePrefix: 'SmartLock',
          optionalServices: [SERVICE_UUID]
        });

        if (!device) throw new Error('No device selected');

        // Connect to the selected device
        await BleClient.connect(device.deviceId, (id) => {
          console.log('BLE Disconnected', id);
          this.ngZone.run(() => {
            this.connectedDevice$.next(null);
            this.isAuthorized = false;
          });
        });

        // Save last connected device ID
        await Preferences.set({ key: 'last_ble_device_id', value: device.deviceId });
      }

      this.ngZone.run(() => this.connectedDevice$.next(device));
      console.log('BLE Connected to', device.name);

    } catch (err) {
      console.error('BLE connect failed', err);
    } finally {
      this.isConnecting = false;
      await BleClient.stopLEScan().catch(() => {});
    }
  }

  async sendCommand(cmd: string) {
    if (!this.connectedDevice$.value) {
      await this.connect(true);
    }

    const connected = this.connectedDevice$.value;
    if (!connected) {
      throw new Error('Bluetooth device not connected');
    }

    // Layer 2: App-level token auth
    if (!this.isAuthorized) {
      const token = this.localAuth.getBleAuthToken();
      console.log('Sending BLE auth token...');
      await this.writeRaw(token);
      this.isAuthorized = true;
      // Small delay for ESP32 to process auth before command
      await new Promise(resolve => setTimeout(resolve, 300));
    }

    await this.writeRaw(cmd);
  }

  private async writeRaw(data: string) {
    const connected = this.connectedDevice$.value;
    if (!connected) return;
    
    await BleClient.write(
      connected.deviceId,
      SERVICE_UUID,
      CHARACTERISTIC_UUID_CTRL,
      textToDataView(data),
      { timeout: 30000 }
    );
  }

  async disconnect() {
    if (this.connectedDevice$.value) {
      await BleClient.disconnect(this.connectedDevice$.value.deviceId);
      this.connectedDevice$.next(null);
    }
  }
}
