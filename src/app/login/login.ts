import { Component, inject, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { AuthService } from '../services/auth-service';
import { Subscription } from 'rxjs';

@Component({
  selector: 'app-login',
  standalone: true,
  imports: [CommonModule, FormsModule],
  templateUrl: './login.html',
  styleUrls: ['./login.css']
})
export class LoginComponent implements OnInit, OnDestroy {
  private authService = inject(AuthService);
  private router = inject(Router);
  private authSubscription?: Subscription;
  
  email = '';
  password = '';
  loading = false;
  checkingAuth = true;
  error = '';

  ngOnInit() {
    // Check if user is already logged in
    this.authSubscription = this.authService.user$.subscribe(user => {
      if (user) {
        this.router.navigate(['/dashboard']);
      } else {
        this.checkingAuth = false;
      }
    });
  }


  ngOnDestroy() {
    if (this.authSubscription) {
      this.authSubscription.unsubscribe();
    }
  }

  async handleLogin(event: Event) {
    event.preventDefault();
    this.loading = true;
    this.error = '';
    
    try {
      await this.authService.login(this.email, this.password);
    } catch (err: any) {
      this.error = 'Invalid credentials. Please try again.';
      console.error(err);
    } finally {
      this.loading = false;
    }
  }
}

