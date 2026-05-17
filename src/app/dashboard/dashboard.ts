import { Component, inject, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Router, ActivatedRoute } from '@angular/router';
import { AuthService } from '../services/auth-service';
import { LockService, DeviceStatus } from '../services/lock-service';
import { LocalAuthService } from '../services/local-auth.service';
import { BleControlService } from '../services/ble-control.service';
import { Subscription } from 'rxjs';

@Component({
  selector: 'app-dashboard',
  standalone: true,
  imports: [CommonModule],
  templateUrl: './dashboard.html',
  styleUrls: ['./dashboard.css']
})
export class DashboardComponent implements OnInit, OnDestroy {
  private authService = inject(AuthService);
  private lockService = inject(LockService);
  private localAuth = inject(LocalAuthService);
  private bleControl = inject(BleControlService);
  private router = inject(Router);
  private route = inject(ActivatedRoute);
  
  private authSubscription?: Subscription;
  private routeSub?: Subscription;
  private pumpSub?: Subscription;
  private statusSub?: Subscription;

  user$ = this.authService.user$;
  deviceStatus$ = this.lockService.getDeviceStatus();
  pump$ = this.lockService.getPumpStatus();
  isUnlocking = false;
  isResettingWifi = false;
  isPumpToggling = false;
  pumpError = '';
  resetMessage = '';
  isPumpOn = false;
  isMenuOpen = false; // Sidebar state
  connectionMode: 'cloud' | 'local' = 'cloud';

  // Premium Widget States
  slideProgress = 0;
  pumpActiveSeconds = 0;
  pumpTimerInterval: any = null;
  activityLogs: Array<{ text: string; time: Date; type: 'info' | 'success' | 'warning' }> = [];

  ngOnInit() {
    this.connectionMode = this.localAuth.getConnectionMode();
    this.addLog('Dashboard console initialized', 'info');
    
    this.authSubscription = this.user$.subscribe(user => {
      if (!user) {
        this.router.navigate(['/login']);
      }
    });

    if (this.connectionMode === 'local') {
      this.addLog('BLE connection mode active - attempting auto-connect', 'info');
      setTimeout(() => this.connectBle(false), 500);
    }

    // Subscribe to widget deep link triggers
    this.routeSub = this.route.queryParams.subscribe(params => {
      const trigger = params['trigger'];
      if (trigger === 'unlock') {
        this.addLog('Direct unlock request received from Widget shortcut', 'info');
        setTimeout(() => this.unlockDoor(), 600);
      } else if (trigger === 'pump') {
        this.addLog('Direct pump toggle request received from Widget shortcut', 'info');
        setTimeout(() => {
          const sub = this.pump$.subscribe(status => {
            this.togglePump(status);
            sub.unsubscribe();
          });
        }, 600);
      }
    });

    // Track pump state to manage live wave-timer
    this.pumpSub = this.pump$.subscribe(status => {
      const isActive = status === 'RUNNING' || status === 'ON';
      
      // Manage timer increment
      if (isActive) {
        if (!this.pumpTimerInterval) {
          this.pumpActiveSeconds = 0;
          this.pumpTimerInterval = setInterval(() => {
            this.pumpActiveSeconds++;
          }, 1000);
          this.addLog('Water Pump started running', 'success');
        }
      } else {
        if (this.pumpTimerInterval) {
          clearInterval(this.pumpTimerInterval);
          this.pumpTimerInterval = null;
          this.addLog('Water Pump stopped', 'info');
        }
        this.pumpActiveSeconds = 0;
      }
    });

    // Monitor connection and signal strength
    this.statusSub = this.deviceStatus$.subscribe(status => {
      if (status.online) {
        this.addLog(`Smart Lock status: Connected (${status.isLocal ? 'BLE' : 'Cloud'})`, 'success');
      } else {
        this.addLog('Smart Lock status: Device went offline', 'warning');
      }
    });
  }

  ngOnDestroy() {
    if (this.authSubscription) this.authSubscription.unsubscribe();
    if (this.routeSub) this.routeSub.unsubscribe();
    if (this.pumpSub) this.pumpSub.unsubscribe();
    if (this.statusSub) this.statusSub.unsubscribe();
    if (this.pumpTimerInterval) clearInterval(this.pumpTimerInterval);
  }

  // ── Logging System ────────────────────────────────────────────────────────
  addLog(text: string, type: 'info' | 'success' | 'warning' = 'info') {
    this.activityLogs.unshift({ text, time: new Date(), type });
    if (this.activityLogs.length > 25) {
      this.activityLogs.pop();
    }
  }

  formatPumpTime(): string {
    const m = Math.floor(this.pumpActiveSeconds / 60);
    const s = this.pumpActiveSeconds % 60;
    return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
  }

  // ── Slide to Unlock handlers ──────────────────────────────────────────────
  onSlideChange(event: any) {
    const val = parseInt(event.target.value, 10);
    this.slideProgress = val;
    if (val >= 98) {
      this.slideProgress = 100;
      this.unlockDoor();
      setTimeout(() => {
        this.slideProgress = 0;
      }, 3000);
    }
  }

  onSlideRelease() {
    if (this.slideProgress < 98) {
      this.slideProgress = 0;
    }
  }

  async connectBle(force = false) {
    try {
      this.addLog('Connecting to BLE Device...', 'info');
      await this.bleControl.connect(force);
      this.addLog('BLE connection succeeded', 'success');
    } catch (e: any) {
      this.addLog('BLE connection failed: ' + (e?.message ?? e), 'warning');
      console.error('BLE connection failed', e);
    }
  }

  async unlockDoor() {
    this.isUnlocking = true;
    this.addLog('Sending Front Door UNLOCK command...', 'info');
    try {
      await this.lockService.triggerUnlock();
      this.addLog('Door unlocked successfully (3s active period)', 'success');
      setTimeout(() => { this.isUnlocking = false; }, 3000);
    } catch (err: any) {
      this.addLog('Failed to unlock Front Door: ' + (err?.message ?? 'Network error'), 'warning');
      console.error('Failed to unlock', err);
      this.isUnlocking = false;
    }
  }

  async togglePump(currentStatus: string) {
    const isRunning = currentStatus === 'ON' || currentStatus === 'RUNNING';
    this.isPumpToggling = true;
    this.pumpError = '';
    const newState = isRunning ? 'OFF' : 'ON';
    this.addLog(`Sending PUMP_${newState} command...`, 'info');
    try {
      await this.lockService.setPump(newState);
      this.addLog(`Pump state updated to ${newState}`, 'success');
    } catch (err: any) {
      console.error('Pump toggle failed', err);
      this.pumpError = 'Failed: ' + (err?.message ?? 'Check Firebase rules.');
      this.addLog('Pump toggle failed: ' + this.pumpError, 'warning');
    } finally {
      this.isPumpToggling = false;
    }
  }

  async changeWifi() {
    const confirmed = confirm(
      'This will reboot the Smart Lock into Bluetooth setup mode.\n\nYou will need to reconnect via the BLE provisioning flow.\n\nContinue?'
    );
    if (!confirmed) return;

    this.isResettingWifi = true;
    this.resetMessage = 'Sending reset command...';
    this.addLog('Initiating WiFi reset on ESP32...', 'warning');
    try {
      await this.lockService.resetWifi();
      this.resetMessage = '✓ Command sent! Smart Lock is rebooting into BLE mode. Open WiFi Setup to reconfigure.';
      this.addLog('WiFi reset command successfully dispatched', 'success');
      setTimeout(() => {
        this.resetMessage = '';
        this.isResettingWifi = false;
        this.router.navigate(['/wifi-setup']);
      }, 4000);
    } catch (err) {
      console.error('Reset failed', err);
      this.resetMessage = 'Failed to send command. Is the device online?';
      this.isResettingWifi = false;
    }
  }

  goToWifiSetup() {
    this.router.navigate(['/wifi-setup']);
  }

  logout() {
    this.authService.logout();
  }

  formatUptime(seconds: number): string {
    if (!seconds) return '—';
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = seconds % 60;
    if (h > 0) return `${h}h ${m}m`;
    if (m > 0) return `${m}m ${s}s`;
    return `${s}s`;
  }

  formatLastSeen(ts: number, offset: number = 0): string {
    if (!ts) return 'Never';
    const serverNow = Date.now() + offset;
    const diff = Math.floor((serverNow - ts) / 1000);
    if (diff < 0) return 'Just now'; // If client clock is behind server
    if (diff < 60) return `${diff}s ago`;
    if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
    return `${Math.floor(diff / 3600)}h ago`;
  }

  getRssiStrength(rssi: number): string {
    if (!rssi) return '—';
    if (rssi >= -50) return 'Excellent';
    if (rssi >= -65) return 'Good';
    if (rssi >= -75) return 'Fair';
    return 'Weak';
  }

  toggleMenu() {
    this.isMenuOpen = !this.isMenuOpen;
  }

  async changePin() {
    const confirmed = confirm('Do you want to change your PIN? This will clear your current PIN and take you to the setup screen.');
    if (confirmed) {
      await this.localAuth.clearPin();
      this.router.navigate(['/pin-lock']);
    }
  }

  async switchConnectionMode(mode: 'cloud' | 'local') {
    this.connectionMode = mode;
    await this.lockService.setConnectionMode(mode);
    this.isMenuOpen = false;
  }

  updateBleToken(event: any) {
    const token = event.target.value;
    if (token) this.localAuth.setBleAuthToken(token);
  }

  updateBlePin(event: any) {
    const pin = event.target.value;
    if (pin) this.localAuth.setBlePin(pin);
  }

  get bleToken() { return this.localAuth.getBleAuthToken(); }
  get blePin() { return this.localAuth.getBlePin(); }
}



