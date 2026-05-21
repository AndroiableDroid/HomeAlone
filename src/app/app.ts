import { Component, inject, OnInit, NgZone } from '@angular/core';
import { RouterOutlet, Router } from '@angular/router';
import { App } from '@capacitor/app';
import { AuthService } from './services/auth-service';
import { LocalAuthService } from './services/local-auth.service';
import { BleControlService } from './services/ble-control.service';
import { NotificationService } from './services/notification.service';

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
  private notificationService = inject(NotificationService);
  private ngZone = inject(NgZone);

  async ngOnInit() {
    console.log('AppComponent initialized');
    await this.localAuth.init();
    await this.bleControl.init();
    await this.notificationService.init();

    // Listen for widget deep-link shortcuts
    App.addListener('appUrlOpen', (event: any) => {
      this.ngZone.run(() => {
        console.log('App opened with URL:', event.url);
        const url = event.url || '';
        if (url.includes('smartlock://unlock') || url.includes('smartlock://pump')) {
          this.localAuth.setJustUnlocked();
          const trigger = url.includes('smartlock://unlock') ? 'unlock' : 'pump';
          
          // Defer navigation to allow Angular to fully bootstrap, preventing TDZ ReferenceErrors
          setTimeout(() => {
            this.ngZone.run(() => {
              console.log(`[DeepLink] Safe-routing to dashboard with trigger: ${trigger}`);
              this.router.navigate(['/dashboard'], { queryParams: { trigger, t: Date.now() } });
            });
          }, 300);
        }
      });
    });

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
