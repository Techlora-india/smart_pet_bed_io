#include <Arduino.h>
#include <Wire.h>
#include "HX711.h"
#include <Adafruit_TMP117.h>
#include <Adafruit_MLX90614.h>
#include "MAX30105.h"
#include "OtaService.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <time.h>
#include "cloudbase_root_ca.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

// ===================== OTA CONFIGURATION =====================
const int   CURRENT_VERSION = 2;
const char* VERSION_URL  =
    "https://raw.githubusercontent.com/Techlora-india/smart_pet_bed_io/main/var.txt";
const char* FIRMWARE_URL =
    "https://raw.githubusercontent.com/Techlora-india/smart_pet_bed_io/main/.pio/build/esp32dev/firmware.bin";

// Set these through PlatformIO build_flags or a private include/secrets.h.
#ifndef CLOUDBASE_SYNC_URL
#define CLOUDBASE_SYNC_URL ""
#endif
#ifndef DEVICE_SYNC_API_KEY
#define DEVICE_SYNC_API_KEY ""
#endif
#ifndef CLOUDBASE_CA_CERT
#define CLOUDBASE_CA_CERT ""
#endif
#ifndef ENABLE_LEGACY_OTA
#define ENABLE_LEGACY_OTA 1
#endif

// Populated at runtime in setup()
OtaConfig otaConfig;

// ===================== PIN DEFINITIONS =====================
#define I2C_SDA_DEFAULT  21
#define I2C_SCL_DEFAULT  22
#define I2C_SDA_TEMP     25
#define I2C_SCL_TEMP     32

#define MUX_RST_PIN      26
#define HEATER_PIN       19
#define HX711_SCK        16
#define HX711_DAT        4
#define LED_PIN          17
#define BOOT_BTN_PIN     0

#define TCAADDR          0x70

// ===================== BUTTON / AP CONFIG =====================
#define LONG_PRESS_MS    10000
#define DEBOUNCE_MS      50
#define OTA_CHECK_MS     60000    // re-check OTA every 60 s
#define SYNC_INTERVAL_MS 3000
#define COMMAND_TIMEOUT_MS 15000
#define WIFI_RETRY_INTERVAL_MS 15000
#define WIFI_LOSS_AP_TIMEOUT_MS 60000
#define WIFI_AP_RECOVERY_GRACE_MS 60000
#define TMP117_RETRY_INTERVAL_MS 10000
#define MAX_PROVISION_BODY_BYTES 512
#define MAX_SAFE_TEMP_C 45.0f
#define TEMP_HYSTERESIS_C 0.3f

// ===================== GLOBAL OBJECTS =====================
TwoWire I2C_SEC(1);
HX711 scale;
Adafruit_TMP117 tmp117;
Adafruit_MLX90614 mlx;
MAX30105 particleSensor;

Preferences prefs;
WebServer   apServer(80);

bool   tmp117_found = false;
bool   apMode       = false;
String wifiSsid;
String wifiPass;
String wifiUserId;
String deviceId;
String apName;
String apReason = "setup_required";
bool apServerStarted = false;
bool powerState = false;
bool provisioningRestartPending = false;
float targetTemp = 28.0f;
String mode = "auto";
float currentTemp = NAN;
bool petPresent = false;
uint32_t lastSync = 0;
uint32_t lastSensorRead = 0;
uint32_t lastCloudSuccess = 0;
uint32_t lastValidSensorRead = 0;
uint32_t lastWifiRetry = 0;
uint32_t wifiDisconnectedAt = 0;
uint32_t wifiConnectedSince = 0;
uint32_t lastTmp117Retry = 0;
uint32_t provisioningRestartAt = 0;
bool systemTimeSyncStarted = false;
uint32_t lastTimeSyncLog = 0;

// Button state machine
uint32_t btnPressStart    = 0;
bool     btnWasPressed    = false;
bool     longPressHandled = false;

// OTA bookkeeping
uint32_t lastOtaCheck = 0;

// ===================== FORWARD DECLS =====================
void handleBootButton();
void startAPMode(const char* reason = "setup_required");
void handleRoot();
void handleSave();
void connectWiFi();
void sendBootAck();
void handleScan();
void handleProvision();
void syncCloud();
bool ensureSystemTime();
void readTelemetry();
void applyDesiredState(JsonObject desired);
void updateHeaterOutput();
String getDeviceId();
void waitWithButton(uint32_t ms);
bool devicePresent(TwoWire &bus, uint8_t address);
bool initializeTmp117();
void logI2cDevices(TwoWire &bus, const char* busName);

// ===================== I2C PRESENCE =====================
bool devicePresent(TwoWire &bus, uint8_t address) {
    bus.beginTransmission(address);
    return (bus.endTransmission() == 0);
}

// ===================== TCA9548A =====================
void tca_select(uint8_t channel) {
    if (channel > 7) return;
    Wire.beginTransmission(TCAADDR);
    Wire.write(1 << channel);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[MUX] Channel %u select FAILED\n", channel);
    }
    delay(5);
}

void reset_mux() {
    Serial.println("[SYSTEM] Resetting TCA9548A Multiplexer...");
    digitalWrite(MUX_RST_PIN, LOW);
    delay(10);
    digitalWrite(MUX_RST_PIN, HIGH);
    delay(50);
}

// ===================== TEST FUNCTIONS =====================
void test_led() {
    Serial.println("[TEST] Toggling Indicator LED...");
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
}

void test_heater() {
    Serial.println("[TEST] Energizing Heater for 500ms...");
    digitalWrite(HEATER_PIN, HIGH);
    waitWithButton(500);   // allow boot button polling during the pulse
    digitalWrite(HEATER_PIN, LOW);
    Serial.println("       Heater OFF.");
}

void test_hx711() {
    Serial.print("[TEST] HX711 Load Cell: ");
    if (scale.is_ready()) {
        long reading = scale.read();
        Serial.printf("Ready. Raw Value = %ld\n", reading);
    } else {
        Serial.println("NOT FOUND or NOT READY.");
    }
}

void test_tmp117() {
    Serial.print("[TEST] Secondary I2C (TMP117 Bed Temp): ");
    if (!tmp117_found) {
        Serial.println("NOT INITIALIZED");
        return;
    }
    sensors_event_t temp;
    if (tmp117.getEvent(&temp)) {
        if (isnan(temp.temperature) || temp.temperature < -100.0f) {
            Serial.println("READ ERROR");
        } else {
            Serial.printf("%.2f °C\n", temp.temperature);
        }
    } else {
        Serial.println("READ FAILED");
    }
}

void test_mux_sensors() {
    Serial.println("[TEST] Scanning MUX Channels 1 to 4 (MLX90614 & MAX30102):");

    for (uint8_t ch = 1; ch <= 4; ch++) {
        tca_select(ch);
        Serial.printf("  --- Channel %d ---\n", ch);

        // ---- MLX90614 ----
        if (devicePresent(Wire, 0x5A) || devicePresent(Wire, 0x5B)) {
            if (mlx.begin()) {
                float objTemp = mlx.readObjectTempC();
                float ambTemp = mlx.readAmbientTempC();

                // Reject the fake 328.23 °C returned when comm fails
                if (objTemp > 200.0f || objTemp < -100.0f) {
                    Serial.println("      MLX90614 -> COMM ERROR (bogus reading)");
                } else {
                    Serial.printf("      MLX90614 -> Object: %.2f °C | Ambient: %.2f °C\n",
                                  objTemp, ambTemp);
                }
            } else {
                Serial.println("      MLX90614 -> INIT FAIL");
            }
        } else {
            Serial.println("      MLX90614 -> NOT PRESENT on I2C");
        }

        // ---- MAX30102 ----
        if (particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
            particleSensor.setup();
            long irValue  = particleSensor.getIR();
            long redValue = particleSensor.getRed();
            Serial.printf("      MAX30102 -> IR: %ld | RED: %ld\n", irValue, redValue);
        } else {
            Serial.println("      MAX30102 -> FAIL / NOT DETECTED");
        }
    }
}

// ===================== WIFI =====================
void connectWiFi() {
    if (wifiSsid.isEmpty()) {
        Serial.println("[WIFI] No stored credentials — starting AP mode.");
        startAPMode();
        return;
    }

    Serial.println("[WIFI] Connecting to configured network...");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000 &&
           !apMode) {
        waitWithButton(300);
        Serial.print(".");
    }

    if (apMode) return;

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connected. IP: %s\n",
                      WiFi.localIP().toString().c_str());
        apMode = false;
        wifiConnectedSince = 0;
    } else {
        Serial.printf("\n[WIFI] Connection failed (status=%d). Starting setup AP.\n",
                      static_cast<int>(WiFi.status()));
        powerState = false;
        digitalWrite(HEATER_PIN, LOW);
        startAPMode("wifi_connection_failed");
    }
}

// ===================== AP MODE + WEB UI =====================
void handleRoot() {
    Serial.printf("[SETUP] GET / from %s; status=%s; provisioned=%s\n",
                  apServer.client().remoteIP().toString().c_str(),
                  apReason.c_str(),
                  (!wifiSsid.isEmpty() && !wifiUserId.isEmpty()) ? "yes" : "no");
    JsonDocument doc;
    doc["deviceId"] = deviceId;
    doc["macAddress"] = deviceId;
    doc["apName"] = apName;
    doc["status"] = apReason;
    doc["provisioned"] = !wifiSsid.isEmpty() && !wifiUserId.isEmpty();
    doc["sensorReady"] = tmp117_found;
    doc["identityReady"] = deviceId.length() == 12;
    doc["protocolVersion"] = 1;
    String body;
    serializeJson(doc, body);
    apServer.send(200, "application/json", body);
}

void handleSave() {
    handleProvision();
}

void startAPMode(const char* reason) {
    apMode = true;
    const String requestedReason = reason;
    apReason = requestedReason;
    Serial.printf("[AP] Starting provisioning AP (reason=%s).\n", apReason.c_str());
    wifiConnectedSince = 0;
    digitalWrite(HEATER_PIN, LOW);
    powerState = false;
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    const String suffix = deviceId.length() == 12
        ? deviceId.substring(6)
        : String("SETUP");
    apName = "karuna_luxe_" + suffix;
    if (!WiFi.softAP(apName.c_str())) {
        Serial.println("[AP] Failed to start the provisioning access point.");
        return;
    }
    Serial.printf("[AP] Started. Connect to '%s'. IP: ", apName.c_str());
    Serial.println(WiFi.softAPIP());

    if (!apServerStarted) {
        apServer.on("/", handleRoot);
        apServer.on("/scan", HTTP_GET, handleScan);
        apServer.on("/provision", HTTP_POST, handleProvision);
        apServer.on("/save", HTTP_POST, handleSave);
        apServer.begin();
        apServerStarted = true;
    }
}

void handleScan() {
    Serial.printf("[SETUP] GET /scan from %s.\n",
                  apServer.client().remoteIP().toString().c_str());
    const int count = WiFi.scanNetworks(false, true);
    if (count < 0) {
        Serial.printf("[SETUP] Wi-Fi scan failed (code=%d).\n", count);
        WiFi.scanDelete();
        apServer.send(503, "application/json",
                      "{\"error\":\"Wi-Fi scan failed; retry the scan\"}");
        return;
    }
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
        JsonObject network = networks.add<JsonObject>();
        network["ssid"] = WiFi.SSID(i);
        network["rssi"] = WiFi.RSSI(i);
        network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    String body;
    serializeJson(doc, body);
    Serial.printf("[SETUP] Wi-Fi scan complete; %d network(s) found.\n", count);
    apServer.send(200, "application/json", body);
}

void handleProvision() {
    Serial.printf("[PROVISION] POST /provision from %s.\n",
                  apServer.client().remoteIP().toString().c_str());
    if (!apServer.hasArg("plain")) {
        Serial.println("[PROVISION] Rejected: request body is missing.");
        apServer.send(400, "application/json",
                      "{\"error\":\"JSON request body required\"}");
        return;
    }
    if (apServer.arg("plain").length() > MAX_PROVISION_BODY_BYTES) {
        Serial.println("[PROVISION] Rejected: request body is too large.");
        apServer.send(413, "application/json",
                      "{\"error\":\"Request body is too large\"}");
        return;
    }

    JsonDocument request;
    if (deserializeJson(request, apServer.arg("plain"))) {
        Serial.println("[PROVISION] Rejected: invalid JSON.");
        apServer.send(400, "application/json",
                      "{\"error\":\"Invalid JSON\"}");
        return;
    }
    if (!request["ssid"].is<const char*>() ||
        !request["password"].is<const char*>() ||
        !request["userId"].is<const char*>()) {
        Serial.println("[PROVISION] Rejected: expected string credentials.");
        apServer.send(400, "application/json",
                      "{\"error\":\"ssid, password, and userId must be strings\"}");
        return;
    }
    if (deviceId.length() != 12) {
        Serial.println("[PROVISION] Rejected: device identity unavailable.");
        apServer.send(503, "application/json",
                      "{\"error\":\"Device identity unavailable; restart the bed and retry\"}");
        return;
    }

    const String newSsid = request["ssid"] | "";
    const String newPass = request["password"] | "";
    const String newUserId = request["userId"] | "";
    if (newSsid.isEmpty() || newSsid.length() > 32 ||
        newPass.length() > 63 || newUserId.isEmpty() ||
        newUserId.length() > 128) {
        Serial.println("[PROVISION] Rejected: credential length is invalid.");
        apServer.send(400, "application/json",
                      "{\"error\":\"Invalid ssid, password, or userId length\"}");
        return;
    }

    JsonDocument config;
    config["ssid"] = newSsid;
    config["password"] = newPass;
    config["userId"] = newUserId;
    String serializedConfig;
    serializeJson(config, serializedConfig);
    if (prefs.putString("network", serializedConfig) == 0) {
        Serial.println("[PROVISION] Failed to save credentials in NVS.");
        apServer.send(500, "application/json",
                      "{\"error\":\"Unable to save provisioning data\"}");
        return;
    }
    wifiSsid = newSsid;
    wifiPass = newPass;
    wifiUserId = newUserId;

    apServer.send(202, "application/json",
                  "{\"status\":\"saved\",\"restartInMs\":1500}");
    provisioningRestartAt = millis() + 1500;
    provisioningRestartPending = true;
    Serial.printf("[PROVISION] Credentials saved (SSID length=%u, account ID length=%u); restart scheduled.\n",
                  static_cast<unsigned>(newSsid.length()),
                  static_cast<unsigned>(newUserId.length()));
}

String getDeviceId() {
    uint8_t mac[6] = {};
    const esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    bool allZero = true;
    bool allOnes = true;
    for (uint8_t i = 0; i < sizeof(mac); ++i) {
        allZero = allZero && mac[i] == 0;
        allOnes = allOnes && mac[i] == 0xFF;
    }

    if (result != ESP_OK || allZero || allOnes) {
        Serial.printf("[DEVICE] Station MAC read failed (error %d); using eFuse fallback.\n",
                      static_cast<int>(result));
        const uint64_t efuseMac = ESP.getEfuseMac();
        for (uint8_t i = 0; i < sizeof(mac); ++i) {
            mac[sizeof(mac) - 1 - i] =
                static_cast<uint8_t>(efuseMac >> (i * 8));
        }
    }

    allZero = true;
    allOnes = true;
    for (uint8_t i = 0; i < sizeof(mac); ++i) {
        allZero = allZero && mac[i] == 0;
        allOnes = allOnes && mac[i] == 0xFF;
    }
    if (allZero || allOnes) {
        Serial.println("[DEVICE] No usable hardware MAC; device identity unavailable.");
        return "";
    }

    char formatted[13];
    snprintf(formatted, sizeof(formatted), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(formatted);
}

void logI2cDevices(TwoWire &bus, const char* busName) {
    Serial.printf("[I2C] %s devices:", busName);
    bool found = false;
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
        if (devicePresent(bus, address)) {
            Serial.printf(" 0x%02X", address);
            found = true;
        }
    }
    if (!found) Serial.print(" none");
    Serial.println();
}

bool initializeTmp117() {
    const uint8_t addresses[] = {0x48, 0x49, 0x4A, 0x4B};
    for (uint8_t address : addresses) {
        if (!devicePresent(I2C_SEC, address)) continue;
        Serial.printf("[INIT] TMP117 responded at 0x%02X on secondary I2C.\n",
                      address);
        if (!tmp117.begin(address, &I2C_SEC)) {
            Serial.printf("[WARN] TMP117 initialization failed at 0x%02X.\n",
                          address);
            continue;
        }

        sensors_event_t event;
        if (!tmp117.getEvent(&event) || !isfinite(event.temperature) ||
            event.temperature < -50.0f || event.temperature > 100.0f) {
            Serial.printf("[WARN] TMP117 at 0x%02X did not return a valid reading.\n",
                          address);
            continue;
        }

        Serial.printf("[INIT] TMP117 ready at 0x%02X (%.2f C).\n",
                      address, event.temperature);
        return true;
    }

    Serial.println("[ERROR] TMP117 not ready on secondary I2C (SDA 25, SCL 32). Heater remains fail-safe off.");
    return false;
}

void readTelemetry() {
    currentTemp = NAN;
    if (tmp117_found) {
        sensors_event_t tempEvent;
        if (tmp117.getEvent(&tempEvent) &&
            !isnan(tempEvent.temperature) &&
            isfinite(tempEvent.temperature) &&
            tempEvent.temperature >= -50.0f &&
            tempEvent.temperature < 100.0f) {
            currentTemp = tempEvent.temperature;
            lastValidSensorRead = millis();
        } else {
            tmp117_found = false;
            lastValidSensorRead = 0;
            Serial.println("[WARN] TMP117 read failed; heater disabled and sensor discovery will retry.");
        }
    }
    petPresent = false;
}

void applyDesiredState(JsonObject desired) {
    if (desired["powerState"].is<bool>()) {
        powerState = desired["powerState"].as<bool>();
    }
    if (desired["targetTemp"].is<float>() || desired["targetTemp"].is<int>()) {
        const float requestedTemp = constrain(
            desired["targetTemp"].as<float>(), 20.0f, 42.0f);
        if (fabsf(requestedTemp - targetTemp) >= 0.01f) {
            targetTemp = requestedTemp;
            if (prefs.putFloat("targetTemp", targetTemp) == 0) {
                Serial.println("[NVS] Failed to save target temperature.");
            }
        }
    }
    const String requestedMode = desired["mode"] | mode;
    if (requestedMode == "auto" || requestedMode == "heat" ||
        requestedMode == "cool") {
        if (requestedMode != mode) {
            mode = requestedMode;
            if (prefs.putString("mode", mode) == 0) {
                Serial.println("[NVS] Failed to save control mode.");
            }
        }
    }
    updateHeaterOutput();
}

void updateHeaterOutput() {
    bool safeToHeat = !isnan(currentTemp) &&
        lastValidSensorRead != 0 &&
        millis() - lastValidSensorRead <= 3000 &&
        currentTemp < MAX_SAFE_TEMP_C &&
        lastCloudSuccess != 0 &&
        millis() - lastCloudSuccess <= COMMAND_TIMEOUT_MS;
    bool shouldHeat = powerState && safeToHeat &&
        (mode == "heat" || mode == "auto") &&
        currentTemp < targetTemp - TEMP_HYSTERESIS_C;
    digitalWrite(HEATER_PIN, shouldHeat ? HIGH : LOW);
}

bool ensureSystemTime() {
    constexpr time_t MIN_VALID_TIME = 1700000000;
    if (time(nullptr) >= MIN_VALID_TIME) return true;

    if (!systemTimeSyncStarted) {
        configTime(0, 0, "pool.ntp.org", "time.google.com",
                   "time.cloudflare.com");
        systemTimeSyncStarted = true;
        Serial.println("[TIME] Waiting for NTP time before validating TLS.");
    }

    if (millis() - lastTimeSyncLog >= 15000) {
        Serial.println("[TIME] Clock is not synchronized; cloud sync is paused.");
        lastTimeSyncLog = millis();
    }
    return false;
}

void syncCloud() {
    static uint32_t lastUnavailableLog = 0;
    const bool missingEndpoint = strlen(CLOUDBASE_SYNC_URL) == 0;
    const bool missingApiKey = strlen(DEVICE_SYNC_API_KEY) == 0;
    const bool disconnected = WiFi.status() != WL_CONNECTED;
    const bool missingAccount = wifiUserId.isEmpty();
    if (missingEndpoint || missingApiKey || disconnected || missingAccount) {
        if (millis() - lastUnavailableLog >= 15000) {
            Serial.printf("[SYNC] Not ready: endpoint=%s apiKey=%s wifi=%d account=%s.\n",
                          missingEndpoint ? "missing" : "set",
                          missingApiKey ? "missing" : "set",
                          static_cast<int>(WiFi.status()),
                          missingAccount ? "missing" : "set");
            lastUnavailableLog = millis();
        }
        return;
    }

    if (!ensureSystemTime()) return;

    readTelemetry();
    WiFiClientSecure client;
    const char* caCertificate = strlen(CLOUDBASE_CA_CERT) > 0
        ? CLOUDBASE_CA_CERT
        : CLOUDBASE_DEFAULT_ROOT_CA;
    client.setCACert(caCertificate);

    HTTPClient http;
    http.setTimeout(4000);
    if (!http.begin(client, CLOUDBASE_SYNC_URL)) {
        Serial.println("[SYNC] Unable to open CloudBase endpoint.");
        return;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", DEVICE_SYNC_API_KEY);

    JsonDocument request;
    request["deviceId"] = deviceId;
    request["userId"] = wifiUserId;
    request["currentTemp"] = currentTemp;
    request["targetTemp"] = targetTemp;
    request["powerState"] = powerState;
    request["heaterOn"] = digitalRead(HEATER_PIN) == HIGH;
    request["mode"] = mode;
    request["petPresent"] = petPresent;
    request["firmwareVersion"] = CURRENT_VERSION;
    String payload;
    serializeJson(request, payload);

    const int status = http.POST(payload);
    if (status >= 200 && status < 300) {
        JsonDocument response;
        if (!deserializeJson(response, http.getString())) {
            const String responseDeviceId = response["deviceId"] | "";
            JsonObject desired = response["desired"];
            const bool validDesired =
                desired["powerState"].is<bool>() &&
                (desired["targetTemp"].is<float>() ||
                 desired["targetTemp"].is<int>()) &&
                (desired["mode"] == "auto" || desired["mode"] == "heat" ||
                 desired["mode"] == "cool");
            if (responseDeviceId == deviceId && validDesired) {
                applyDesiredState(desired);
                lastCloudSuccess = millis();
                Serial.printf("[SYNC] Cloud update OK; desired heater=%s target=%.1f C mode=%s.\n",
                              powerState ? "on" : "off",
                              targetTemp,
                              mode.c_str());
            } else {
                Serial.printf("[SYNC] Ignoring invalid command response (HTTP %d).\n",
                              status);
            }
        } else {
            Serial.printf("[SYNC] Invalid JSON response (HTTP %d).\n", status);
        }
    } else {
        Serial.printf("[SYNC] HTTP/TLS request failed (status=%d): %s\n",
                      status,
                      http.errorToString(status).c_str());
    }
    http.end();
}

// ===================== BOOT BUTTON =====================
void sendBootAck() {
    Serial.println("[BOOT] Short press — sending boot ACK");

    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["event"]     = "boot";
    doc["version"]   = CURRENT_VERSION;
    doc["uptime_ms"] = millis();

    send_ota_ack(otaConfig, doc);
}

void handleBootButton() {
    bool pressed = (digitalRead(BOOT_BTN_PIN) == LOW);

    if (pressed && !btnWasPressed) {
        delay(DEBOUNCE_MS);
        if (digitalRead(BOOT_BTN_PIN) == LOW) {
            btnPressStart    = millis();
            btnWasPressed    = true;
            longPressHandled = false;
        }
    }

    if (pressed && btnWasPressed && !longPressHandled) {
        if (millis() - btnPressStart >= LONG_PRESS_MS) {
            longPressHandled = true;
            Serial.println("[BOOT] Long press detected — clearing Wi-Fi credentials and entering AP mode");
            prefs.remove("network");
            prefs.remove("ssid");
            prefs.remove("pass");
            prefs.remove("userId");
            wifiSsid = "";
            wifiPass = "";
            wifiUserId = "";
            startAPMode("setup_required");
        }
    }

    if (!pressed && btnWasPressed) {
        uint32_t held = millis() - btnPressStart;
        btnWasPressed = false;

        if (!longPressHandled && held >= DEBOUNCE_MS && held < LONG_PRESS_MS) {
#if ENABLE_LEGACY_OTA
            sendBootAck();
            check_ota(otaConfig);
#endif
        }
    }
}

// ===================== NON-BLOCKING WAIT =====================
// Polls the boot button and serves the AP web UI during any delay.
void waitWithButton(uint32_t ms) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        handleBootButton();
        if (apMode) apServer.handleClient();
        delay(20);
    }
}

// ===================== SETUP =====================
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n\n=== Smart Pet Bed Controller ===");

    // ---- NVM ----
    prefs.begin("wifi", false);
    const String savedNetwork = prefs.getString("network", "");
    if (!savedNetwork.isEmpty()) {
        JsonDocument config;
        if (deserializeJson(config, savedNetwork) ||
            !(config["ssid"].is<const char*>() &&
              config["password"].is<const char*>() &&
              config["userId"].is<const char*>())) {
            Serial.println("[NVS] Saved Wi-Fi configuration is invalid.");
            prefs.remove("network");
        } else {
            wifiSsid = config["ssid"].as<String>();
            wifiPass = config["password"].as<String>();
            wifiUserId = config["userId"].as<String>();
        }
    } else {
        wifiSsid = prefs.getString("ssid", "");
        wifiPass = prefs.getString("pass", "");
        wifiUserId = prefs.getString("userId", "");
        if (!wifiSsid.isEmpty() && !wifiUserId.isEmpty()) {
            JsonDocument config;
            config["ssid"] = wifiSsid;
            config["password"] = wifiPass;
            config["userId"] = wifiUserId;
            String serializedConfig;
            serializeJson(config, serializedConfig);
            if (prefs.putString("network", serializedConfig) == 0) {
                Serial.println("[NVS] Failed to migrate Wi-Fi configuration.");
            }
        }
    }
    targetTemp = prefs.getFloat("targetTemp", 28.0f);
    mode = prefs.getString("mode", "auto");
    if (!isfinite(targetTemp) || targetTemp < 20.0f || targetTemp > 42.0f) {
        targetTemp = 28.0f;
        prefs.putFloat("targetTemp", targetTemp);
    }
    if (mode != "auto" && mode != "heat" && mode != "cool") {
        mode = "auto";
        prefs.putString("mode", mode);
    }
    WiFi.mode(WIFI_STA);
    deviceId = getDeviceId();
    if (!deviceId.isEmpty()) {
        Serial.printf("[DEVICE] Station MAC / device ID: %s\n", deviceId.c_str());
    }

    // ---- GPIO ----
    pinMode(LED_PIN,      OUTPUT);
    pinMode(HEATER_PIN,   OUTPUT);
    pinMode(MUX_RST_PIN,  OUTPUT);
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

    digitalWrite(LED_PIN,     LOW);
    digitalWrite(HEATER_PIN,  LOW);
    digitalWrite(MUX_RST_PIN, HIGH);

    reset_mux();

    // ---- I2C buses ----
    Wire.begin(I2C_SDA_DEFAULT, I2C_SCL_DEFAULT);
    Wire.setClock(100000);
    Serial.printf("[INIT] Default I2C   SDA:%d SCL:%d\n",
                  I2C_SDA_DEFAULT, I2C_SCL_DEFAULT);

    I2C_SEC.begin(I2C_SDA_TEMP, I2C_SCL_TEMP, 100000);
    Serial.printf("[INIT] Secondary I2C SDA:%d SCL:%d\n",
                  I2C_SDA_TEMP, I2C_SCL_TEMP);
    logI2cDevices(Wire, "Default bus");
    logI2cDevices(I2C_SEC, "Secondary bus");

    // ---- HX711 ----
    scale.begin(HX711_DAT, HX711_SCK);
    Serial.println("[INIT] HX711 Initialized");

    // ---- TMP117 ----
    tmp117_found = initializeTmp117();
    lastTmp117Retry = millis();

    // ---- Populate OtaConfig ----
    // Firebase URL intentionally set to nullptr — the library uses its
    // own default (kDefaultBootAckBaseUrl) in that case.
    otaConfig.currentVersion            = CURRENT_VERSION;
    otaConfig.versionUrl                = VERSION_URL;
    otaConfig.firmwareUrl               = FIRMWARE_URL;
    otaConfig.deviceId                  = deviceId.c_str();
    otaConfig.firebaseBootAckBaseUrl    = nullptr;
    otaConfig.firebaseAuthToken         = "";

    // ---- WiFi ----
    if (wifiSsid.isEmpty() || wifiUserId.isEmpty()) {
        startAPMode();
    } else {
        connectWiFi();
    }

    // ---- OTA check on boot (skipped if we're in AP mode) ----
    if (!apMode) {
#if ENABLE_LEGACY_OTA
        Serial.println("[OTA] Boot check...");
        check_ota(otaConfig);
#endif
    }
    lastOtaCheck = millis();

    Serial.println("=========================================\n");
}

// ===================== LOOP =====================
void loop() {
    // 1. Boot button handling (highest priority)
    handleBootButton();

    if (provisioningRestartPending &&
        static_cast<int32_t>(millis() - provisioningRestartAt) >= 0) {
        Serial.println("[PROVISION] Restarting to apply saved Wi-Fi settings.");
        ESP.restart();
    }

    // 2. If in AP mode, just serve the web UI and skip diagnostics.
    if (apMode) {
        if (!wifiSsid.isEmpty() && WiFi.status() == WL_CONNECTED) {
            if (wifiConnectedSince == 0) {
                wifiConnectedSince = millis();
                Serial.println("[WIFI] Saved network recovered; keeping setup AP available briefly.");
            }
            if (WiFi.softAPgetStationNum() == 0 &&
                millis() - wifiConnectedSince >= WIFI_AP_RECOVERY_GRACE_MS) {
                Serial.println("[WIFI] Network stable and no setup client is connected; closing recovery AP.");
                apServer.stop();
                apServerStarted = false;
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                apMode = false;
                apReason = "connected";
                lastCloudSuccess = 0;
                lastSync = 0;
                wifiDisconnectedAt = 0;
                wifiConnectedSince = 0;
            } else {
                if (WiFi.softAPgetStationNum() > 0) {
                    wifiConnectedSince = millis();
                }
                apServer.handleClient();
                waitWithButton(50);
                return;
            }
        } else {
            wifiConnectedSince = 0;
            if (!apServerStarted &&
                millis() - lastWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
                startAPMode(apReason.c_str());
                lastWifiRetry = millis();
            }
            if (!wifiSsid.isEmpty() &&
                millis() - lastWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
                Serial.println("[WIFI] Retrying saved home network while setup AP stays available.");
                WiFi.reconnect();
                lastWifiRetry = millis();
            }
            apServer.handleClient();
            waitWithButton(50);
            return;
        }
    }

    if (apMode) {
        apServer.handleClient();
        waitWithButton(50);
        return;
    }

    // 3. Periodic OTA check (non-blocking interval)
#if ENABLE_LEGACY_OTA
    if (millis() - lastOtaCheck >= OTA_CHECK_MS) {
        check_ota(otaConfig);
        lastOtaCheck = millis();
    }
#endif

    // 4. Hardware diagnostics
    if (millis() - lastSensorRead >= 1000) {
        readTelemetry();
        lastSensorRead = millis();
    }
    if (!tmp117_found &&
        millis() - lastTmp117Retry >= TMP117_RETRY_INTERVAL_MS) {
        lastTmp117Retry = millis();
        tmp117_found = initializeTmp117();
    }
    if (WiFi.status() != WL_CONNECTED ||
        (lastCloudSuccess != 0 &&
         millis() - lastCloudSuccess > COMMAND_TIMEOUT_MS)) {
        powerState = false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        if (wifiDisconnectedAt == 0) wifiDisconnectedAt = millis();
        if (millis() - lastWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
            WiFi.reconnect();
            lastWifiRetry = millis();
        }
        if (!wifiSsid.isEmpty() &&
            millis() - wifiDisconnectedAt >= WIFI_LOSS_AP_TIMEOUT_MS) {
            Serial.println("[WIFI] Network unavailable; starting recovery AP.");
            startAPMode("wifi_connection_lost");
        }
    } else {
        wifiDisconnectedAt = 0;
        lastWifiRetry = 0;
    }
    updateHeaterOutput();
    if (millis() - lastSync >= SYNC_INTERVAL_MS) {
        syncCloud();
        lastSync = millis();
    }
    waitWithButton(50);
}