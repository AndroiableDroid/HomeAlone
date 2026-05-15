import { Component, inject, NgZone, OnDestroy, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { BleClient, BleDevice, textToDataView } from '@capacitor-community/bluetooth-le';

const SERVICE_UUID = '4fafc201-1fb5-459e-8fcc-c5c9c331914b';
const CHARACTERISTIC_UUID_SSID = 'beb5483e-36e1-4688-b7f5-ea07361b26a8';
const CHARACTERISTIC_UUID_PASS = '6b4122d2-8a9d-4952-b8d2-4309320257e5';

type SetupStep = 'scan' | 'connect' | 'credentials' | 'sending' | 'success' | 'error';

@Component({
  selector: 'app-wifi-setup',
  standalone: true,
  imports: [CommonModule, FormsModule],
  templateUrl: './wifi-setup.html',
  styleUrls: ['./wifi-setup.css']
})
export class WifiSetupComponent implements OnInit, OnDestroy {
  private router = inject(Router);
  private ngZone = inject(NgZone);

  currentStep: SetupStep = 'scan';
  ssid = '';
  password = '';
  error = '';

  isScanning = false;
  discoveredDevices: BleDevice[] = [];
  connectedDevice: BleDevice | null = null;

  async ngOnInit() {
    try {
      await BleClient.initialize({ androidNeverForLocation: true });
    } catch (e) {
      console.error('BLE init error', e);
      this.error = 'Bluetooth is not available on this device.';
      this.currentStep = 'error';
    }
  }

  ngOnDestroy() {
    this.stopScan();
    if (this.connectedDevice) {
      BleClient.disconnect(this.connectedDevice.deviceId).catch(() => {});
    }
  }

  async startScan() {
    this.isScanning = true;
    this.discoveredDevices = [];
    this.error = '';

    try {
      // Ensure Bluetooth is enabled
      const isEnabled = await BleClient.isEnabled();
      if (!isEnabled) {
        await BleClient.enable();
      }

      // Scan without service UUID filter — 128-bit UUIDs are in the scan response
      // packet, not the primary advertisement, so the OS filter drops them.
      // We filter by name in the callback instead.
      await BleClient.requestLEScan(
        { allowDuplicates: false },
        (result) => {
          this.ngZone.run(() => {
            const name = result.device.name ?? result.localName ?? '';
            if (!name.startsWith('SmartLock')) return;

            const exists = this.discoveredDevices.find(
              d => d.deviceId === result.device.deviceId
            );
            if (!exists) {
              this.discoveredDevices.push({ ...result.device, name });
            }
          });
        }
      );

      // Stop scan after 12 seconds
      setTimeout(async () => {
        await this.stopScan();
      }, 12000);
    } catch (err: any) {
      console.error('Scan failed', err);
      // Fallback: native OS BLE picker (always works for permissions)
      try {
        const device = await BleClient.requestDevice({
          namePrefix: 'SmartLock'
        });
        if (device) {
          this.ngZone.run(() => {
            this.discoveredDevices = [device];
            this.isScanning = false;
          });
          return;
        }
      } catch (fallbackErr) {
        console.error('Fallback also failed', fallbackErr);
      }

      this.ngZone.run(() => {
        this.error = 'Scanning failed. Please ensure Bluetooth is enabled and permissions are granted.';
        this.isScanning = false;
      });
    }
  }

  async stopScan() {
    try {
      await BleClient.stopLEScan();
    } catch (e) { /* ignore */ }
    this.ngZone.run(() => {
      this.isScanning = false;
    });
  }

  async connectToDevice(device: BleDevice) {
    this.currentStep = 'connect';
    this.error = '';
    try {
      await this.stopScan();
      await BleClient.connect(device.deviceId, (deviceId) => {
        // Disconnect callback
        this.ngZone.run(() => {
          if (this.currentStep === 'credentials') {
            this.error = 'Device disconnected unexpectedly.';
            this.connectedDevice = null;
            this.currentStep = 'scan';
          }
        });
      });
      this.connectedDevice = device;
      this.currentStep = 'credentials';
    } catch (err) {
      console.error('Connect failed', err);
      this.error = 'Failed to connect. Please try again.';
      this.currentStep = 'scan';
    }
  }

  async sendCredentials() {
    if (!this.ssid || !this.password || !this.connectedDevice) return;

    this.currentStep = 'sending';
    this.error = '';

    try {
      // Write SSID first
      await BleClient.write(
        this.connectedDevice.deviceId,
        SERVICE_UUID,
        CHARACTERISTIC_UUID_SSID,
        textToDataView(this.ssid)
      );

      // Small delay to let the ESP32 process
      await new Promise(resolve => setTimeout(resolve, 500));

      // Write Password (triggers save & reboot on the ESP32)
      await BleClient.write(
        this.connectedDevice.deviceId,
        SERVICE_UUID,
        CHARACTERISTIC_UUID_PASS,
        textToDataView(this.password)
      );

      // Disconnect gracefully (may fail since ESP32 reboots)
      try {
        await BleClient.disconnect(this.connectedDevice.deviceId);
      } catch (e) { /* ESP32 already rebooting */ }

      this.currentStep = 'success';

      setTimeout(() => {
        this.router.navigate(['/dashboard']);
      }, 5000);
    } catch (err) {
      console.error('Send failed', err);
      this.error = 'Failed to send credentials. Please try again.';
      this.currentStep = 'credentials';
    }
  }

  goBack() {
    if (this.currentStep === 'credentials' && this.connectedDevice) {
      BleClient.disconnect(this.connectedDevice.deviceId).catch(() => {});
      this.connectedDevice = null;
      this.currentStep = 'scan';
    } else {
      this.router.navigate(['/dashboard']);
    }
  }

  getStepNumber(): number {
    switch (this.currentStep) {
      case 'scan': return 1;
      case 'connect': return 2;
      case 'credentials': return 3;
      case 'sending': return 3;
      case 'success': return 4;
      default: return 1;
    }
  }
}
