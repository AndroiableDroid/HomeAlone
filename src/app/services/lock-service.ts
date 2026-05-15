import { Injectable, inject } from '@angular/core';
import { Database, ref, set, object } from '@angular/fire/database';
import { Observable, map, combineLatest, timer } from 'rxjs';

export interface DeviceStatus {
  online: boolean;
  ip?: string;
  rssi?: number;
  uptime?: number;
  lastSeen?: number;
  serverTimeOffset?: number;
}

@Injectable({ providedIn: 'root' })
export class LockService {
  private db: Database = inject(Database);

  triggerUnlock() {
    const doorRef = ref(this.db, 'door/status');
    return set(doorRef, 'UNLOCKED');
  }

  resetWifi() {
    const cmdRef = ref(this.db, 'device/command');
    return set(cmdRef, 'RESET_WIFI');
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

    // Combine with a timer to force re-evaluation of 'isStale' every 10 seconds
    return combineLatest([statusData$, offset$, timer(0, 10000)]).pipe(
      map(([val, offset]) => {
        if (!val) return { online: false };

        const serverNow = Date.now() + offset;
        const lastSeen = val.lastSeen ?? 0;
        // Consider offline if lastSeen is > 50 seconds old (heartbeat is 30s)
        const isStale = (serverNow - lastSeen) > 50000;

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