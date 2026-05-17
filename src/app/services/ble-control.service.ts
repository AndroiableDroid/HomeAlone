import { Injectable, inject, NgZone } from '@angular/core';
import { BleClient, BleDevice, textToDataView } from '@capacitor-community/bluetooth-le';
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

  async init() {
    if (this.initialized) return;
    try {
      await BleClient.initialize();
      this.initialized = true;
    } catch (e) {
      console.error('BLE Client init failed', e);
    }
  }

  getConnectedDevice() {
    return this.connectedDevice$.asObservable();
  }

  async connect() {
    if (this.connectedDevice$.value || this.isConnecting) return;
    this.isConnecting = true;
    this.isAuthorized = false; // Reset auth on new connection

    // Ensure BLE stack is initialized before any operation
    await this.init();

    try {
      // Use the native OS device picker — this shows a visible dialog
      // where the user can select SmartLock-BLE.
      const device = await BleClient.requestDevice({
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
      await this.connect();
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
      await new Promise(r => setTimeout(r, 300));
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
