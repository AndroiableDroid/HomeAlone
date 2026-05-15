import { Component, OnInit, inject } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Router } from '@angular/router';
import { LocalAuthService } from '../services/local-auth.service';

type PinScreenMode = 'verify' | 'create' | 'confirm';

@Component({
  selector: 'app-pin-lock',
  standalone: true,
  imports: [CommonModule],
  templateUrl: './pin-lock.html',
  styleUrls: ['./pin-lock.css'],
})
export class PinLockComponent implements OnInit {
  private localAuth = inject(LocalAuthService);
  private router = inject(Router);

  mode: PinScreenMode = 'verify';
  pin = '';
  firstPin = '';       // stores first entry during create→confirm flow
  errorMessage = '';
  shakeError = false;
  isBiometricAvailable = false;
  biometricType = 'biometric';
  isLoading = false;

  readonly maxPinLength = 6;
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

    // Auto-attempt biometric when verifying
    if (pinSet && this.isBiometricAvailable) {
      setTimeout(() => this.tryBiometric(), 400);
    }
  }

  pressKey(key: string) {
    if (this.pin.length >= this.maxPinLength) return;
    this.pin += key;
    this.errorMessage = '';

    // Auto-submit when max length reached
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
      this.router.navigate(['/dashboard']);
      return;
    }

    // ── Verify mode: check against stored PIN ───────────────
    const ok = await this.localAuth.verifyPin(this.pin);
    this.isLoading = false;
    if (ok) {
      this.router.navigate(['/dashboard']);
    } else {
      this.showError('Incorrect PIN. Try again.');
      this.pin = '';
    }
  }

  async tryBiometric() {
    if (!this.isBiometricAvailable) return;
    const ok = await this.localAuth.authenticateWithBiometric();
    if (ok) {
      this.router.navigate(['/dashboard']);
    }
  }

  private showError(msg: string) {
    this.errorMessage = msg;
    this.shakeError = true;
    setTimeout(() => { this.shakeError = false; }, 650);
  }
}
