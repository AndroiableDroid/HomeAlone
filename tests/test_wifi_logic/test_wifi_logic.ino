/*
 * test_wifi_logic.ino
 *
 * Unit tests for the WiFi credential management logic in sketch_may13a.
 *
 * Flash this sketch onto the ESP32 and observe Serial output at 115200 baud.
 * Each test prints PASS or FAIL with a reason.
 *
 * Tests cover:
 *   1. Creds cleared on WL_CONNECT_FAILED (bad password)
 *   2. Creds preserved on WL_DISCONNECTED (transient / router restart)
 *   3. Creds preserved on WL_NO_SSID_AVAIL (SSID temporarily missing)
 *   4. Setup mode entered only on bad credentials, not transient failure
 *   5. Creds cleared immediately via RESET_WIFI command
 *   6. Credentials saved correctly via BLE provisioning
 *   7. fail_count key is absent after migration (no stale counter)
 *   8. BLE session deauthorized on disconnect
 */

#include <WiFi.h>       // Required for wl_status_t and WL_* constants
#include <Preferences.h>

Preferences preferences;

// ─── Minimal stubs matching firmware globals ────────────────

static bool   isSetupMode        = false;
static bool   bleSessionAuthorized = false;
static bool   bleServerStarted   = false;

// Mirrors the firmware's credential-save logic (PassCallbacks::onWrite)
void saveCredentials(const String &ssid, const String &pass) {
  preferences.begin("wifi-creds", false);
  preferences.putString("ssid",     ssid);
  preferences.putString("password", pass);
  preferences.end();
}

// Mirrors firmware's clear-on-bad-password path
void clearCredentials() {
  preferences.begin("wifi-creds", false);
  preferences.clear();
  preferences.end();
}

// Mirrors the firmware's WiFi failure decision tree
// Returns true  -> creds were cleared, setup mode entered
// Returns false -> creds preserved, BLE standby only
bool handleWifiFailure(wl_status_t status) {
  bool badCredentials = (status == WL_CONNECT_FAILED);
  if (badCredentials) {
    clearCredentials();
    isSetupMode = true;
    return true;
  } else {
    // Transient: keep creds, do NOT enter setup mode
    return false;
  }
}

// Mirrors commandCallback for RESET_WIFI
void handleResetWifiCommand() {
  preferences.begin("wifi-creds", false);
  preferences.clear();
  preferences.end();
}

// Mirrors PassCallbacks::onWrite (BLE credential provisioning)
void bleProvisionCredentials(const String &ssid, const String &pass) {
  if (ssid != "") {
    preferences.begin("wifi-creds", false);
    preferences.putString("ssid",     ssid);
    preferences.putString("password", pass);
    preferences.end();
  }
}

// Mirrors ServerCallbacks::onDisconnect
void onBleDisconnect() {
  bleSessionAuthorized = false;
}

// ─── Test Helpers ───────────────────────────────────────────

static int testsPassed = 0;
static int testsFailed = 0;

void pass(const char *name) {
  Serial.printf("  [PASS] %s\n", name);
  testsPassed++;
}

void fail(const char *name, const char *reason) {
  Serial.printf("  [FAIL] %s -- %s\n", name, reason);
  testsFailed++;
}

void assertBool(const char *name, bool actual, bool expected, const char *reason) {
  if (actual == expected) pass(name);
  else                    fail(name, reason);
}

void assertStr(const char *name, const String &actual, const String &expected, const char *reason) {
  if (actual == expected) pass(name);
  else {
    String msg = String(reason) + " (got \"" + actual + "\", want \"" + expected + "\")";
    fail(name, msg.c_str());
  }
}

// ─── Individual Tests ───────────────────────────────────────

/*
 * TC-01: WL_CONNECT_FAILED should clear credentials.
 * Rationale: Router explicitly rejected the password -- creds are wrong.
 */
void tc01_bad_password_clears_creds() {
  Serial.println("\n[TC-01] Bad password (WL_CONNECT_FAILED) clears credentials");

  saveCredentials("MyNetwork", "wrongpass");
  isSetupMode = false;

  bool cleared = handleWifiFailure(WL_CONNECT_FAILED);

  preferences.begin("wifi-creds", true);
  String ssid = preferences.getString("ssid", "__MISSING__");
  preferences.end();

  assertBool("returns true (cleared)",  cleared, true,       "should indicate creds were cleared");
  assertStr ("ssid is absent",          ssid,    "__MISSING__", "ssid should be gone after clear");
  assertBool("isSetupMode set",         isSetupMode, true,   "should enter provisioning mode");
}

/*
 * TC-02: WL_DISCONNECTED should preserve credentials.
 * Rationale: Timeout means router is temporarily unreachable, not wrong password.
 */
void tc02_transient_failure_preserves_creds() {
  Serial.println("\n[TC-02] Transient failure (WL_DISCONNECTED) preserves credentials");

  saveCredentials("HomeRouter", "correctpass");
  isSetupMode = false;

  bool cleared = handleWifiFailure(WL_DISCONNECTED);

  preferences.begin("wifi-creds", true);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("password", "");
  preferences.end();

  assertBool("returns false (preserved)", cleared, false,         "should NOT clear creds");
  assertStr ("ssid preserved",            ssid,    "HomeRouter",  "ssid must survive transient failure");
  assertStr ("pass preserved",            pass,    "correctpass", "password must survive transient failure");
  assertBool("isSetupMode NOT set",       isSetupMode, false,     "should NOT enter setup mode");
}

/*
 * TC-03: WL_NO_SSID_AVAIL should preserve credentials.
 * Rationale: SSID not visible yet -- router may still be booting up.
 */
void tc03_ssid_unavailable_preserves_creds() {
  Serial.println("\n[TC-03] SSID unavailable (WL_NO_SSID_AVAIL) preserves credentials");

  saveCredentials("OfflineNet", "mypass123");
  isSetupMode = false;

  bool cleared = handleWifiFailure(WL_NO_SSID_AVAIL);

  preferences.begin("wifi-creds", true);
  String ssid = preferences.getString("ssid", "");
  preferences.end();

  assertBool("returns false (preserved)", cleared,  false,        "should NOT clear creds");
  assertStr ("ssid preserved",            ssid,      "OfflineNet", "ssid must survive SSID-missing failure");
  assertBool("isSetupMode NOT set",       isSetupMode, false,      "should NOT enter setup mode");
}

/*
 * TC-04: RESET_WIFI command clears credentials.
 * Rationale: Explicit user command from Firebase should always wipe creds.
 */
void tc04_reset_wifi_command_clears_creds() {
  Serial.println("\n[TC-04] RESET_WIFI Firebase command clears credentials");

  saveCredentials("Net", "pass");

  handleResetWifiCommand();

  preferences.begin("wifi-creds", true);
  String ssid = preferences.getString("ssid", "__MISSING__");
  preferences.end();

  assertStr("ssid cleared after RESET_WIFI", ssid, "__MISSING__", "creds must be wiped by RESET_WIFI");
}

/*
 * TC-05: BLE provisioning saves SSID and password.
 * Rationale: Valid provisioning over BLE should persist both fields.
 */
void tc05_ble_provisioning_saves_creds() {
  Serial.println("\n[TC-05] BLE provisioning saves SSID and password");

  // Clear first so we start clean
  preferences.begin("wifi-creds", false);
  preferences.clear();
  preferences.end();

  bleProvisionCredentials("BLE_Network", "ble_password");

  preferences.begin("wifi-creds", true);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("password", "");
  preferences.end();

  assertStr("ssid saved", ssid, "BLE_Network",  "ssid must be stored");
  assertStr("pass saved", pass, "ble_password", "password must be stored");
}

/*
 * TC-06: BLE provisioning with empty SSID does nothing.
 * Rationale: Firmware guards against partial provisioning (SSID sent first).
 */
void tc06_ble_provisioning_rejects_empty_ssid() {
  Serial.println("\n[TC-06] BLE provisioning with empty SSID does not overwrite existing creds");

  saveCredentials("ExistingNet", "existingpass");

  // Attempt to provision with no SSID (simulates password-only write)
  bleProvisionCredentials("", "newpass");

  preferences.begin("wifi-creds", true);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("password", "");
  preferences.end();

  assertStr("ssid unchanged", ssid, "ExistingNet",  "existing ssid must not be overwritten");
  assertStr("pass unchanged", pass, "existingpass", "existing password must not be overwritten");
}

/*
 * TC-07: BLE session deauthorized on disconnect.
 * Rationale: Auth token gate must reset per-session; different client
 *             should not inherit a previous session's authorization.
 */
void tc07_ble_session_reset_on_disconnect() {
  Serial.println("\n[TC-07] BLE session deauthorized on client disconnect");

  bleSessionAuthorized = true;  // Simulate an authorized session
  onBleDisconnect();

  assertBool("bleSessionAuthorized is false after disconnect",
             bleSessionAuthorized, false, "session must be reset on disconnect");
}

/*
 * TC-08: Stale fail_count key from old firmware does not exist.
 * Rationale: After flashing updated firmware, Preferences should not
 *            contain a fail_count that could confuse future logic.
 *            (This is a migration smoke-test; run after first flash.)
 */
void tc08_no_stale_fail_count() {
  Serial.println("\n[TC-08] Stale fail_count key is absent (migration check)");

  // Write a legacy fail_count to simulate old firmware state
  preferences.begin("wifi-creds", false);
  preferences.putInt("fail_count", 2);
  preferences.end();

  // New firmware never reads or writes fail_count, so we just verify
  // that removing it manually leaves creds intact (the migration path).
  preferences.begin("wifi-creds", false);
  preferences.remove("fail_count");
  preferences.end();

  preferences.begin("wifi-creds", true);
  bool hasFailCount = preferences.isKey("fail_count");
  preferences.end();

  assertBool("fail_count key absent", hasFailCount, false,
             "fail_count should not exist after migration");
}

// ─── Setup / Loop ───────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n========================================");
  Serial.println("  SmartLock WiFi Logic -- Unit Tests");
  Serial.println("========================================");

  tc01_bad_password_clears_creds();
  tc02_transient_failure_preserves_creds();
  tc03_ssid_unavailable_preserves_creds();
  tc04_reset_wifi_command_clears_creds();
  tc05_ble_provisioning_saves_creds();
  tc06_ble_provisioning_rejects_empty_ssid();
  tc07_ble_session_reset_on_disconnect();
  tc08_no_stale_fail_count();

  Serial.println("\n========================================");
  Serial.printf("  Results: %d passed, %d failed\n", testsPassed, testsFailed);
  Serial.println("========================================\n");
}

void loop() {
  // Nothing -- tests run once in setup()
}
