import { Component, OnInit, OnDestroy, inject, NgZone } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Router } from '@angular/router';
import { App } from '@capacitor/app';
import { PluginListenerHandle } from '@capacitor/core';
import { LocalAuthService } from '../services/local-auth.service';

type PinScreenMode = 'verify' | 'create' | 'confirm';

@Component({
  selector: 'app-pin-lock',
  standalone: true,
  imports: [CommonModule],
  templateUrl: './pin-lock.html',
  styleUrls: ['./pin-lock.css'],
})
export class PinLockComponent implements OnInit, OnDestroy {
  private localAuth = inject(LocalAuthService);
  private router = inject(Router);
  private ngZone = inject(NgZone);
  private resumeListener?: PluginListenerHandle;
  private lastAuthTime = 0; // Timestamp of last biometric attempt

  mode: PinScreenMode = 'verify';
  pin = '';
  firstPin = '';       // stores first entry during create→confirm flow
  errorMessage = '';
  shakeError = false;
  isBiometricAvailable = false;
  biometricType = 'biometric';
  isLoading = false;

  maxPinLength = 4;
  readonly minPinLength = 4;

  get dots(): boolean[] {
    return Array(this.maxPinLength).fill(false).map((_, i) => i < this.pin.length);
  }

  get title(): string {
    if (this.mode === 'create')  return 'Create PIN';
    if (this.mode === 'confirm') return 'Confirm PIN';
    return 'Enter PIN';
  }

  get subtitle(): string {
    if (this.mode === 'create')  return 'Set a 4–6 digit PIN for quick access';
    if (this.mode === 'confirm') return 'Re-enter your PIN to confirm';
    return 'Enter your PIN to continue';
  }

  async ngOnInit() {
    this.isBiometricAvailable = await this.localAuth.isBiometricAvailable();
    this.biometricType = await this.localAuth.getBiometricType();

    const pinSet = await this.localAuth.isPinSet();
    this.mode = pinSet ? 'verify' : 'create';

    if (pinSet) {
      this.maxPinLength = await this.localAuth.getPinLength();
    } else {
      this.maxPinLength = 4; // Default for creation
    }

    // Auto-attempt biometric when verifying (initial load)
    if (pinSet && this.isBiometricAvailable) {
      setTimeout(() => this.tryBiometric(), 800);
    }

    // Also re-trigger biometric when app returns to foreground (if already on screen)
    this.resumeListener = await App.addListener('appStateChange', ({ isActive }) => {
      console.log('PinLockComponent: App state changed, isActive:', isActive);
      const now = Date.now();
      // Only re-trigger if app became active AND we haven't tried recently (> 3s)
      // This prevents the loop caused by the biometric dialog itself resuming the app
      if (isActive && this.mode === 'verify' && this.isBiometricAvailable && (now - this.lastAuthTime > 3000)) {
        console.log('PinLockComponent: Re-triggering biometric (cooldown passed)...');
        setTimeout(() => this.tryBiometric(), 500);
      }
    });
  }

  ngOnDestroy() {
    if (this.resumeListener) {
      this.resumeListener.remove();
    }
  }

  pressKey(key: string) {
    if (this.pin.length >= this.maxPinLength) return;
    this.pin += key;
    this.errorMessage = '';

    // Auto-submit when length reached
    if (this.pin.length === this.maxPinLength) {
      setTimeout(() => this.submitPin(), 120);
    }
  }

  pressBackspace() {
    if (this.pin.length > 0) {
      this.pin = this.pin.slice(0, -1);
    }
  }

  async submitPin() {
    if (this.pin.length < this.minPinLength) {
      this.showError(`PIN must be at least ${this.minPinLength} digits`);
      return;
    }

    this.isLoading = true;

    // ── Create mode: save first entry, switch to confirm ───
    if (this.mode === 'create') {
      this.maxPinLength = this.pin.length; // Lock in the length they chose
      this.firstPin = this.pin;
      this.pin = '';
      this.mode = 'confirm';
      this.isLoading = false;
      return;
    }

    // ── Confirm mode: compare with first entry ──────────────
    if (this.mode === 'confirm') {
      if (this.pin !== this.firstPin) {
        this.showError('PINs do not match. Try again.');
        this.pin = '';
        this.firstPin = '';
        this.mode = 'create';
        this.isLoading = false;
        return;
      }
      await this.localAuth.savePin(this.pin);
      this.isLoading = false;
      this.ngZone.run(() => {
        this.router.navigate(['/dashboard']);
      });
      return;
    }

    // ── Verify mode: check against stored PIN ───────────────
    const ok = await this.localAuth.verifyPin(this.pin);
    this.isLoading = false;
    if (ok) {
      this.localAuth.setJustUnlocked();
      this.ngZone.run(() => {
        this.router.navigate(['/dashboard']);
      });
    } else {
      this.showError('Incorrect PIN. Try again.');
      this.pin = '';
    }
  }

  async tryBiometric() {
    const now = Date.now();
    if (!this.isBiometricAvailable || this.localAuth.getIsAuthenticating() || (now - this.lastAuthTime < 2000)) {
      return;
    }
    
    this.localAuth.setAuthenticating(true);
    this.lastAuthTime = now;
    try {
      const ok = await this.localAuth.authenticateWithBiometric();
      if (ok) {
        console.log('Biometric auth successful, navigating...');
        this.localAuth.setJustUnlocked();
        this.ngZone.run(() => {
          this.router.navigate(['/dashboard']);
        });
      }
    } catch (err) {
      console.error('Biometric error:', err);
    } finally {
      // Small delay before allowing another attempt, to let the UI settle
      setTimeout(() => {
        this.localAuth.setAuthenticating(false);
      }, 1000);
    }
  }

  private showError(msg: string) {
    this.errorMessage = msg;
    this.shakeError = true;
    setTimeout(() => { this.shakeError = false; }, 650);
  }
}
