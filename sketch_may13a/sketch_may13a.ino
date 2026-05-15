#include <WiFi.h>
#include <NimBLEDevice.h>
#include <Firebase_ESP_Client.h>
#include <Preferences.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#define API_KEY 
#define DATABASE_URL 
#define USER_EMAIL 
#define USER_PASSWORD 

// UUIDs for BLE Service and Characteristics
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID_SSID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHARACTERISTIC_UUID_PASS "6b4122d2-8a9d-4952-b8d2-4309320257e5"

const int relayPin = 27; // Door lock relay
const int pumpPin  = 26; // Water pump relay
const int ledPin   = 2;  // Built-in LED

// Only 2 FirebaseData objects to save RAM
FirebaseData streamFbdo;  // For command stream
FirebaseData reqFbdo;     // For door check, heartbeat, writes
FirebaseData pumpFbdo;    // For pump check (avoids collision with reqFbdo)
FirebaseAuth auth;
FirebaseConfig config;
Preferences preferences;

bool isFirebaseReady = false;
unsigned long lastHeartbeat = 0;
unsigned long lastDoorCheck = 0;
unsigned long lastPumpCheck = 0;
const unsigned long HEARTBEAT_INTERVAL = 15000;
const unsigned long DOOR_CHECK_INTERVAL = 1000;
const unsigned long PUMP_CHECK_INTERVAL = 5000;

bool isSetupMode = false;
bool shouldReboot = false;
String bleSsid = "";
String blePass = "";

// ─── LED Helpers ───────────────────────────────────────────

void blinkLed(int delayMs) {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  if (millis() - lastBlink >= delayMs) {
    lastBlink = millis();
    ledState = !ledState;
    digitalWrite(ledPin, ledState ? HIGH : LOW);
  }
}

void flashLed(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(ledPin, HIGH);
    delay(delayMs);
    digitalWrite(ledPin, LOW);
    delay(delayMs);
  }
}

// ─── Firebase Command Stream Callback ──────────────────────
// This ONLY listens to /device/command
void commandCallback(FirebaseStream data) {
  if (data.dataTypeEnum() != firebase_rtdb_data_type_string) return;

  String cmd = data.stringData();
  Serial.printf("[Stream] /device/command = %s\n", cmd.c_str());

  if (cmd == "RESET_WIFI") {
    Serial.println(">>> RESET_WIFI received! Clearing credentials...");
    // Acknowledge immediately
    Firebase.RTDB.setString(&reqFbdo, "/device/command", "IDLE");
    flashLed(5, 100);

    preferences.begin("wifi-creds", false);
    preferences.clear();
    preferences.end();

    delay(1000);
    ESP.restart();
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout, resuming...");
}

// ─── Door Check (polling, saves RAM vs. a second stream) ───
void checkDoorStatus() {
  if (!Firebase.ready()) return;

  if (Firebase.RTDB.getString(&reqFbdo, "/door/status")) {
    String val = reqFbdo.stringData();
    if (val == "UNLOCKED") {
      Serial.println(">>> UNLOCK command detected!");
      digitalWrite(relayPin, LOW);
      delay(2000);
      digitalWrite(relayPin, HIGH);
      Firebase.RTDB.setString(&reqFbdo, "/door/status", "LOCKED");
      Serial.println("Door re-locked.");
    }
  }
}

// ─── Pump Check ────────────────────────────────────────────
void checkPumpStatus() {
  if (!Firebase.ready()) return;

  if (Firebase.RTDB.getString(&pumpFbdo, "/pump/status")) {
    String val = pumpFbdo.stringData();
    Serial.printf("[Pump] status = %s\n", val.c_str());
    if (val == "ON") {
      Serial.println(">>> PUMP ON!");
      digitalWrite(pumpPin, LOW); // Activate relay
      Firebase.RTDB.setString(&pumpFbdo, "/pump/status", "RUNNING");
    } else if (val == "OFF") {
      Serial.println(">>> PUMP OFF!");
      digitalWrite(pumpPin, HIGH); // Deactivate relay
      Firebase.RTDB.setString(&pumpFbdo, "/pump/status", "STOPPED");
    }
  } else {
    Serial.printf("[Pump] read error: %s\n", pumpFbdo.errorReason().c_str());
  }
}

// ─── Heartbeat ─────────────────────────────────────────────
void sendHeartbeat() {
  if (!Firebase.ready()) return;

  FirebaseJson json;
  json.set("online", true);
  json.set("rssi", WiFi.RSSI());
  json.set("uptime", (int)(millis() / 1000));
  json.set("lastSeen/.sv", "timestamp");

  if (Firebase.RTDB.setJSON(&reqFbdo, "/device/status", &json)) {
    Serial.println("Heartbeat sent.");
  } else {
    Serial.println("Heartbeat failed: " + reqFbdo.errorReason());
  }
}

// ─── BLE Callbacks ─────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) {
    Serial.print("BLE Client connected: ");
    Serial.println(connInfo.getAddress().toString().c_str());
    digitalWrite(ledPin, HIGH);
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) {
    Serial.println("BLE Client disconnected. Resuming advertising...");
    digitalWrite(ledPin, LOW);
    NimBLEDevice::startAdvertising();
  }
};

class SsidCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) {
    std::string value = pCharacteristic->getValue();
    bleSsid = String(value.c_str());
    Serial.print("BLE Received SSID: ");
    Serial.println(bleSsid);
  }
};

class PassCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) {
    std::string value = pCharacteristic->getValue();
    blePass = String(value.c_str());
    Serial.println("BLE Received Password. Saving and rebooting...");

    if (bleSsid != "") {
      preferences.begin("wifi-creds", false);
      preferences.putString("ssid", bleSsid);
      preferences.putString("password", blePass);
      preferences.putInt("fail_count", 0);
      preferences.end();
      Serial.println("Credentials saved!");
      flashLed(5, 100);
      shouldReboot = true;
    } else {
      Serial.println("Error: SSID missing. Send SSID first.");
    }
  }
};

// ─── BLE Setup Mode ────────────────────────────────────────
void startSetupMode() {
  isSetupMode = true;
  Serial.println("Entering BLE Setup Mode...");

  NimBLEDevice::init("SmartLock-BLE");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);

  NimBLECharacteristic *pSsidChar = pService->createCharacteristic(
    CHARACTERISTIC_UUID_SSID, NIMBLE_PROPERTY::WRITE
  );
  pSsidChar->setCallbacks(new SsidCallbacks());

  NimBLECharacteristic *pPassChar = pService->createCharacteristic(
    CHARACTERISTIC_UUID_PASS, NIMBLE_PROPERTY::WRITE
  );
  pPassChar->setCallbacks(new PassCallbacks());

  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setName("SmartLock-BLE");
  advData.setFlags(0x06);
  pAdvertising->setAdvertisementData(advData);

  NimBLEAdvertisementData scanRsp;
  scanRsp.setCompleteServices(NimBLEUUID(SERVICE_UUID));
  pAdvertising->setScanResponseData(scanRsp);

  pAdvertising->start();
  Serial.println("BLE Advertising started. Device name: SmartLock-BLE");
}

// ─── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH); // HIGH = relay OFF (active-LOW)
  pinMode(pumpPin, OUTPUT);
  digitalWrite(pumpPin, HIGH);  // HIGH = relay OFF (active-LOW)
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  preferences.begin("wifi-creds", true); // Read-only
  String savedSsid = preferences.getString("ssid", "");
  String savedPass = preferences.getString("password", "");
  int failCount = preferences.getInt("fail_count", 0);
  preferences.end();

  if (savedSsid == "") {
    Serial.println("No saved credentials. Entering setup mode...");
    startSetupMode();
    return;
  }

  Serial.printf("Connecting to WiFi: %s (Attempt %d/3)\n", savedSsid.c_str(), failCount + 1);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  int counter = 0;
  while (WiFi.status() != WL_CONNECTED && counter < 30) {
    blinkLed(500);
    delay(500);
    Serial.print(".");
    counter++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    failCount++;
    preferences.begin("wifi-creds", false);
    preferences.putInt("fail_count", failCount);
    preferences.end();

    if (failCount >= 3) {
      Serial.println("\n3 WiFi failures. Clearing credentials...");
      preferences.begin("wifi-creds", false);
      preferences.clear();
      preferences.end();
    }
    startSetupMode();
    return;
  }

  // ── WiFi Connected ──
  Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
  digitalWrite(ledPin, HIGH);

  preferences.begin("wifi-creds", false);
  preferences.putInt("fail_count", 0);
  preferences.end();

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Wait for Firebase auth token (required before any DB operations)
  Serial.print("Waiting for Firebase token");
  int tokenWait = 0;
  while (!Firebase.ready() && tokenWait < 30) {
    delay(500);
    Serial.print(".");
    tokenWait++;
  }
  if (!Firebase.ready()) {
    Serial.println(" Failed! Rebooting...");
    delay(1000);
    ESP.restart();
  }
  Serial.println(" Ready!");

  // Clear any stale command BEFORE starting the stream
  Firebase.RTDB.setString(&reqFbdo, "/device/command", "IDLE");

  // Start command stream on /device/command ONLY
  if (!Firebase.RTDB.beginStream(&streamFbdo, "/device/command")) {
    Serial.printf("Command stream error: %s\n", streamFbdo.errorReason().c_str());
  }
  Firebase.RTDB.setStreamCallback(&streamFbdo, commandCallback, streamTimeoutCallback);

  isFirebaseReady = true;
  Serial.println("System online. Listening for commands...");
}

// ─── Loop ──────────────────────────────────────────────────
void loop() {
  if (shouldReboot) {
    delay(2000);
    ESP.restart();
  }

  if (isSetupMode) {
    blinkLed(200);
    return;
  }

  if (!isFirebaseReady) return;

  unsigned long now = millis();

  // Check door status every 5s
  if (now - lastDoorCheck >= DOOR_CHECK_INTERVAL) {
    lastDoorCheck = now;
    checkDoorStatus();
  }

  // Check pump status every 5s
  if (now - lastPumpCheck >= PUMP_CHECK_INTERVAL) {
    lastPumpCheck = now;
    checkPumpStatus();
  }

  // Send heartbeat every 15 seconds
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL || lastHeartbeat == 0) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
}