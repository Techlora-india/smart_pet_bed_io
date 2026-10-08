#include <Arduino.h>
#include <Wire.h>
#include "HX711.h"
#include <Adafruit_TMP117.h>
#include <Adafruit_MLX90614.h>
#include "MAX30105.h"
#include "OtaService.h" // The SparkFun library uses this header for MAX30102


// ===================== OTA CONFIGURATION =====================
const int CURRENT_VERSION = 1;
const char* versionUrl =
    "https://raw.githubusercontent.com/Techlora-india/kitchen_inventory_management/main/kitchent_inventory_io/var.txt";
const char* firmwareUrl =
    "https://raw.githubusercontent.com/Techlora-india/kitchen_inventory_management/main/kitchent_inventory_io/.pio/build/seeed_xiao_esp32c3/firmware.bin";
const char* firebaseBootAckBaseUrl = nullptr;
const char* firebaseAuthToken = "";

// Populated at runtime after deviceId is loaded from NVM.
// Do NOT initialize with deviceId here — String would be captured empty.
OtaConfig otaConfig;

// --- Pin Definitions ---
#define I2C_SDA_DEFAULT  21
#define I2C_SCL_DEFAULT  22
#define I2C_SDA_TEMP     25
#define I2C_SCL_TEMP     32

#define MUX_RST_PIN     26
#define HEATER_PIN      19
#define HX711_SCK       16
#define HX711_DAT       4
#define LED_PIN         17

#define TCAADDR         0x70

// --- Global Objects ---
TwoWire I2C_SEC = TwoWire(1); // Hardware I2C bus 1 for TMP117
HX711 scale;
Adafruit_TMP117 tmp117;
Adafruit_MLX90614 mlx;
MAX30105 particleSensor;

// --- Helper Functions ---

// Select TCA9548A channel (0-7)
void tca_select(uint8_t channel) {
    if (channel > 7) return;
    Wire.beginTransmission(TCAADDR);
    Wire.write(1 << channel);
    Wire.endTransmission();
    delay(5); // Brief settling time
}

// Reset the TCA9548A hardware
void reset_mux() {
    Serial.println("[SYSTEM] Resetting TCA9548A Multiplexer...");
    digitalWrite(MUX_RST_PIN, LOW);
    delay(10);
    digitalWrite(MUX_RST_PIN, HIGH);
    delay(50);
}

// Track TMP117 presence so we don't call into it if init failed
bool tmp117_found = false;

// --- Modular Testing Functions ---

void test_led() {
    Serial.println("[TEST] Toggling Indicator LED...");
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
}

void test_heater() {
    // Brief pulse to test the gate driver without overheating the bed
    Serial.println("[TEST] Energizing Heater for 500ms...");
    digitalWrite(HEATER_PIN, HIGH);
    delay(500); 
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
    tmp117.getEvent(&temp);
    // The library returns ~ -273.15 or NaN if it fails to read
    Serial.printf("%.2f °C\n", temp.temperature);
}

void test_mux_sensors() {
    Serial.println("[TEST] Scanning MUX Channels 1 to 4 (MLX90614 & MAX30102):");
    
    // Testing channels 1, 2, 3, 4 based on your wiring
    for (uint8_t ch = 1; ch <= 4; ch++) {
        tca_select(ch);
        Serial.printf("  --- Channel %d ---\n", ch);

        // 1. Test MLX90614
        if (mlx.begin()) {
            float objTemp = mlx.readObjectTempC();
            float ambTemp = mlx.readAmbientTempC();
            Serial.printf("      MLX90614 -> Object: %.2f °C | Ambient: %.2f °C\n", objTemp, ambTemp);
        } else {
            Serial.println("      MLX90614 -> FAIL / NOT DETECTED");
        }

        // 2. Test MAX30102
        // We initialize it on the fly for the active MUX channel
        if (particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
            particleSensor.setup(); // Default setup
            long irValue = particleSensor.getIR();
            long redValue = particleSensor.getRed();
            Serial.printf("      MAX30102 -> IR: %ld | RED: %ld\n", irValue, redValue);
        } else {
            Serial.println("      MAX30102 -> FAIL / NOT DETECTED");
        }
    }
}

// --- Main Setup & Loop ---

void setup() {
    Serial.begin(115200);
    delay(2000); // Allow serial monitor to connect
    Serial.println("\n\n=== Smart Pet Bed Hardware Diagnostic ===");

    // Initialize GPIO
    pinMode(LED_PIN, OUTPUT);
    pinMode(HEATER_PIN, OUTPUT);
    pinMode(MUX_RST_PIN, OUTPUT);
    
    digitalWrite(LED_PIN, LOW);
    digitalWrite(HEATER_PIN, LOW);
    digitalWrite(MUX_RST_PIN, HIGH);

    // Initialize MUX hardware reset
    reset_mux();

    // Initialize Default I2C (MUX & MUX'd sensors)
    Wire.begin(I2C_SDA_DEFAULT, I2C_SCL_DEFAULT);
    Serial.printf("[INIT] Default I2C Started (SDA: %d, SCL: %d)\n", I2C_SDA_DEFAULT, I2C_SCL_DEFAULT);

    // Initialize Secondary I2C (TMP117)
    I2C_SEC.begin(I2C_SDA_TEMP, I2C_SCL_TEMP);
    Serial.printf("[INIT] Secondary I2C Started (SDA: %d, SCL: %d)\n", I2C_SDA_TEMP, I2C_SCL_TEMP);

    // Initialize HX711
    scale.begin(HX711_DAT, HX711_SCK);
    Serial.println("[INIT] HX711 Initialized");

    // Initialize TMP117 on Secondary I2C Bus
    if (!tmp117.begin(0x48, &I2C_SEC)) {
        tmp117_found = false;
        Serial.println("[ERROR] TMP117 not found on Secondary I2C!");
    } else {
        tmp117_found = true;
        Serial.println("[INIT] TMP117 Found.");
    }
    
    Serial.println("=========================================\n");
}

void loop() {
    test_led();
    test_heater();
    test_hx711();
    test_tmp117();
    test_mux_sensors();
    
    Serial.println("\n--- Cycle Complete. Waiting 5 seconds... ---\n");
    delay(5000);
}