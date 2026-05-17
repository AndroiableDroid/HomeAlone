import { Component, inject, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Router } from '@angular/router';
import { AuthService } from '../services/auth-service';
import { LockService, DeviceStatus } from '../services/lock-service';
import { LocalAuthService } from '../services/local-auth.service';
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
  private router = inject(Router);
  private authSubscription?: Subscription;

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

  ngOnInit() {
    this.connectionMode = this.localAuth.getConnectionMode();
    
    this.authSubscription = this.user$.subscribe(user => {
      if (!user) {
        this.router.navigate(['/login']);
      }
    });

    if (this.connectionMode === 'local') {
      // Small delay to ensure BLE is ready
      setTimeout(() => this.lockService.triggerUnlock().catch(() => {}), 1000);
    }
  }

  ngOnDestroy() {
    if (this.authSubscription) {
      this.authSubscription.unsubscribe();
    }
  }

  async unlockDoor() {
    this.isUnlocking = true;
    try {
      await this.lockService.triggerUnlock();
      setTimeout(() => { this.isUnlocking = false; }, 3000);
    } catch (err) {
      console.error('Failed to unlock', err);
      this.isUnlocking = false;
    }
  }

  async togglePump(currentStatus: string) {
    const isRunning = currentStatus === 'ON' || currentStatus === 'RUNNING';
    this.isPumpToggling = true;
    this.pumpError = '';
    try {
      await this.lockService.setPump(isRunning ? 'OFF' : 'ON');
    } catch (err: any) {
      console.error('Pump toggle failed', err);
      this.pumpError = 'Failed: ' + (err?.message ?? 'Check Firebase rules.');
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
    try {
      await this.lockService.resetWifi();
      this.resetMessage = '✓ Command sent! Smart Lock is rebooting into BLE mode. Open WiFi Setup to reconfigure.';
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



