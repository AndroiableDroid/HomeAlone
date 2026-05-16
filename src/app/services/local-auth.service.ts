import { Injectable } from '@angular/core';
import { Preferences } from '@capacitor/preferences';
import {
  BiometricAuth,
  BiometryType,
} from '@aparajita/capacitor-biometric-auth';

const PIN_KEY = 'app_pin_hash';

@Injectable({ providedIn: 'root' })
export class LocalAuthService {
  private justUnlocked = false;
  private isAuthenticating = false;
  private unlockTimeout: any;

  setJustUnlocked() {
    this.justUnlocked = true;
    if (this.unlockTimeout) clearTimeout(this.unlockTimeout);
    this.unlockTimeout = setTimeout(() => {
      this.justUnlocked = false;
    }, 5000); // 5 second grace period
  }

  isJustUnlocked(): boolean {
    return this.justUnlocked;
  }

  setAuthenticating(val: boolean) {
    this.isAuthenticating = val;
  }

  getIsAuthenticating(): boolean {
    return this.isAuthenticating;
  }

  // ── PIN Storage ─────────────────────────────────────────────────────────────

  private async hashPin(pin: string): Promise<string> {
    const encoder = new TextEncoder();
    const data = encoder.encode(pin + 'smart-lock-salt');
    const hashBuffer = await crypto.subtle.digest('SHA-256', data);
    const hashArray = Array.from(new Uint8Array(hashBuffer));
    return hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
  }

  async isPinSet(): Promise<boolean> {
    const { value } = await Preferences.get({ key: PIN_KEY });
    return value !== null;
  }

  async savePin(pin: string): Promise<void> {
    const hash = await this.hashPin(pin);
    await Preferences.set({ key: PIN_KEY, value: hash });
    await Preferences.set({ key: 'app_pin_length', value: pin.length.toString() });
  }

  async getPinLength(): Promise<number> {
    const { value } = await Preferences.get({ key: 'app_pin_length' });
    return value ? parseInt(value, 10) : 4; // Default to 4
  }

  async verifyPin(pin: string): Promise<boolean> {
    const { value } = await Preferences.get({ key: PIN_KEY });
    if (!value) return false;
    const hash = await this.hashPin(pin);
    return hash === value;
  }

  async clearPin(): Promise<void> {
    await Preferences.remove({ key: PIN_KEY });
  }

  // ── Biometric Auth ───────────────────────────────────────────────────────────

  async isBiometricAvailable(): Promise<boolean> {
    try {
      const info = await BiometricAuth.checkBiometry();
      return info.isAvailable;
    } catch {
      return false;
    }
  }

  async getBiometricType(): Promise<string> {
    try {
      const info = await BiometricAuth.checkBiometry();
      switch (info.biometryType) {
        case BiometryType.fingerprintAuthentication: return 'fingerprint';
        case BiometryType.faceAuthentication:        return 'face';
        case BiometryType.irisAuthentication:        return 'iris';
        default: return 'biometric';
      }
    } catch {
      return 'biometric';
    }
  }

  async authenticateWithBiometric(): Promise<boolean> {
    try {
      await BiometricAuth.authenticate({
        reason: 'Verify your identity to access Smart Lock',
        cancelTitle: 'Use PIN instead',
        androidTitle: 'Smart Lock',
        androidSubtitle: 'Use your biometric to unlock',
        allowDeviceCredential: false,
      });
      return true;
    } catch {
      return false;
    }
  }
}
