// Copy this file to environment.ts and replace all placeholder values with your real credentials.
// environment.ts is git-ignored and will never be committed.
export const environment = {
    production: false,
    firebaseConfig: {
        // ... your firebase config
    },
    bleAuthToken: "your-secret-token",  // must match BLE_AUTH_TOKEN in secrets.h
    blePin: "123456"                    // must match BLE_PIN in secrets.h
};
