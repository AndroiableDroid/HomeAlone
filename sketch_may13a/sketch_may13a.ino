#include <WiFi.h>
#include <NimBLEDevice.h>
#include <Firebase_ESP_Client.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>  // ESP32 core 3.x: setInsecure() lives here
#include <Update.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// Firmware version -- bump this string with every OTA release
#define APP_VERSION    "1.0.6"

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
#ifndef GITHUB_TOKEN
#  error "GITHUB_TOKEN not defined. Add your fine-grained PAT to secrets.h."
#endif
#ifndef BETTERSTACK_TOKEN
#  error "BETTERSTACK_TOKEN not defined. Add your Better Stack source token to secrets.h."
#endif
#ifndef BETTERSTACK_URL
#  error "BETTERSTACK_URL not defined. Add your Better Stack URL endpoint to secrets.h."
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
unsigned long lastOtaCheck  = 0;
unsigned long lastInetCheck = 0;
const unsigned long HEARTBEAT_INTERVAL  = 15000;
const unsigned long DOOR_CHECK_INTERVAL = 1000;
const unsigned long PUMP_CHECK_INTERVAL = 5000;
const unsigned long OTA_CHECK_INTERVAL  = 300000; // 5 minutes
const unsigned long INET_CHECK_INTERVAL = 15000;  // 15 seconds

// Internet connectivity watchdog: after this many consecutive failures,
// clear WiFi credentials and reboot into BLE setup mode.
#define INET_FAIL_THRESHOLD  5
int inetFailCount = 0;

// ─── Scheduled restart & Activity tracking ─────────────────
// Reboot every day at 22:00 (10 PM) local time to keep the device fresh.
// IST = UTC + 5h30m = 19800 s offset, no DST.
#define SCHEDULED_RESTART_HOUR  22
#define NTP_TZ_OFFSET_SEC       19800   // IST: UTC+5:30
bool scheduledRestartFired = false;     // guard: fire once per boot
const unsigned long DOOR_RELOCK_DELAY_MS = 1000;

// Track last user activity (unlocking door, toggling pump, BLE commands, etc.)
// Reboots if inactive for a long time (default: 6 hours) to prevent freeze-ups.
unsigned long lastActivityTime = 0;
#define INACTIVITY_RESTART_TIMEOUT_MS  21600000 // 6 hours in milliseconds

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
  lastActivityTime = millis();
  Serial.println(">>> UNLOCKING...");
  digitalWrite(relayPin, LOW);
  delay(DOOR_RELOCK_DELAY_MS);
  digitalWrite(relayPin, HIGH);
  Serial.println(">>> LOCKED.");
}

void setPumpAction(bool on) {
  lastActivityTime = millis();
  if (on) {
    Serial.println(">>> PUMP ON!");
    digitalWrite(pumpPin, LOW);
  } else {
    Serial.println(">>> PUMP OFF!");
    digitalWrite(pumpPin, HIGH);
  }
}

// Send logs over the internet to Better Stack dashboard
void remoteLog(const String &level, const String &msg) {
  if (WiFi.status() != WL_CONNECTED) return;

  NetworkClientSecure client;
  client.setInsecure(); // Skip certificate verification to save memory and flash size

  HTTPClient http;
  http.begin(client, BETTERSTACK_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " BETTERSTACK_TOKEN);

  String payload = "{\"level\":\"" + level + 
                   "\",\"message\":\"" + msg + 
                   "\",\"version\":\"" APP_VERSION "\"}";

  int httpCode = http.POST(payload);
  http.end();
}

// ─── Internet Reachability Check ───────────────────────────
// Lightweight test: Google's generate_204 returns HTTP 204 with an empty
// body.  If we get *any* HTTP response we have internet.  A connect or
// timeout failure means no internet.  Uses plain HTTP (no TLS overhead).
bool hasInternet() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin("http://clients3.google.com/generate_204");
  http.setTimeout(5000);
  int code = http.GET();
  http.end();

  return (code > 0);  // any HTTP response means we reached the internet
}

// ─── OTA Update ────────────────────────────────────────────
//
// All OTA HTTPS calls share a single NetworkClientSecure with setInsecure()
// to avoid heap fragmentation. Each TLS session allocates ~40 KB; creating
// three separate clients exhausts the ESP32's ~160 KB of usable heap.
// By reusing one client we keep peak memory low and predictable.

// Resolve the GitHub release tag to an API asset download URL.
// Returns "" on any error.
String resolveGitHubAssetUrl(NetworkClientSecure &client, const String &tag) {
  String apiUrl = "https://api.github.com/repos/"
                  GITHUB_OWNER "/" GITHUB_REPO "/releases/tags/" + tag;
  Serial.printf("[OTA] Querying: %s\n", apiUrl.c_str());

  HTTPClient http;
  http.begin(client, apiUrl);
  http.addHeader("Authorization",        "Bearer " GITHUB_TOKEN);
  http.addHeader("Accept",               "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.setTimeout(10000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] API error: %d\n", code);
    remoteLog("error", "[OTA] API release check error: " + String(code));
    http.end();
    return "";
  }

  String body = http.getString();
  http.end();

  // Find the first asset URL inside the "assets" array.
  const String prefix = "https://api.github.com/repos/"
                        GITHUB_OWNER "/" GITHUB_REPO "/releases/assets/";
  int assetsIdx = body.indexOf("\"assets\":");
  if (assetsIdx == -1) {
    Serial.println("[OTA] No assets key in response.");
    return "";
  }
  int urlIdx = body.indexOf(prefix, assetsIdx);
  if (urlIdx == -1) {
    Serial.println("[OTA] No asset uploaded to this release.");
    return "";
  }
  int urlEnd = body.indexOf('"', urlIdx);
  if (urlEnd == -1) return "";

  String assetUrl = body.substring(urlIdx, urlEnd);
  Serial.printf("[OTA] Asset URL: %s\n", assetUrl.c_str());
  return assetUrl;
}

// Download and flash a firmware binary.
//
// Flow:
//   Step 1: GET api.github.com/.../assets/<id>  (Bearer auth)
//           -> 302 to release-assets.githubusercontent.com
//   Step 2: GET release-assets URL               (no auth)
//           -> stream binary to Update
void performOTA(NetworkClientSecure &client, const String &assetApiUrl) {
  Serial.println("[OTA] Starting update...");
  remoteLog("info", "[OTA] Starting firmware update from asset API");
  flashLed(3, 100);

  // -- Step 1: get the CDN redirect from the API asset URL --------------------
  HTTPClient apiHttp;
  apiHttp.begin(client, assetApiUrl);
  apiHttp.addHeader("Authorization",        "Bearer " GITHUB_TOKEN);
  apiHttp.addHeader("Accept",               "application/octet-stream");
  apiHttp.addHeader("X-GitHub-Api-Version", "2022-11-28");
  apiHttp.setTimeout(10000);
  apiHttp.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  int apiCode = apiHttp.GET();
  String cdnUrl = "";

  if (apiCode == HTTP_CODE_FOUND             // 302 expected
   || apiCode == HTTP_CODE_MOVED_PERMANENTLY // 301
   || apiCode == 303) {
    cdnUrl = apiHttp.getLocation();
    Serial.printf("[OTA] CDN URL resolved (%d chars)\n", cdnUrl.length());
  } else {
    Serial.printf("[OTA] API did not redirect: %d (%s)\n",
                  apiCode, apiHttp.errorToString(apiCode).c_str());
    remoteLog("error", "[OTA] API did not redirect. Code: " + String(apiCode) + " (" + apiHttp.errorToString(apiCode) + ")");
    apiHttp.end();
    return;
  }
  apiHttp.end();

  if (cdnUrl == "") {
    Serial.println("[OTA] Empty CDN URL, aborting.");
    return;
  }

  // Small delay to let the TCP stack fully release the previous connection
  delay(100);

  // -- Step 2: download binary from CDN (no auth) -----------------------------
  HTTPClient cdnHttp;
  cdnHttp.begin(client, cdnUrl);               // reuse the same TLS client
  cdnHttp.setTimeout(30000);
  cdnHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int cdnCode = cdnHttp.GET();

  if (cdnCode != HTTP_CODE_OK) {
    Serial.printf("[OTA] CDN error: %d (%s)\n",
                  cdnCode, cdnHttp.errorToString(cdnCode).c_str());
    remoteLog("error", "[OTA] CDN download error: " + String(cdnCode) + " (" + cdnHttp.errorToString(cdnCode) + ")");
    cdnHttp.end();
    return;
  }

  int totalSize = cdnHttp.getSize();
  Serial.printf("[OTA] Firmware size: %d bytes\n", totalSize);

  if (!Update.begin(totalSize > 0 ? totalSize : UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("[OTA] Cannot begin: %s\n", Update.errorString());
    remoteLog("error", "[OTA] Cannot begin flashing: " + String(Update.errorString()));
    cdnHttp.end();
    return;
  }

  WiFiClient *stream = cdnHttp.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  Serial.printf("[OTA] Written %u / %d bytes\n", written, totalSize);

  if (!Update.end(true)) {
    Serial.printf("[OTA] Finalize failed: %s\n", Update.errorString());
    remoteLog("error", "[OTA] Flashing finalize failed: " + String(Update.errorString()));
    cdnHttp.end();
    return;
  }

  cdnHttp.end();
  Serial.println("[OTA] Update complete! Rebooting...");
  remoteLog("info", "[OTA] Update complete! Rebooting to new version.");
  delay(1000); // Allow remoteLog http request to finish
  flashLed(10, 50);
  delay(500);
  ESP.restart();
}

// Poll Firebase for a newer firmware version and trigger OTA if found.
// Firebase schema:
//   /ota/enabled  (bool)   -- safety switch; set false to pause OTA
//   /ota/version  (string) -- available version, e.g. "1.0.2"
//   /ota/tag      (string) -- matching GitHub release tag, e.g. "v1.0.2"
void checkOTA() {
  if (!Firebase.ready()) return;

  if (Firebase.RTDB.getBool(&reqFbdo, "/ota/enabled")) {
    if (!reqFbdo.boolData()) {
      Serial.println("[OTA] OTA disabled via Firebase.");
      return;
    }
  }

  if (!Firebase.RTDB.getString(&reqFbdo, "/ota/version")) return;
  String available = reqFbdo.stringData();
  if (available == "" || available == APP_VERSION) return;

  Serial.printf("[OTA] New version: %s (running: %s)\n",
                available.c_str(), APP_VERSION);
  remoteLog("info", "[OTA] Found new version online: " + available + " (running: " + String(APP_VERSION) + ")");

  if (!Firebase.RTDB.getString(&reqFbdo, "/ota/tag")) return;
  String tag = reqFbdo.stringData();
  if (tag == "") {
    Serial.println("[OTA] /ota/tag is empty, skipping.");
    return;
  }

  // Create one shared TLS client for the entire OTA process.
  // setInsecure() skips cert verification, which is acceptable here because
  // the URLs come from our own Firebase RTDB and the GitHub API validates
  // via Bearer token before serving content.
  NetworkClientSecure otaClient;
  otaClient.setInsecure();

  String assetUrl = resolveGitHubAssetUrl(otaClient, tag);
  if (assetUrl == "") return;

  performOTA(otaClient, assetUrl);
}

// ─── Firebase Command Stream Callback ──────────────────────
void commandCallback(FirebaseStream data) {
  if (data.dataTypeEnum() != firebase_rtdb_data_type_string) return;

  String cmd = data.stringData();
  if (cmd != "IDLE") {
    lastActivityTime = millis();
  }
  Serial.printf("[Stream] /device/command = %s\n", cmd.c_str());
  remoteLog("info", "[Stream] Received remote command: " + cmd);

  if (cmd == "RESET_WIFI") {
    Serial.println(">>> RESET_WIFI received! Clearing credentials...");
    Firebase.RTDB.setString(&reqFbdo, "/device/command", "IDLE");
    flashLed(5, 100);
    preferences.begin("wifi-creds", false);
    preferences.clear();
    preferences.end();
    delay(1000);
    ESP.restart();
  } else if (cmd == "OTA_UPDATE") {
    Serial.println(">>> OTA_UPDATE received! Checking for update...");
    Firebase.RTDB.setString(&reqFbdo, "/device/command", "IDLE");
    checkOTA(); // Immediate check, does not wait for the poll interval
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
      remoteLog("info", "[Action] Door unlocked remotely via Firebase");
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
      remoteLog("info", "[Action] Water Pump set to ON");
      Firebase.RTDB.setString(&pumpFbdo, "/pump/status", "RUNNING");
    } else if (val == "OFF") {
      setPumpAction(false);
      remoteLog("info", "[Action] Water Pump set to OFF");
      Firebase.RTDB.setString(&pumpFbdo, "/pump/status", "STOPPED");
    }
  }
}

// ─── Scheduled Restart ────────────────────────────────────
// Called every loop(). Reboots at SCHEDULED_RESTART_HOUR (local time).
// The guard flag resets naturally because the device reboots.
void checkScheduledRestart() {
  if (scheduledRestartFired) return;

  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 0)) return; // NTP not ready yet

  if (timeInfo.tm_hour == SCHEDULED_RESTART_HOUR && timeInfo.tm_min == 0) {
    scheduledRestartFired = true; // prevent double-trigger this minute
    Serial.printf("[Scheduler] %02d:00 reached — rebooting for daily maintenance.\n",
                  SCHEDULED_RESTART_HOUR);
    remoteLog("info", "[Scheduler] Daily scheduled reboot at 22:00.");
    delay(1000); // let remoteLog finish
    ESP.restart();
  }
}

// ─── Inactivity Restart ───────────────────────────────────
void checkInactivityRestart() {
  if (millis() - lastActivityTime >= INACTIVITY_RESTART_TIMEOUT_MS) {
    Serial.println("[Scheduler] Device inactive for too long — rebooting.");
    remoteLog("info", "[Scheduler] Rebooting due to inactivity.");
    delay(1000); // let remoteLog finish
    ESP.restart();
  }
}

// ─── Heartbeat ─────────────────────────────────────────────
void sendHeartbeat() {
  if (!Firebase.ready()) return;
  FirebaseJson json;
  json.set("online", true);
  json.set("rssi", WiFi.RSSI());
  json.set("uptime", (int)(millis() / 1000));
  json.set("version", APP_VERSION);
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
    lastActivityTime = millis();
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
    lastActivityTime = millis();
    std::string value = pCharacteristic->getValue();
    bleSsid = String(value.c_str());
    Serial.print("BLE Received SSID: ");
    Serial.println(bleSsid);
  }
};

class PassCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) {
    lastActivityTime = millis();
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
  preferences.end();

  if (savedSsid == "" || isBleOnly) {
    if (isBleOnly) Serial.println("Privacy Mode: BLE Only active.");
    else           Serial.println("No saved credentials.");

    startBleServer();
    if (isBleOnly) return;

    startSetupMode();
    return;
  }

  Serial.printf("Connecting to WiFi: %s\n", savedSsid.c_str());
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  // Wait up to 15 s for connection
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED && counter < 30) {
    blinkLed(500);
    delay(500);
    Serial.print(".");
    counter++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    wl_status_t wifiStatus = WiFi.status();

    // ── Distinguish failure type ────────────────────────────
    // WL_CONNECT_FAILED  → router rejected the password (bad creds)
    // WL_NO_SSID_AVAIL   → SSID not found (wrong SSID or router restarting)
    // WL_DISCONNECTED    → timeout / router temporarily offline
    bool badCredentials = (wifiStatus == WL_CONNECT_FAILED);

    if (badCredentials) {
      // Confirmed wrong password: wipe creds immediately so user
      // can re-provision via BLE without waiting for 3 attempts.
      Serial.println("[WiFi] Auth rejected by router. Clearing credentials.");
      preferences.begin("wifi-creds", false);
      preferences.clear();
      preferences.end();
      startBleServer();
      startSetupMode();   // Enter provisioning mode
    } else {
      // Transient failure (router restarting, signal lost, power blip).
      // Keep saved credentials -- next reboot will retry automatically.
      Serial.printf("[WiFi] Transient failure (status=%d). Credentials preserved.\n", wifiStatus);
      Serial.println("[WiFi] Starting BLE standby. Device will retry on next reboot.");
      startBleServer();   // BLE available for manual re-provisioning if needed
      // Do NOT call startSetupMode() -- flag stays false so loop() runs normally
      // but isFirebaseReady=false keeps Firebase ops gated.
    }
    return;
  }

  Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());

  // ── Verify internet reachability before proceeding ────────
  // WiFi may associate to the router even when the router has no
  // upstream internet.  A quick HTTP check catches this early.
  Serial.print("[WiFi] Checking internet reachability...");
  bool internetOk = false;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (hasInternet()) { internetOk = true; break; }
    Serial.print(".");
    delay(2000);
  }

  if (!internetOk) {
    Serial.println(" No internet!");
    Serial.println("[WiFi] Connected to router but no internet. Entering BLE setup.");
    WiFi.disconnect(true);
    startBleServer();
    startSetupMode();
    return;
  }
  Serial.println(" OK");

  // ── Sync NTP for scheduled-restart time-keeping ───────────
  configTime(NTP_TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[NTP] Syncing time");
  struct tm ntpCheck;
  for (int i = 0; i < 20 && !getLocalTime(&ntpCheck, 0); i++) {
    delay(500);
    Serial.print(".");
  }
  if (getLocalTime(&ntpCheck, 0)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ntpCheck);
    Serial.printf(" OK: %s IST\n", buf);
  } else {
    Serial.println(" NTP sync failed (will retry in background).");
  }
  digitalWrite(ledPin, HIGH);

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
  remoteLog("info", "System online. Listening for commands. IP: " + WiFi.localIP().toString());

  // Always start BLE for direct control even in WiFi mode
  startBleServer();

  lastActivityTime = millis();
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

  // Check scheduled daily restart and inactivity restart
  checkScheduledRestart();
  checkInactivityRestart();

  if (!isFirebaseReady) return;

  unsigned long now = millis();

  // ── Internet watchdog: detect prolonged internet loss ─────
  // If the router loses upstream internet while we are running,
  // Firebase ops will silently fail. After INET_FAIL_THRESHOLD
  // consecutive failed pings, clear creds and reboot into BLE
  // setup so the user can provision a new network.
  if (now - lastInetCheck >= INET_CHECK_INTERVAL) {
    lastInetCheck = now;
    if (hasInternet()) {
      inetFailCount = 0;
    } else {
      inetFailCount++;
      Serial.printf("[WiFi] Internet check failed (%d/%d)\n",
                    inetFailCount, INET_FAIL_THRESHOLD);
      if (inetFailCount >= INET_FAIL_THRESHOLD) {
        Serial.println("[WiFi] Internet lost for too long. Clearing creds, entering BLE setup.");
        remoteLog("warn", "[WiFi] Internet lost — rebooting to BLE setup.");
        WiFi.disconnect(true);
        preferences.begin("wifi-creds", false);
        preferences.clear();
        preferences.end();
        delay(1000);
        ESP.restart();
      }
    }
  }

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

  if (now - lastOtaCheck >= OTA_CHECK_INTERVAL || lastOtaCheck == 0) {
    lastOtaCheck = now;
    checkOTA();
  }
}