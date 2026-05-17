import { Injectable, inject } from '@angular/core';
import { Database, ref, set, object } from '@angular/fire/database';
import { Observable, map, combineLatest, timer, startWith, of, BehaviorSubject } from 'rxjs';
import { LocalAuthService } from './local-auth.service';
import { BleControlService } from './ble-control.service';

export interface DeviceStatus {
  online: boolean;
  isLocal?: boolean;
  ip?: string;
  rssi?: number;
  uptime?: number;
  lastSeen?: number;
  serverTimeOffset?: number;
}

@Injectable({ providedIn: 'root' })
export class LockService {
  private db: Database = inject(Database);
  private localAuth = inject(LocalAuthService);
  private bleControl = inject(BleControlService);

  private localPumpStatus$ = new BehaviorSubject<string>('STOPPED');

  async triggerUnlock() {
    if (this.localAuth.getConnectionMode() === 'local') {
      return this.bleControl.sendCommand('UNLOCK');
    }
    const doorRef = ref(this.db, 'door/status');
    return set(doorRef, 'UNLOCKED');
  }

  async setPump(state: 'ON' | 'OFF') {
    if (this.localAuth.getConnectionMode() === 'local') {
      await this.bleControl.sendCommand(state === 'ON' ? 'PUMP_ON' : 'PUMP_OFF');
      this.localPumpStatus$.next(state === 'ON' ? 'RUNNING' : 'STOPPED');
      return;
    }
    return set(ref(this.db, 'pump/status'), state);
  }

  async resetWifi() {
    const cmdRef = ref(this.db, 'device/command');
    return set(cmdRef, 'RESET_WIFI');
  }

  async setConnectionMode(mode: 'cloud' | 'local') {
    // Tell ESP32 to switch mode if connected via BLE
    if (this.localAuth.getConnectionMode() === 'local' || mode === 'local') {
      try {
        await this.bleControl.sendCommand(mode === 'local' ? 'MODE_BLE' : 'MODE_WIFI');
      } catch (e) {
        console.warn('Could not send mode change command to ESP32 via BLE', e);
      }
    }
    await this.localAuth.setConnectionMode(mode);
  }

  getPumpStatus(): Observable<string> {
    const firebasePump$ = object(ref(this.db, 'pump/status')).pipe(
      map(snap => (snap.snapshot.val() as string) || 'STOPPED'),
      startWith('STOPPED')
    );

    return combineLatest([
      firebasePump$,
      this.localPumpStatus$,
      this.localAuth.getConnectionMode$()
    ]).pipe(
      map(([firebaseVal, localVal, mode]) => {
        return mode === 'local' ? localVal : firebaseVal;
      })
    );
  }

  getDeviceStatus(): Observable<DeviceStatus> {
    // Get the time offset between client and server
    const offsetRef = ref(this.db, '.info/serverTimeOffset');
    const offset$ = object(offsetRef).pipe(
      map(snap => (snap.snapshot.val() as number) || 0)
    );

    const statusRef = ref(this.db, 'device/status');
    const statusData$ = object(statusRef).pipe(
      map(snapshot => snapshot.snapshot.val() as any)
    );

    // Combine with a timer to force re-evaluation of 'isStale' every 5 seconds
    return combineLatest([
      statusData$,
      offset$,
      timer(0, 5000),
      this.bleControl.getConnectedDevice(),
      this.localAuth.getConnectionMode$()
    ]).pipe(
      map(([val, offset, _, bleDevice, mode]) => {
        if (mode === 'local') {
          return {
            online: !!bleDevice,
            isLocal: true,
            rssi: -50, // Static for local
            uptime: 0
          };
        }

        if (!val) return { online: false };

        const serverNow = Date.now() + offset;
        const lastSeen = val.lastSeen ?? 0;
        // Offline if lastSeen is > 25s old (heartbeat is every 15s, so 1 missed beat)
        const isStale = (serverNow - lastSeen) > 25000;

        return {
          online: val.online === true && !isStale,
          ip: val.ip,
          rssi: val.rssi,
          uptime: val.uptime,
          lastSeen: lastSeen,
          serverTimeOffset: offset
        };
      })
    );
  }
}