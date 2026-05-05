/*
 * TerraFirm — ESP32 Greenhouse Monitoring & Control v4.4
 * Blynk (real-time) + ThingSpeak (logging) + EMA smoothing + watchdog
 * Relay logic: ACTIVE-LOW — LOW = ON, HIGH = OFF
 */

// ── Credentials ──────────────────────────────────────────────
#define BLYNK_TEMPLATE_ID        "TMPL3yv2jXZIj"
#define BLYNK_TEMPLATE_NAME      "Garden Controller"
#define BLYNK_AUTH_TOKEN         "xKgE6hKHN4xmnUR2y4QL6VnzaasNFxf1"
#define THINGSPEAK_CHANNEL_ID    3332580
#define THINGSPEAK_WRITE_API_KEY "EIMJ20CSU6M4T8LV"

// ── Libraries ────────────────────────────────────────────────
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <esp_task_wdt.h>
#include "ThingSpeak.h"

// ── WiFi ─────────────────────────────────────────────────────
#define WIFI_SSID       "Redmi Note 10 Pro_"
#define WIFI_PASSWORD   "abhiabhi"

// ── Pins ─────────────────────────────────────────────────────
#define DHT_PIN    4    // DHT11 data (10kΩ pull-up to 3.3V required)
#define SOIL_PIN   32   // Soil moisture ADC (ADC1_CH4)
#define FAN_PIN    26   // Fan relay  — active-LOW
#define PUMP_PIN   27   // Pump relay — active-LOW
#define LED_PIN    25   // Status LED (mirrors fan)
#define I2C_SDA    21
#define I2C_SCL    22
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ── Sensor config ─────────────────────────────────────────────
#define DHT_TYPE            DHT11
#define SOIL_ADC_DRY        4095   // Calibrate: sensor in open air
#define SOIL_ADC_WET        1000   // Calibrate: sensor submerged in water
#define SOIL_MEDIAN_SAMPLES 5
#define TEMP_EMA_ALPHA      0.3f   // EMA: 30% new, 70% history
#define HUM_EMA_ALPHA       0.3f
#define SOIL_EMA_ALPHA      0.3f
#define DHT_MAX_FAILURES    5      // Consecutive failures before sensor marked dead

// ── Control thresholds ────────────────────────────────────────
#define PUMP_ON_MOISTURE  30    // Pump ON  when soil < 30%
#define PUMP_OFF_MOISTURE 60    // Pump OFF when soil > 60%
#define FAN_ON_TEMP       30.0f // Fan ON  when temp > 30°C
#define FAN_OFF_TEMP      28.0f // Fan OFF when temp < 28°C  (2°C hysteresis)

// ── Timing (ms) ───────────────────────────────────────────────
#define STARTUP_HOLD_MS        10000UL  // Wait before first control decision
#define PUMP_MAX_ON_MS         30000UL  // Force pump OFF after 30s (flood guard)
#define PUMP_COOLDOWN_MS       60000UL  // Min gap between pump cycles (motor guard)
#define SENSOR_INTERVAL_MS     2000UL
#define LCD_INTERVAL_MS        2000UL
#define SERIAL_INTERVAL_MS     2000UL
#define BLYNK_SEND_INTERVAL_MS 30000UL
#define WIFI_CHECK_INTERVAL_MS 10000UL
#define THINGSPEAK_INTERVAL_MS 30000UL  // Matches 15 samples × 2s buffer exactly
#define WDT_TIMEOUT_S          10       // Must exceed longest blocking operation
#define LCD_COLS               16
#define LCD_ROWS               2

// ── Circular buffer ───────────────────────────────────────────
#define LOG_BUFFER_SIZE 15  // 15 × 2s = 30s of history

// ── Sensor globals ────────────────────────────────────────────
float g_temperature  = 0.0f;
float g_humidity     = 0.0f;
int   g_soilPercent  = 0;
int   g_soilRaw      = 0;
bool  g_tempValid    = false;
bool  g_humValid     = false;
bool  g_soilValid    = false;
int   g_dhtFailCount = 0;

// ── Control state ─────────────────────────────────────────────
bool          g_pumpOn          = false;
bool          g_fanOn           = false;
bool          g_ledOn           = false;
bool          g_pumpCoolingDown = false;
bool          g_startupHoldDone = false;
unsigned long g_pumpStartMs     = 0;
unsigned long g_pumpStopMs      = 0;

// ── Network state ─────────────────────────────────────────────
bool          g_wifiConnected   = false;
bool          g_blynkConnected  = false;
bool          g_thingspeakOK    = false;
bool          g_thingspeakReady = false;
unsigned long g_wifiBackoffMs   = 10000UL; // Doubles on each failure, cap 60s

// ── Timing trackers ───────────────────────────────────────────
unsigned long g_lastSensorMs     = 0;
unsigned long g_lastLCDMs        = 0;
unsigned long g_lastSerialMs     = 0;
unsigned long g_lastBlynkSendMs  = 0;
unsigned long g_lastWifiCheckMs  = 0;
unsigned long g_lastThingSpeakMs = 0;

// ── Data buffer ───────────────────────────────────────────────
struct DataPoint {
    unsigned long timestamp;
    float temp;
    float hum;
    int   soil;
    bool  pump;
    bool  fan;
};
DataPoint g_logBuffer[LOG_BUFFER_SIZE];
int       g_logHead  = 0;
int       g_logCount = 0;

// ── Objects ───────────────────────────────────────────────────
DHT                dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C* lcd    = nullptr;
bool               g_lcdOK = false;
WiFiClient         g_wifiClient;
BlynkTimer         blynkTimer;

// ─────────────────────────────────────────────────────────────
// I2C scanner — returns first found address, 0 if none
// ─────────────────────────────────────────────────────────────
byte scanI2C() {
    Serial.println("[I2C] Scanning...");
    byte found = 0;
    for (byte addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C] Device at 0x%02X\n", addr);
            if (!found) found = addr;
        }
    }
    if (!found) Serial.println("[I2C] No devices found.");
    return found;
}

// ─────────────────────────────────────────────────────────────
// Median filter — removes single-sample ADC spikes (pump EMI)
// Run this BEFORE EMA: median kills outliers, EMA smooths trends
// ─────────────────────────────────────────────────────────────
int medianADC(uint8_t pin, int n) {
    int buf[SOIL_MEDIAN_SAMPLES];
    n = constrain(n, 1, SOIL_MEDIAN_SAMPLES);
    for (int i = 0; i < n; i++) {
        buf[i] = analogRead(pin);
        delayMicroseconds(200);
    }
    for (int i = 1; i < n; i++) {
        int key = buf[i], j = i - 1;
        while (j >= 0 && buf[j] > key) { buf[j+1] = buf[j]; j--; }
        buf[j+1] = key;
    }
    return buf[n / 2];
}

// ─────────────────────────────────────────────────────────────
// LCD writer — pads with spaces to prevent character ghosting
// ─────────────────────────────────────────────────────────────
void lcdWriteRow(int row, const char* str) {
    if (!g_lcdOK || !lcd) return;
    lcd->setCursor(0, row);
    for (int i = 0; i < LCD_COLS; i++)
        lcd->write(str[i] ? str[i] : ' ');
}

// ─────────────────────────────────────────────────────────────
// Store current snapshot into circular buffer (overwrites oldest)
// ─────────────────────────────────────────────────────────────
void bufferDataPoint() {
    g_logBuffer[g_logHead] = { millis(), g_temperature, g_humidity, g_soilPercent, g_pumpOn, g_fanOn };
    g_logHead = (g_logHead + 1) % LOG_BUFFER_SIZE;
    if (g_logCount < LOG_BUFFER_SIZE) g_logCount++;
}

// ─────────────────────────────────────────────────────────────
// Compute 30s averages + duty cycles from circular buffer
// pumpDuty/fanDuty = % of samples where device was ON
// ─────────────────────────────────────────────────────────────
void calculateBufferStats(float* avgTemp, float* avgHum, int* avgSoil,
                          int* pumpDuty, int* fanDuty, int* sampleCount) {
    if (g_logCount == 0) {
        *avgTemp    = g_temperature;
        *avgHum     = g_humidity;
        *avgSoil    = g_soilPercent;
        *pumpDuty   = g_pumpOn ? 100 : 0;
        *fanDuty    = g_fanOn  ? 100 : 0;
        *sampleCount = 1;
        return;
    }
    float sumTemp = 0, sumHum = 0;
    int sumSoil = 0, pumpOnCount = 0, fanOnCount = 0;
    for (int i = 0; i < g_logCount; i++) {
        int idx = (g_logHead - g_logCount + i + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
        sumTemp      += g_logBuffer[idx].temp;
        sumHum       += g_logBuffer[idx].hum;
        sumSoil      += g_logBuffer[idx].soil;
        if (g_logBuffer[idx].pump) pumpOnCount++;
        if (g_logBuffer[idx].fan)  fanOnCount++;
    }
    *avgTemp     = sumTemp / g_logCount;
    *avgHum      = sumHum  / g_logCount;
    *avgSoil     = sumSoil / g_logCount;
    *pumpDuty    = (pumpOnCount * 100) / g_logCount;
    *fanDuty     = (fanOnCount  * 100) / g_logCount;
    *sampleCount = g_logCount;
}

// ─────────────────────────────────────────────────────────────
// WiFi manager — exponential backoff: 10s → 20s → 40s → 60s cap
// Non-blocking: never stalls loop(), never triggers watchdog
// ─────────────────────────────────────────────────────────────
void checkWiFi() {
    unsigned long now = millis();
    if (now - g_lastWifiCheckMs < g_wifiBackoffMs) return;
    g_lastWifiCheckMs = now;
    if (WiFi.status() != WL_CONNECTED) {
        g_wifiConnected   = false;
        g_blynkConnected  = false;
        g_thingspeakReady = false;
        Serial.printf("[WiFi] Disconnected — retry in %lus\n", g_wifiBackoffMs / 1000);
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        g_wifiBackoffMs = min(g_wifiBackoffMs * 2, 60000UL);
    } else {
        if (!g_wifiConnected) {
            g_wifiConnected = true;
            g_wifiBackoffMs = 10000;
            Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Blynk upload — live values every 30s
// V0=temp  V1=hum  V3=pump  V4=fan  V5=soil
// ─────────────────────────────────────────────────────────────
void sendToBlynk() {
    if (!g_blynkConnected || !Blynk.connected()) { g_blynkConnected = false; return; }
    unsigned long now = millis();
    if (now - g_lastBlynkSendMs < BLYNK_SEND_INTERVAL_MS) return;
    g_lastBlynkSendMs = now;
    if (g_tempValid) {
        Blynk.virtualWrite(V0, g_temperature);
        Blynk.virtualWrite(V1, g_humidity);
    }
    Blynk.virtualWrite(V5, g_soilPercent);
    Blynk.virtualWrite(V3, g_pumpOn ? 1 : 0);
    Blynk.virtualWrite(V4, g_fanOn  ? 1 : 0);
    Serial.println("[BLYNK] Sent");
}

// ─────────────────────────────────────────────────────────────
// ThingSpeak upload — 30s averaged buffer data
// Field 1=avgTemp  2=avgHum  3=avgSoil  4=pumpDuty  5=fanDuty  6=sampleCount
// ─────────────────────────────────────────────────────────────
void sendToThingSpeak() {
    static bool firstSend = true;
    if (!g_wifiConnected) { g_thingspeakOK = false; return; }
    if (!g_thingspeakReady) {
        ThingSpeak.begin(g_wifiClient);
        g_thingspeakReady    = true;
        g_lastThingSpeakMs   = millis();
        Serial.println("[ThingSpeak] Initialized");
    }
    unsigned long now = millis();
    if (!firstSend && (now - g_lastThingSpeakMs < THINGSPEAK_INTERVAL_MS)) return;
    g_lastThingSpeakMs = now;
    firstSend = false;

    float avgTemp, avgHum;
    int avgSoil, pumpDuty, fanDuty, sampleCount;
    calculateBufferStats(&avgTemp, &avgHum, &avgSoil, &pumpDuty, &fanDuty, &sampleCount);

    ThingSpeak.setField(1, avgTemp);
    ThingSpeak.setField(2, avgHum);
    ThingSpeak.setField(3, avgSoil);
    ThingSpeak.setField(4, pumpDuty);
    ThingSpeak.setField(5, fanDuty);
    ThingSpeak.setField(6, sampleCount);

    char status[32];
    snprintf(status, sizeof(status), "Avg%d V:%c%c%c", sampleCount,
             g_tempValid ? 'T' : '-', g_humValid ? 'H' : '-', g_soilValid ? 'S' : '-');
    ThingSpeak.setStatus(status);

    Serial.printf("[ThingSpeak] Avg %d samples: T=%.1f H=%.1f S=%d%% P=%d%% F=%d%%\n",
                  sampleCount, avgTemp, avgHum, avgSoil, pumpDuty, fanDuty);

    int response = ThingSpeak.writeFields(THINGSPEAK_CHANNEL_ID, THINGSPEAK_WRITE_API_KEY);
    g_thingspeakOK = (response == 200);
    if (g_thingspeakOK) {
        Serial.println("[ThingSpeak] OK");
        g_logCount = 0;  // Clear buffer after successful upload
        g_logHead  = 0;
    } else {
        Serial.printf("[ThingSpeak] FAILED: HTTP %d\n", response);
    }
}

// ─────────────────────────────────────────────────────────────
// Blynk callbacks — sync device states to app on (re)connect
// ─────────────────────────────────────────────────────────────
BLYNK_CONNECTED() {
    g_blynkConnected = true;
    Serial.println("[BLYNK] Connected");
    Blynk.virtualWrite(V3, g_pumpOn ? 1 : 0);
    Blynk.virtualWrite(V4, g_fanOn  ? 1 : 0);
}
BLYNK_DISCONNECTED() {
    g_blynkConnected = false;
    Serial.println("[BLYNK] Disconnected");
}

// ─────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== TerraFirm v4.4 ===");

    // Relays OFF first — CRITICAL: active-LOW relays fire on boot if not set
    pinMode(FAN_PIN,  OUTPUT); digitalWrite(FAN_PIN,  RELAY_OFF);
    pinMode(PUMP_PIN, OUTPUT); digitalWrite(PUMP_PIN, RELAY_OFF);
    pinMode(LED_PIN,  OUTPUT); digitalWrite(LED_PIN,  LOW);
    Serial.println("[GPIO] Relays OFF");

    dht.begin();
    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin();
    delay(200);

    byte addr = scanI2C();
    if (addr) {
        lcd = new LiquidCrystal_I2C(addr, LCD_COLS, LCD_ROWS);
        lcd->init();
        if (Wire.endTransmission(addr) == 0) {
            g_lcdOK = true;
            lcd->backlight();
            lcdWriteRow(0, "TerraFirm v4.4  ");
            lcdWriteRow(1, "Connecting...   ");
            Serial.printf("[LCD] Ready at 0x%02X\n", addr);
        } else {
            delete lcd; lcd = nullptr;
        }
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
        delay(200); Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        g_wifiConnected = true;
        Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
        if (g_lcdOK) lcdWriteRow(1, "WiFi OK         ");
        Blynk.config(BLYNK_AUTH_TOKEN);
        Blynk.connect(3000);
        ThingSpeak.begin(g_wifiClient);
        g_thingspeakReady = true;
        Serial.println("[ThingSpeak] Ready");
    } else {
        Serial.println("[WiFi] Offline mode");
        if (g_lcdOK) lcdWriteRow(1, "Offline mode    ");
    }

    // Watchdog — 10s timeout, triggers panic reset on hang
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms    = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = (1 << 0),
        .trigger_panic  = true
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(nullptr);
    Serial.printf("[WDT] Armed: %ds\n", WDT_TIMEOUT_S);

    Serial.printf("[BOOT] Hold: %lus | Buffer: %d samples\n", STARTUP_HOLD_MS / 1000, LOG_BUFFER_SIZE);
    delay(1000);
    if (g_lcdOK) lcd->clear();
    Serial.println("=== Ready ===\n");
}

// ─────────────────────────────────────────────────────────────
// readSensors() — DHT11 + soil ADC with EMA smoothing
// Pipeline: raw ADC → median(5) → EMA → g_soilPercent
// ─────────────────────────────────────────────────────────────
void readSensors() {
    float rawT = dht.readTemperature();
    float rawH = dht.readHumidity();

    if (isnan(rawT) || isnan(rawH) || rawT < -40.0f || rawT > 80.0f) {
        if (++g_dhtFailCount >= DHT_MAX_FAILURES) {
            g_tempValid = false;
            g_humValid  = false;
        }
    } else {
        g_dhtFailCount = 0;
        if (!g_tempValid) { g_temperature = rawT; g_tempValid = true; }
        else g_temperature = TEMP_EMA_ALPHA * rawT + (1.0f - TEMP_EMA_ALPHA) * g_temperature;
        if (!g_humValid)  { g_humidity = rawH; g_humValid = true; }
        else g_humidity = HUM_EMA_ALPHA * rawH + (1.0f - HUM_EMA_ALPHA) * g_humidity;
    }

    int rawADC = medianADC(SOIL_PIN, SOIL_MEDIAN_SAMPLES);
    int rawPercent;
    if      (rawADC >= SOIL_ADC_DRY) rawPercent = 0;
    else if (rawADC <= SOIL_ADC_WET) rawPercent = 100;
    else rawPercent = map(rawADC, SOIL_ADC_DRY, SOIL_ADC_WET, 0, 100);
    g_soilRaw = rawADC;

    if (!g_soilValid) { g_soilPercent = rawPercent; g_soilValid = true; }
    else g_soilPercent = (int)(SOIL_EMA_ALPHA * rawPercent + (1.0f - SOIL_EMA_ALPHA) * g_soilPercent);
}

// ─────────────────────────────────────────────────────────────
// controlPump() — hysteresis + 30s max runtime + 60s cooldown
// ─────────────────────────────────────────────────────────────
void controlPump() {
    unsigned long now = millis();
    if (g_pumpOn) {
        bool soilWet  = (g_soilPercent >= PUMP_OFF_MOISTURE);
        bool timedOut = (now - g_pumpStartMs >= PUMP_MAX_ON_MS);
        if (soilWet || timedOut) {
            digitalWrite(PUMP_PIN, RELAY_OFF);
            g_pumpOn = false; g_pumpCoolingDown = true; g_pumpStopMs = now;
            Serial.printf("[PUMP] OFF — %s\n", timedOut ? "TIMEOUT" : "wet");
            if (g_blynkConnected) Blynk.virtualWrite(V3, 0);
        }
    } else {
        if (g_pumpCoolingDown && (now - g_pumpStopMs >= PUMP_COOLDOWN_MS)) {
            g_pumpCoolingDown = false;
            Serial.println("[PUMP] Cooldown done");
        }
        if (g_soilPercent <= PUMP_ON_MOISTURE && !g_pumpCoolingDown) {
            digitalWrite(PUMP_PIN, RELAY_ON);
            g_pumpOn = true; g_pumpStartMs = now;
            Serial.printf("[PUMP] ON — %d%%\n", g_soilPercent);
            if (g_blynkConnected) Blynk.virtualWrite(V3, 1);
        }
    }
}

// ─────────────────────────────────────────────────────────────
// controlFan() — 2°C hysteresis thermostat, LED mirrors fan
// ─────────────────────────────────────────────────────────────
void controlFan() {
    if (!g_tempValid) return;
    if (g_fanOn && g_temperature <= FAN_OFF_TEMP) {
        digitalWrite(FAN_PIN, RELAY_OFF); digitalWrite(LED_PIN, LOW);
        g_fanOn = false; g_ledOn = false;
        Serial.printf("[FAN] OFF — %.1fC\n", g_temperature);
        if (g_blynkConnected) Blynk.virtualWrite(V4, 0);
    } else if (!g_fanOn && g_temperature >= FAN_ON_TEMP) {
        digitalWrite(FAN_PIN, RELAY_ON); digitalWrite(LED_PIN, HIGH);
        g_fanOn = true; g_ledOn = true;
        Serial.printf("[FAN] ON — %.1fC\n", g_temperature);
        if (g_blynkConnected) Blynk.virtualWrite(V4, 1);
    }
}

// ─────────────────────────────────────────────────────────────
// updateLCD()
// Row 0: S:xx%  T:xxC
// Row 1: H:xx%  P:ON/OFF  F:ON/OFF  [C/B/T/-]
//        cloud indicator: C=both  B=Blynk  T=ThingSpeak  -=offline
// ─────────────────────────────────────────────────────────────
void updateLCD() {
    if (!g_lcdOK || !lcd) return;
    unsigned long now = millis();
    if (now - g_lastLCDMs < LCD_INTERVAL_MS) return;
    g_lastLCDMs = now;

    char row0[LCD_COLS + 1], row1[LCD_COLS + 1];

    if (g_tempValid) snprintf(row0, sizeof(row0), "S:%3d%% T:%3dC   ", g_soilPercent, (int)g_temperature);
    else             snprintf(row0, sizeof(row0), "S:%3d%% T:---C   ", g_soilPercent);

    if (!g_startupHoldDone) {
        unsigned long rem = (STARTUP_HOLD_MS - millis()) / 1000UL + 1;
        snprintf(row1, sizeof(row1), "Hold: %2lus        ", rem);
    } else {
        const char* p = g_pumpOn ? "ON " : "OFF";
        const char* f = g_fanOn  ? "ON " : "OFF";
        char cloud = (g_blynkConnected && g_thingspeakOK) ? 'C' :
                     (g_blynkConnected  ? 'B' : (g_thingspeakOK ? 'T' : '-'));
        snprintf(row1, sizeof(row1), "H:%3d%% P:%s F:%s%c", constrain((int)g_humidity, 0, 999), p, f, cloud);
    }
    lcdWriteRow(0, row0);
    lcdWriteRow(1, row1);
}

// ─────────────────────────────────────────────────────────────
// printStatus() — single-line serial debug output every 2s
// ─────────────────────────────────────────────────────────────
void printStatus() {
    unsigned long now = millis();
    if (now - g_lastSerialMs < SERIAL_INTERVAL_MS) return;
    g_lastSerialMs = now;

    if (!g_startupHoldDone) {
        unsigned long rem = (STARTUP_HOLD_MS - now) / 1000UL + 1;
        Serial.printf("[HOLD] %lus — Soil:%d%% T:%s\n", rem, g_soilPercent,
                      g_tempValid ? String(g_temperature, 1).c_str() : "N/A");
        return;
    }
    Serial.printf("T:%s H:%s S:%d%%(R:%d) P:%s F:%s | V:%c%c%c | WiFi:%s(%lus) | Blynk:%s | TS:%s | Buf:%d/15\n",
                  g_tempValid ? String(g_temperature, 1).c_str() : "N/A",
                  g_humValid  ? String(g_humidity,    1).c_str() : "N/A",
                  g_soilPercent, g_soilRaw,
                  g_pumpOn ? "ON" : "OFF", g_fanOn ? "ON" : "OFF",
                  g_tempValid ? 'T' : '-', g_humValid ? 'H' : '-', g_soilValid ? 'S' : '-',
                  g_wifiConnected  ? "OK" : "NO", g_wifiBackoffMs / 1000,
                  g_blynkConnected ? "OK" : "NO",
                  g_thingspeakOK   ? "OK" : "NO",
                  g_logCount);
}

// ─────────────────────────────────────────────────────────────
// loop() — non-blocking, all operations time-sliced
// ─────────────────────────────────────────────────────────────
void loop() {
    esp_task_wdt_reset();  // Feed watchdog — must run every <10s

    unsigned long now = millis();

    if (g_wifiConnected) {
        Blynk.run();
        if (Blynk.connected()) g_blynkConnected = true;
    }

    if (!g_startupHoldDone && now >= STARTUP_HOLD_MS) {
        g_startupHoldDone = true;
        Serial.println("[BOOT] Control active");
        if (g_lcdOK) lcd->clear();
    }

    if (now - g_lastSensorMs >= SENSOR_INTERVAL_MS) {
        g_lastSensorMs = now;
        readSensors();
        bufferDataPoint();
        if (g_startupHoldDone) {
            controlPump();
            controlFan();
        }
    }

    sendToBlynk();
    sendToThingSpeak();
    checkWiFi();
    updateLCD();
    printStatus();
}
