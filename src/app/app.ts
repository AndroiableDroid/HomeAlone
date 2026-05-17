import { Component, inject, OnInit, NgZone } from '@angular/core';
import { RouterOutlet, Router } from '@angular/router';
import { App } from '@capacitor/app';
import { AuthService } from './services/auth-service';
import { LocalAuthService } from './services/local-auth.service';
import { BleControlService } from './services/ble-control.service';

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [RouterOutlet],
  template: `
    <router-outlet></router-outlet>
  `
})
export class AppComponent implements OnInit {
  private router = inject(Router);
  private authService = inject(AuthService);
  private localAuth = inject(LocalAuthService);
  private bleControl = inject(BleControlService);
  private ngZone = inject(NgZone);

  async ngOnInit() {
    console.log('AppComponent initialized');
    await this.localAuth.init();
    await this.bleControl.init();
    // Listen for app state changes (background/foreground)
    App.addListener('appStateChange', async ({ isActive }) => {
      console.log('App state changed, isActive:', isActive);
      if (isActive) {
        // Use a timeout to ensure Firebase has time to re-establish its state if needed
        setTimeout(async () => {
          const user = this.authService.getCurrentUser();
          const pinSet = await this.localAuth.isPinSet();
          const currentUrl = this.router.url;
          const justUnlocked = this.localAuth.isJustUnlocked();
          
          console.log('Resume check - User:', !!user, 'PIN set:', pinSet, 'URL:', currentUrl, 'Just Unlocked:', justUnlocked);

          // Only redirect if logged in, PIN is set, NOT just unlocked, and not already on a sensitive screen
          if (user && pinSet && !justUnlocked &&
              !currentUrl.includes('pin-lock') && 
              !currentUrl.includes('login') &&
              !currentUrl.includes('wifi-setup')) {
            console.log('Redirecting to PIN lock...');
            this.ngZone.run(() => {
              this.router.navigate(['/pin-lock']);
            });
          }
        }, 300);

      }
    });
  }
}
