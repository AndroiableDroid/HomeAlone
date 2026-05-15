import { Routes } from '@angular/router';
import { LoginComponent } from './login/login';
import { DashboardComponent } from './dashboard/dashboard';
import { WifiSetupComponent } from './wifi-setup/wifi-setup';
import { PinLockComponent } from './pin-lock/pin-lock';

export const routes: Routes = [
  { path: '', redirectTo: 'login', pathMatch: 'full' },
  { path: 'login', component: LoginComponent },
  { path: 'pin-lock', component: PinLockComponent },
  { path: 'dashboard', component: DashboardComponent },
  { path: 'wifi-setup', component: WifiSetupComponent },
];
