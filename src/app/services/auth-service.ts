import { Injectable, inject } from '@angular/core';
import { Auth, signInWithEmailAndPassword, signOut, user, User, setPersistence, indexedDBLocalPersistence } from '@angular/fire/auth';
import { Router } from '@angular/router';
import { Observable, from } from 'rxjs';

@Injectable({ providedIn: 'root' })
export class AuthService {
  private auth: Auth = inject(Auth);
  private router: Router = inject(Router);
  
  user$: Observable<User | null> = user(this.auth);

  constructor() {
    // Ensure persistence is set for mobile environments
    from(setPersistence(this.auth, indexedDBLocalPersistence)).subscribe({
      error: (err) => console.error('Persistence error', err)
    });
  }

  async login(email: string, pass: string) {
    try {
      await signInWithEmailAndPassword(this.auth, email, pass);
      this.router.navigate(['/dashboard']);
    } catch (error) {
      console.error('Login failed', error);
      throw error;
    }
  }

  async logout() {
    await signOut(this.auth);
    this.router.navigate(['/login']);
  }
}

