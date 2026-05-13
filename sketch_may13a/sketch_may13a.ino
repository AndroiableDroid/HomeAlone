#include <WiFi.h>
#include <Firebase_ESP_Client.h>

// Provide the token generation process info.
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#define WIFI_SSID "14/12/17"
#define WIFI_PASSWORD "EDC10C2B"

#define API_KEY "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL "YOUR_FIREBASE_DATABASE_URL"
#define USER_EMAIL "YOUR_AUTH_EMAIL"
#define USER_PASSWORD "YOUR_AUTH_PASSWORD"

const int relayPin = 5;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Callback function to handle real-time database changes
void streamCallback(FirebaseStream data) {
  if (data.dataType() == "string" && data.stringData() == "UNLOCKED") {
    Serial.println("Secure command received. Unlocking...");
    
    digitalWrite(relayPin, LOW); // Trigger relay
    delay(2000);                 // Keep unlocked for 2 seconds
    digitalWrite(relayPin, HIGH); // Lock again
    
    // Reset the database state back to LOCKED
    Firebase.RTDB.setString(&fbdo, "/door/status", "LOCKED");
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout, resuming...");
}

void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);

  // Connect to Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(300); 
    Serial.print("."); 
  }
  Serial.println("\nWiFi connected.");

  // Configure Firebase Auth
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  // Initialize Firebase and start the stream
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (!Firebase.RTDB.beginStream(&fbdo, "/door/status")) {
    Serial.printf("Stream error: %s\n", fbdo.errorReason().c_str());
  }
  Firebase.RTDB.setStreamCallback(&fbdo, streamCallback, streamTimeoutCallback);
}

void loop() {
  // The stream is handled asynchronously in the background
}