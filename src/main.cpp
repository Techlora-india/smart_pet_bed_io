#include <Wire.h>
#include <Adafruit_MLX90614.h>

// ========== PIN DEFINITIONS ==========
#define I2C_SDA  19
#define I2C_SCL  18
#define VCC_PIN  5        // Power supply for the sensor

#define MLX_ADDR     0x5A   // Default address
#define MLX_ADDR_ALT 0x5B   // If ADDR pin is pulled high

Adafruit_MLX90614 mlx;

// ========== SCAN BUS ==========
void scanBus(TwoWire &bus) {
    Serial.println("Scanning I2C bus...");
    bool found = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        bus.beginTransmission(addr);
        if (bus.endTransmission() == 0) {
            Serial.printf("  Found device at 0x%02X\n", addr);
            found = true;
        }
    }
    if (!found) Serial.println("  No I2C devices found.");
}

// ========== SETUP ==========
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n========== MLX90614 SINGLE SENSOR TEST ==========");

    // ---- Power on sensor ----
    pinMode(VCC_PIN, OUTPUT);
    digitalWrite(VCC_PIN, HIGH);
    Serial.println("Sensor powered on (VCC = HIGH).");
    delay(200);  // allow power to stabilise

    // ---- I2C initialisation ----
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);   // 100 kHz

    // ---- (Optional) Enable internal pull-ups for a quick test ----
    // This is NOT a proper fix – external resistors are strongly recommended.
    // gpio_pullup_en(GPIO_NUM_13);
    // gpio_pullup_en(GPIO_NUM_12);

    // ---- Scan bus ----
    scanBus(Wire);

    // ---- MLX90614 initialisation ----
    Serial.println("\n--- MLX90614 ---");
    bool mlxOK = false;
    if (mlx.begin(MLX_ADDR, &Wire)) {
        mlxOK = true;
        Serial.println("MLX found at 0x5A.");
    } else if (mlx.begin(MLX_ADDR_ALT, &Wire)) {
        mlxOK = true;
        Serial.println("MLX found at 0x5B (alternate).");
    }

    if (mlxOK) {
        Serial.printf("  Ambient: %.2f °C\n", mlx.readAmbientTempC());
        Serial.printf("  Object:  %.2f °C\n", mlx.readObjectTempC());
    } else {
        Serial.println("MLX not found – check wiring, power, and pull‑up resistors.");
        Serial.println("If you have no external pull‑ups, enable the internal ones (see code comments).");
    }

    Serial.println("\nLoop will read every second...");
}

// ========== LOOP ==========
void loop() {
    // Try to re‑initialise each loop (in case of glitches)
    if (mlx.begin(MLX_ADDR, &Wire) || mlx.begin(MLX_ADDR_ALT, &Wire)) {
        Serial.printf("MLX: Obj=%.2f°C, Amb=%.2f°C\n",
                      mlx.readObjectTempC(), mlx.readAmbientTempC());
    } else {
        Serial.println("MLX: not responding");
    }

    delay(1000);
}