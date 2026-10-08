#include <Arduino.h>
#include <Wire.h>
#include "HX711.h"
#include <Adafruit_TMP117.h>
#include <Adafruit_MLX90614.h>
#include "MAX30105.h"
#include "OtaService.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ===================== OTA CONFIGURATION =====================
const int   CURRENT_VERSION = 1;
const char* VERSION_URL  =
    "https://raw.githubusercontent.com/Techlora-india/smart_pet_bed_io/main/var.txt";
const char* FIRMWARE_URL =
    "https://raw.githubusercontent.com/Techlora-india/smart_pet_bed_io/main/.pio/build/esp32dev/firmware.bin";

// Hardcoded device ID (baked into firmware)
#define DEVICE_ID  "petbed-001"

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
#define LONG_PRESS_MS    5000
#define DEBOUNCE_MS      50
#define OTA_CHECK_MS     60000    // re-check OTA every 60 s

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

// Button state machine
uint32_t btnPressStart    = 0;
bool     btnWasPressed    = false;
bool     longPressHandled = false;

// OTA bookkeeping
uint32_t lastOtaCheck = 0;

// ===================== FORWARD DECLS =====================
void handleBootButton();
void startAPMode();
void handleRoot();
void handleSave();
void connectWiFi();
void sendBootAck();
void waitWithButton(uint32_t ms);
bool devicePresent(TwoWire &bus, uint8_t address);

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

    Serial.printf("[WIFI] Connecting to '%s'...\n", wifiSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        waitWithButton(300);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connected. IP: %s\n",
                      WiFi.localIP().toString().c_str());
        apMode = false;
    } else {
        Serial.println("\n[WIFI] Failed. Starting AP mode.");
        startAPMode();
    }
}

// ===================== AP MODE + WEB UI =====================
void handleRoot() {
    String html =
        "<!DOCTYPE html><html><head><meta name='viewport' "
        "content='width=device-width,initial-scale=1'>"
        "<title>Pet Bed Setup</title>"
        "<style>body{font-family:Arial;margin:20px;max-width:400px}"
        "input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box}"
        "button{width:100%;padding:10px;background:#2c7;color:#fff;"
        "border:none;cursor:pointer}</style></head><body>"
        "<h2>WiFi Setup</h2><form action='/save' method='POST'>"
        "SSID:<input name='ssid' required>"
        "Password:<input name='pass' type='password'>"
        "<button type='submit'>Save &amp; Reboot</button>"
        "</form></body></html>";
    apServer.send(200, "text/html", html);
}

void handleSave() {
    String newSsid = apServer.arg("ssid");
    String newPass = apServer.arg("pass");

    if (newSsid.length() > 0) {
        prefs.putString("ssid", newSsid);
        prefs.putString("pass", newPass);
        Serial.printf("[AP] Saved SSID='%s'\n", newSsid.c_str());
        apServer.send(200, "text/html", "<h3>Saved! Rebooting…</h3>");
        delay(1500);
        ESP.restart();
    } else {
        apServer.send(400, "text/plain", "SSID required");
    }
}

void startAPMode() {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("PetBed-Setup");   // passwordless
    Serial.print("[AP] Started. Connect to 'PetBed-Setup'. IP: ");
    Serial.println(WiFi.softAPIP());

    apServer.on("/",     handleRoot);
    apServer.on("/save", HTTP_POST, handleSave);
    apServer.begin();
}

// ===================== BOOT BUTTON =====================
void sendBootAck() {
    Serial.println("[BOOT] Short press — sending boot ACK");

    JsonDocument doc;
    doc["device_id"] = DEVICE_ID;
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
            Serial.println("[BOOT] Long press detected — entering AP mode");
            startAPMode();
        }
    }

    if (!pressed && btnWasPressed) {
        uint32_t held = millis() - btnPressStart;
        btnWasPressed = false;

        if (!longPressHandled && held >= DEBOUNCE_MS && held < LONG_PRESS_MS) {
            sendBootAck();
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
    Serial.println("\n\n=== Smart Pet Bed Hardware Diagnostic ===");

    // ---- NVM ----
    prefs.begin("wifi", false);
    wifiSsid = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");

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
    Serial.printf("[INIT] Default I2C   SDA:%d SCL:%d\n",
                  I2C_SDA_DEFAULT, I2C_SCL_DEFAULT);

    I2C_SEC.begin(I2C_SDA_TEMP, I2C_SCL_TEMP);
    Serial.printf("[INIT] Secondary I2C SDA:%d SCL:%d\n",
                  I2C_SDA_TEMP, I2C_SCL_TEMP);

    // ---- HX711 ----
    scale.begin(HX711_DAT, HX711_SCK);
    Serial.println("[INIT] HX711 Initialized");

    // ---- TMP117 ----
    tmp117_found = false;
    if (tmp117.begin(0x48, &I2C_SEC)) {
        sensors_event_t tmpEvent;
        if (tmp117.getEvent(&tmpEvent) &&
            !isnan(tmpEvent.temperature) &&
            tmpEvent.temperature > -100.0f) {
            tmp117_found = true;
            Serial.println("[INIT] TMP117 Found.");
        } else {
            Serial.println("[ERROR] TMP117 responded but read failed.");
        }
    } else {
        Serial.println("[ERROR] TMP117 not found on Secondary I2C!");
    }

    // ---- Populate OtaConfig ----
    // Firebase URL intentionally set to nullptr — the library uses its
    // own default (kDefaultBootAckBaseUrl) in that case.
    otaConfig.currentVersion            = CURRENT_VERSION;
    otaConfig.versionUrl                = VERSION_URL;
    otaConfig.firmwareUrl               = FIRMWARE_URL;
    otaConfig.deviceId                  = DEVICE_ID;
    otaConfig.firebaseBootAckBaseUrl    = nullptr;
    otaConfig.firebaseAuthToken         = "";

    // ---- WiFi ----
    connectWiFi();

    // ---- OTA check on boot (skipped if we're in AP mode) ----
    if (!apMode) {
        Serial.println("[OTA] Boot check...");
        check_ota(otaConfig);
    }
    lastOtaCheck = millis();

    Serial.println("=========================================\n");
}

// ===================== LOOP =====================
void loop() {
    // 1. Boot button handling (highest priority)
    handleBootButton();

    // 2. If in AP mode, just serve the web UI and skip diagnostics.
    if (apMode) {
        apServer.handleClient();
        waitWithButton(50);
        return;
    }

    // 3. Periodic OTA check (non-blocking interval)
    if (millis() - lastOtaCheck >= OTA_CHECK_MS) {
        check_ota(otaConfig);
        lastOtaCheck = millis();
    }

    // 4. Hardware diagnostics
    test_led();
    test_heater();
    test_hx711();
    test_tmp117();
    test_mux_sensors();

    Serial.println("\n--- Cycle Complete. Waiting 5 seconds... ---\n");
    waitWithButton(5000);   // button still polled during this wait
}