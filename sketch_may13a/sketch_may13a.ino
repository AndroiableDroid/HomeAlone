#include <WiFi.h>
#include <NimBLEDevice.h>
#include <Firebase_ESP_Client.h>
#include <Preferences.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#ifdef __has_include
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

// Fallback: catch-fire errors if any secret is still missing
#ifndef API_KEY
#  error "API_KEY not defined. Create secrets.h with your secrets."
#endif
#ifndef DATABASE_URL
#  error "DATABASE_URL not defined. Create secrets.h with your secrets."
#endif
#ifndef USER_EMAIL
#  error "USER_EMAIL not defined. Create secrets.h with your secrets."
#endif
#ifndef USER_PASSWORD
#  error "USER_PASSWORD not defined. Create secrets.h with your secrets."
#endif
#ifndef BLE_AUTH_TOKEN
#  error "BLE_AUTH_TOKEN not defined. Create secrets.h with your secrets."
#endif
#ifndef BLE_PIN
#  error "BLE_PIN not defined. Create secrets.h with your secrets."
#endif

// UUIDs for BLE Service and Characteristics
#define SERVICE_UUID             "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID_SSID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHARACTERISTIC_UUID_PASS "6b4122d2-8a9d-4952-b8d2-4309320257e5"
#define CHARACTERISTIC_UUID_CTRL "d2b45071-34aa-4161-80a5-e3d6229505f1"

const int relayPin = 27; // Door lock relay
const int pumpPin  = 26; // Water pump relay
const int ledPin   = 2;  // Built-in LED

FirebaseData streamFbdo;
FirebaseData reqFbdo;
FirebaseData pumpFbdo;
FirebaseAuth auth;
FirebaseConfig config;
Preferences preferences;

bool isFirebaseReady = false;
unsigned long lastHeartbeat = 0;
unsigned long lastDoorCheck = 0;
unsigned long lastPumpCheck = 0;
const unsigned long HEARTBEAT_INTERVAL  = 15000;
const unsigned long DOOR_CHECK_INTERVAL = 1000;
const unsigned long PUMP_CHECK_INTERVAL = 5000;
const unsigned long DOOR_RELOCK_DELAY_MS = 1000;

bool isSetupMode = false;
bool shouldReboot = false;
String bleSsid = "";
String blePass = "";
bool isBleOnly = false;

// ─── SECURITY OPTION 2: App-level token auth ───────────────
// Loaded from Preferences at boot; fallback default shown here.
// Change via Firebase: write new token to /device/ble_token
String bleAuthToken = BLE_AUTH_TOKEN;  // fallback from secrets.h
bool bleSessionAuthorized = false;

// ─── SECURITY OPTION 3: MAC whitelist ──────────────────────
// TODO: disabled for now — enable when ready
#if 0
#define MAX_MAC_WHITELIST 5
String macWhitelist[MAX_MAC_WHITELIST];

void loadMacWhitelist() {
  preferences.begin("ble-auth", true);
  for (int i = 0; i < MAX_MAC_WHITELIST; i++) {
    macWhitelist[i] = preferences.getString(("mac" + String(i)).c_str(), "");
  }
  preferences.end();
}

bool isMacAllowed(const String &addr) {
  // If whitelist is completely empty, allow all (open mode, not recommended)
  bool anyEntry = false;
  for (int i = 0; i < MAX_MAC_WHITELIST; i++) {
    if (macWhitelist[i] != "") { anyEntry = true; break; }
  }
  if (!anyEntry) return true; // whitelist not configured yet

  for (int i = 0; i < MAX_MAC_WHITELIST; i++) {
    if (macWhitelist[i].equalsIgnoreCase(addr)) return true;
  }
  return false;
}
#endif // MAC whitelist

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

// ─── Hardware Action Helpers ────────────────────────────────

void unlockDoorAction() {
  Serial.println(">>> UNLOCKING...");
  digitalWrite(relayPin, LOW);
  delay(DOOR_RELOCK_DELAY_MS);
  digitalWrite(relayPin, HIGH);
  Serial.println(">>> LOCKED.");
}

void setPumpAction(bool on) {
  if (on) {
    Serial.println(">>> PUMP ON!");
    digitalWrite(pumpPin, LOW);
  } else {
    Serial.println(">>> PUMP OFF!");
    digitalWrite(pumpPin, HIGH);
  }
}

// ─── Firebase Command Stream Callback ──────────────────────
void commandCallback(FirebaseStream data) {
  if (data.dataTypeEnum() != firebase_rtdb_data_type_string) return;

  String cmd = data.stringData();
  Serial.printf("[Stream] /device/command = %s\n", cmd.c_str());

  if (cmd == "RESET_WIFI") {
    Serial.println(">>> RESET_WIFI received! Clearing credentials...");
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

// ─── Status Checks (polling) ───────────────────────────────

void checkDoorStatus() {
  if (!Firebase.ready()) return;
  if (Firebase.RTDB.getString(&reqFbdo, "/door/status")) {
    String val = reqFbdo.stringData();
    if (val == "UNLOCKED") {
      unlockDoorAction();
      Firebase.RTDB.setString(&reqFbdo, "/door/status", "LOCKED");
    }
  }
}

void checkPumpStatus() {
  if (!Firebase.ready()) return;
  if (Firebase.RTDB.getString(&pumpFbdo, "/pump/status")) {
    String val = pumpFbdo.stringData();
    if (val == "ON") {
      setPumpAction(true);
      Firebase.RTDB.setString(&pumpFbdo, "/pump/status", "RUNNING");
    } else if (val == "OFF") {
      setPumpAction(false);
      Firebase.RTDB.setString(&pumpFbdo, "/pump/status", "STOPPED");
    }
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
  Firebase.RTDB.setJSON(&reqFbdo, "/device/status", &json);
}

// ─── BLE Control Callbacks (with all 3 security layers) ────
//
//  Layer 1 (OPTION 1): NimBLE passkey pairing — enforced at the
//                       BLE stack level before onWrite is ever called.
//  Layer 2 (OPTION 2): App-level token — first write to CTRL char
//                       must be the secret token; commands only work
//                       after that in the same session.
//  Layer 3 (OPTION 3): MAC whitelist — DISABLED, enable when ready.

class CtrlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) {
    String cmd = String(pCharacteristic->getValue().c_str());
    Serial.printf("[BLE CTRL] Received: %s\n", cmd.c_str());

    // ── OPTION 2: Token gate ──────────────────────────────
    if (!bleSessionAuthorized) {
      if (cmd == bleAuthToken) {
        bleSessionAuthorized = true;
        Serial.println("[BLE] Session authorized.");
        flashLed(2, 80);
      } else {
        Serial.println("[BLE] Auth FAILED — wrong token.");
        flashLed(5, 50); // Fast angry blink
      }
      return; // Never execute a command in the same write as auth
    }

    // ── Authorized commands ───────────────────────────────
    if (cmd == "UNLOCK") {
      unlockDoorAction();
    } else if (cmd == "PUMP_ON") {
      setPumpAction(true);
    } else if (cmd == "PUMP_OFF") {
      setPumpAction(false);
    } else if (cmd == "MODE_WIFI") {
      preferences.begin("system", false);
      preferences.putBool("ble_only", false);
      preferences.end();
      ESP.restart();
    } else if (cmd == "MODE_BLE") {
      preferences.begin("system", false);
      preferences.putBool("ble_only", true);
      preferences.end();
      ESP.restart();
    // ADD_MAC command disabled along with MAC whitelist (OPTION 3)
#if 0
    } else if (cmd.startsWith("ADD_MAC:")) {
      // Allow owner to add a new MAC to the whitelist over BLE
      // Usage: send "ADD_MAC:aa:bb:cc:dd:ee:ff"
      String newMac = cmd.substring(8);
      newMac.trim();
      preferences.begin("ble-auth", false);
      for (int i = 0; i < MAX_MAC_WHITELIST; i++) {
        String key = "mac" + String(i);
        if (macWhitelist[i] == "" || macWhitelist[i].equalsIgnoreCase(newMac)) {
          macWhitelist[i] = newMac;
          preferences.putString(key.c_str(), newMac);
          Serial.printf("[BLE] MAC whitelisted: %s\n", newMac.c_str());
          break;
        }
      }
      preferences.end();
#endif // ADD_MAC (OPTION 3)
    } else if (cmd.startsWith("SET_TOKEN:")) {
      // Allow owner to rotate the auth token over BLE
      // Usage: send "SET_TOKEN:mynewsecret"
      String newToken = cmd.substring(10);
      newToken.trim();
      preferences.begin("ble-auth", false);
      preferences.putString("token", newToken);
      preferences.end();
      bleAuthToken = newToken;
      Serial.println("[BLE] Auth token updated.");
    } else if (cmd.startsWith("SET_PIN:")) {
      // Allow owner to change the BLE passkey over BLE (takes effect after reboot)
      // Usage: send "SET_PIN:654321"
      int newPin = cmd.substring(8).toInt();
      if (newPin >= 100000 && newPin <= 999999) {
        preferences.begin("ble-auth", false);
        preferences.putInt("pin", newPin);
        preferences.end();
        Serial.printf("[BLE] PIN updated to %d. Reboot to apply.\n", newPin);
      } else {
        Serial.println("[BLE] Invalid PIN. Must be 6 digits.");
      }
    } else {
      Serial.printf("[BLE] Unknown command: %s\n", cmd.c_str());
    }
  }
};

// ─── BLE Server Callbacks ──────────────────────────────────

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) {
    String addr = connInfo.getAddress().toString().c_str();
    Serial.printf("[BLE] Client connected: %s\n", addr.c_str());

    // ── OPTION 3: MAC whitelist check (disabled) ──────────
#if 0
    if (!isMacAllowed(addr)) {
      Serial.printf("[BLE] MAC not whitelisted: %s — disconnecting.\n", addr.c_str());
      flashLed(3, 100);
      pServer->disconnect(connInfo.getConnHandle());
      return;
    }
#endif

    digitalWrite(ledPin, HIGH);

    // Force the client (smartphone) to initiate pairing/encryption immediately.
    // This solves the issue where Android/Windows fails to show the PIN dialog on write.
    NimBLEDevice::startSecurity(connInfo.getConnHandle());
  }

  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) {
    Serial.println("[BLE] Client disconnected. Resuming advertising...");
    digitalWrite(ledPin, LOW);

    // ── OPTION 2: Reset session token on every disconnect ─
    bleSessionAuthorized = false;
    Serial.println("[BLE] Session deauthorized.");

    NimBLEDevice::startAdvertising();
  }
};

// ─── BLE Credential Callbacks ──────────────────────────────

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

      preferences.begin("system", false);
      preferences.putBool("ble_only", false);
      preferences.end();

      Serial.println("Credentials saved!");
      flashLed(5, 100);
      shouldReboot = true;
    } else {
      Serial.println("Error: SSID missing. Send SSID first.");
    }
  }
};

// ─── BLE Server Init ───────────────────────────────────────

void startBleServer() {
  // ── OPTION 1: NimBLE passkey (MITM + Secure Connections) ─
  // Load PIN from Preferences, fallback to BLE_PIN
  preferences.begin("ble-auth", true);
  int blePin = preferences.getInt("pin", BLE_PIN);
  preferences.end();

  // init() MUST come first — it starts the NimBLE stack.
  // Security settings applied after init() stick; before init() they get wiped.
  NimBLEDevice::init("SmartLock-BLE");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Enable Bonding, MITM (requires PIN), and Secure Connections
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityPasskey(blePin);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  Serial.printf("[BLE] Passkey auth enabled. PIN: %d\n", blePin);

  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);

  // SSID & Password chars: encrypted writes required (OPTION 1)
  pService->createCharacteristic(
    CHARACTERISTIC_UUID_SSID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN
  )->setCallbacks(new SsidCallbacks());

  pService->createCharacteristic(
    CHARACTERISTIC_UUID_PASS,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN
  )->setCallbacks(new PassCallbacks());

  // Control char: encrypted + app-token gated (OPTION 1 + 2)
  pService->createCharacteristic(
    CHARACTERISTIC_UUID_CTRL,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN
  )->setCallbacks(new CtrlCallbacks());

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
  Serial.println("[BLE] Server started.");
}

// ─── BLE Setup Mode ────────────────────────────────────────

void startSetupMode() {
  isSetupMode = true;
  Serial.println("Entering BLE Setup Mode...");
  // startBleServer() already called before this; just log.
  Serial.println("Send SSID then password to configure WiFi.");
}

// ─── Setup ─────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);
  pinMode(pumpPin, OUTPUT);
  digitalWrite(pumpPin, HIGH);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  // ── Load system flags ─────────────────────────────────
  preferences.begin("system", true);
  isBleOnly = preferences.getBool("ble_only", false);
  preferences.end();

  // ── Load BLE auth token (OPTION 2) ────────────────────
  preferences.begin("ble-auth", true);
  bleAuthToken = preferences.getString("token", BLE_AUTH_TOKEN);
  preferences.end();
  Serial.println("[Security] BLE auth token loaded.");

  // ── Load MAC whitelist (OPTION 3 — disabled) ──────────
#if 0
  loadMacWhitelist();
  Serial.println("[Security] MAC whitelist loaded.");
#endif

  // ── Load WiFi credentials ─────────────────────────────
  preferences.begin("wifi-creds", true);
  String savedSsid = preferences.getString("ssid", "");
  String savedPass = preferences.getString("password", "");
  int failCount    = preferences.getInt("fail_count", 0);
  preferences.end();

  if (savedSsid == "" || isBleOnly) {
    if (isBleOnly) Serial.println("Privacy Mode: BLE Only active.");
    else           Serial.println("No saved credentials.");

    startBleServer();
    if (isBleOnly) return;

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
    startBleServer();
    startSetupMode();
    return;
  }

  Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
  digitalWrite(ledPin, HIGH);

  preferences.begin("wifi-creds", false);
  preferences.putInt("fail_count", 0);
  preferences.end();

  config.api_key        = API_KEY;
  config.database_url   = DATABASE_URL;
  auth.user.email       = USER_EMAIL;
  auth.user.password    = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

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

  Firebase.RTDB.setString(&reqFbdo, "/device/command", "IDLE");

  if (!Firebase.RTDB.beginStream(&streamFbdo, "/device/command")) {
    Serial.printf("Command stream error: %s\n", streamFbdo.errorReason().c_str());
  }
  Firebase.RTDB.setStreamCallback(&streamFbdo, commandCallback, streamTimeoutCallback);

  isFirebaseReady = true;
  Serial.println("System online. Listening for commands...");

  // Always start BLE for direct control even in WiFi mode
  startBleServer();
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

  if (now - lastDoorCheck >= DOOR_CHECK_INTERVAL) {
    lastDoorCheck = now;
    checkDoorStatus();
  }

  if (now - lastPumpCheck >= PUMP_CHECK_INTERVAL) {
    lastPumpCheck = now;
    checkPumpStatus();
  }

  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL || lastHeartbeat == 0) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
}