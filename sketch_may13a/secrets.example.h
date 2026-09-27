// Copy this file to secrets.h and replace all placeholder values with your real credentials.
// secrets.h is git-ignored and will never be committed.
#ifndef BUILD_OPT_H
#define BUILD_OPT_H

#define API_KEY        "AIza..."                           // Firebase Web API key
#define DATABASE_URL   "https://your-project.firebaseio.com"  // Firebase RTDB URL
#define USER_EMAIL     "device@example.com"                   // Firebase Auth user for the device
#define USER_PASSWORD  "your-device-account-password"
#define BLE_AUTH_TOKEN "your-secret-token"                    // must match bleAuthToken in environment.ts
#define BLE_PIN        123456                                 // 6-digit BLE passkey shown on device display

// GitHub repo coordinates for the OTA API calls
#define GITHUB_OWNER   "your-username"       // GitHub account that hosts the releases
#define GITHUB_REPO    "your-repo"            // Repository name
#define GITHUB_TOKEN   "github_pat_..."       // Fine-grained PAT with releases:read permission

#define BETTERSTACK_TOKEN "your_betterstack_source_token"   // Better Stack source token
#define BETTERSTACK_URL   "https://in.logs.betterstack.com"  // Better Stack ingest URL

#endif // BUILD_OPT_H