# SmartLock

A self-hosted smart lock and water pump controller built with an **ESP32**, an **Angular + Capacitor** Android app, and **Firebase Realtime Database**. Supports both cloud-based remote control and local Bluetooth (BLE) control when offline.

## Features

- 🔒 **Remote unlock** via Firebase from anywhere
- 💧 **Water pump control** (remote + local)
- 📶 **Dual-mode operation** — cloud (Firebase) or local (BLE), with automatic fallback
- 🔵 **BLE provisioning** — configure WiFi credentials over Bluetooth without touching the device
- 🔐 **Multi-layer BLE security** — NimBLE passkey pairing + app-level auth token
- 🤳 **Biometric + PIN app lock** — fingerprint/face unlock before accessing controls
- 📲 **Home screen widget + notification actions** — one-tap lock/pump without opening the app
- 🔄 **OTA firmware updates** via GitHub Releases (triggered from Firebase)
- 📊 **Remote logging** via Better Stack
- 🕙 **Scheduled daily restart** and inactivity watchdog on the device

## Architecture

```
┌─────────────────────┐        Firebase RTDB        ┌──────────────────────┐
│   Android App       │ ◄──────────────────────────► │   ESP32 Firmware     │
│ (Angular/Capacitor) │                              │  (sketch_may13a.ino) │
│                     │        BLE (local mode)      │                      │
│                     │ ◄──────────────────────────► │                      │
└─────────────────────┘                              └──────────────────────┘
```

## Repository Structure

```
├── src/                        # Angular app source
│   ├── app/
│   │   ├── dashboard/          # Main lock/pump controls UI
│   │   ├── login/              # Firebase email/password login
│   │   ├── pin-lock/           # PIN + biometric gate (post-login)
│   │   ├── wifi-setup/         # BLE WiFi provisioning screen
│   │   ├── plugins/            # Capacitor native plugin wrappers
│   │   └── services/
│   │       ├── auth-service.ts         # Firebase Authentication
│   │       ├── ble-control.service.ts  # BLE connection + commands
│   │       ├── local-auth.service.ts   # PIN, biometric, BLE token mgmt
│   │       ├── lock-service.ts         # Firebase RTDB lock/pump state
│   │       └── notification.service.ts # Push notifications
│   └── environments/
│       └── environment.example.ts      # ← copy to environment.ts and fill in
├── android/                    # Capacitor Android project
├── sketch_may13a/
│   ├── sketch_may13a.ino       # ESP32 firmware
│   └── secrets.example.h       # ← copy to secrets.h and fill in
├── firebase-rules.json         # Firebase RTDB security rules
└── tests/                      # Arduino unit tests
```

## Getting Started

### Prerequisites

- Node.js 20+ and npm
- [Angular CLI](https://angular.dev/tools/cli): `npm install -g @angular/cli`
- [Capacitor CLI](https://capacitorjs.com): installed via `npm install`
- [Arduino IDE](https://www.arduino.cc/en/software) or Arduino CLI with ESP32 board support
- A Firebase project with **Realtime Database** and **Authentication** (email/password) enabled
- A [Better Stack](https://betterstack.com) source token (for device remote logging)
- A GitHub **fine-grained personal access token** with read access to releases (for OTA)

### 1. Clone and install

```bash
git clone https://github.com/AndroiableDroid/HomeAlone.git
cd HomeAlone
npm install
```

### 2. Configure the Angular app

Copy the example environment file and fill in your Firebase project credentials:

```bash
cp src/environments/environment.example.ts src/environments/environment.ts
```

Edit `src/environments/environment.ts`:

```ts
export const environment = {
    production: false,
    firebaseConfig: {
        apiKey: "AIza...",
        authDomain: "your-project.firebaseapp.com",
        databaseURL: "https://your-project-default-rtdb.firebaseio.com",
        projectId: "your-project",
        storageBucket: "your-project.appspot.com",
        messagingSenderId: "...",
        appId: "..."
    },
    bleAuthToken: "your-secret-token",  // must match BLE_AUTH_TOKEN in secrets.h
    blePin: "123456"                    // must match BLE_PIN in secrets.h
};
```

> `src/environments/environment.ts` is git-ignored — it will never be committed.

### 3. Configure the ESP32 firmware

Copy the example secrets file and fill in your credentials:

```bash
cp sketch_may13a/secrets.example.h sketch_may13a/secrets.h
```

Edit `sketch_may13a/secrets.h`:

```c
#define API_KEY          "AIza..."                           // Firebase Web API key
#define DATABASE_URL     "https://your-project.firebaseio.com"
#define USER_EMAIL       "device@example.com"               // Firebase Auth user for the device
#define USER_PASSWORD    "a-strong-password"
#define BLE_AUTH_TOKEN   "your-secret-token"                // must match bleAuthToken in environment.ts
#define BLE_PIN          123456                             // 6-digit BLE passkey shown on the device

#define GITHUB_OWNER     "your-username"
#define GITHUB_REPO      "your-repo"
#define GITHUB_TOKEN     "github_pat_..."                   // fine-grained PAT, releases:read

#define BETTERSTACK_TOKEN "your_betterstack_source_token"
#define BETTERSTACK_URL   "https://in.logs.betterstack.com"
```

> `secrets.h` is git-ignored — it will never be committed.

Flash the sketch via Arduino IDE or CLI to your ESP32.

### 4. Set up Firebase

1. In the Firebase console, create a Realtime Database with the following initial structure:
   ```json
   {
     "door":   { "status": "LOCKED" },
     "pump":   { "status": "OFF" },
     "device": { "command": "IDLE" },
     "ota":    { "enabled": false, "version": "", "tag": "" }
   }
   ```
2. Deploy the security rules from this repo:
   ```bash
   firebase deploy --only database
   ```
   Or paste the contents of `firebase-rules.json` into the Firebase console manually.

3. Create a Firebase Authentication user (email/password) for the device — use those credentials as `USER_EMAIL` / `USER_PASSWORD` in `secrets.h`.

4. Create a separate Firebase Authentication user for yourself — these are the credentials you'll log in with in the app.

### 5. Run the app locally

```bash
ng serve
```

Navigate to `http://localhost:4200/`.

### 6. Build and deploy to Android

```bash
ng build
npx cap sync android
npx cap open android
```

Build and run from Android Studio, or use `npx cap run android`.

## OTA Firmware Updates

1. Build a new `.bin` firmware file from Arduino IDE (`Sketch → Export Compiled Binary`).
2. Create a GitHub Release (e.g. `v1.0.7`) and attach the `.bin` as a release asset.
3. In Firebase RTDB, update:
   ```json
   { "ota": { "enabled": true, "version": "1.0.7", "tag": "v1.0.7" } }
   ```
   The device checks every 5 minutes and will flash itself automatically.

## BLE Security Model

BLE control uses three layered security options:

| Layer | Mechanism | Status |
|-------|-----------|--------|
| 1 | NimBLE passkey pairing (MITM + Secure Connections) | ✅ Active |
| 2 | App-level auth token (sent as first write per session) | ✅ Active |
| 3 | MAC address whitelist | ⚠️ Disabled (code present, `#if 0`) |

The BLE auth token and PIN can be rotated over BLE itself using the `SET_TOKEN:` and `SET_PIN:` commands (requires an already-authorized session).

## Timezone

The scheduled daily restart is set to **22:00 IST** (UTC+5:30). To change it, edit these defines in `sketch_may13a.ino`:

```c
#define SCHEDULED_RESTART_HOUR  22
#define NTP_TZ_OFFSET_SEC       19800   // IST: UTC+5:30
```

## License

MIT
