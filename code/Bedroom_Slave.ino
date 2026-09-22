#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <Adafruit_MCP9808.h>

#define WIFI_SSID "Reyy Ra"
#define WIFI_PASSWORD "[REDACTED]"
#define PKT_SIZE 128

// ESP32-S3 Pin Allocations
#define LED_PIN 3
#define FAN_PWM 9
#define TOUCH_LED 10
#define TOUCH_FAN 37
#define PIR_PIN 38 // IR sensor pin
#define SDA_PIN 39
#define SCL_PIN 40

#define PWM_FREQ 25000
#define PWM_RESOLUTION 8
#define FAN_OFF 0
#define FAN_LOW 100
#define FAN_HIGH 255
#define TEMP_HIGH 30.0
#define TEMP_LOW 27.0

// -- IR Sensor Configuration --
#define IR_ACTIVE_STATE LOW

uint8_t masterMAC[] = {0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX}; // [REDACTED]

// -- State variables --
bool ledState = false;
int fanSpeed = FAN_OFF;
bool autoMode = false;
bool confirmedPIR = false;

// -- Timers & Intervals --
unsigned long bootIgnoreUntil = 0;
#define BOOT_IGNORE_MS 6000

unsigned long lastHelloTime = 0;
#define HELLO_INTERVAL_MS 5000

// IR Sensor Filter Timers
unsigned long pirLowSince = 0;
bool pirLowTimerRunning = false;
#define PIR_CLEAR_MS 3000      // Time to wait before clearing motion
#define IR_DEBOUNCE_MS 150     // Time the IR sensor state must remain stable

// Temperature Cache (Prevents I2C lockups)
float cachedTemp = 25.0;
unsigned long lastTempCheck = 0;
#define TEMP_READ_INTERVAL_MS 4000

// ESP-NOW Safe Transmission Queue
unsigned long lastStatusSent = 0;
#define STATUS_MIN_INTERVAL_MS 200
bool pendingStatusSend = false;

char outBuf[PKT_SIZE];
bool masterAcked = false;

Adafruit_MCP9808 tempSensor;
bool mcpFound = false;

                                                                                Home Automation System — Project Documentation

// Forward Declarations
void sendFullStatus();
void sendFullStatusInstant();
void updateTemperatureCached();
void processPendingStatus();

// -- Hardware Control --
void setLED(bool on) {
  ledState = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  Serial.println("LED -> " + String(on ? "ON" : "OFF"));
}

void setFanPWM(int speed) {
  fanSpeed = speed;
  ledcWrite(FAN_PWM, speed);
  String level = (speed == FAN_OFF) ? "OFF" : (speed == FAN_LOW) ? "LOW" : "HIGH";
  Serial.println("Fan -> " + level);
}

void updateTemperatureCached() {
  if (!mcpFound) return;
  unsigned long now = millis();
  if (now - lastTempCheck >= TEMP_READ_INTERVAL_MS || lastTempCheck == 0) {
    lastTempCheck = now;
    tempSensor.wake();
    float t = tempSensor.readTempC();
    tempSensor.shutdown_wake(1);
    if (t > -10.0 && t < 60.0) { // Keep only sane readings
      cachedTemp = t;
    }
  }
}

// Turns off appliances but does not alter the autoMode state
void autoShutdown() {
  bool changed = false;
  if (ledState) {
    setLED(false);
    changed = true;
  }
  if (fanSpeed != FAN_OFF) {
    setFanPWM(FAN_OFF);
    changed = true;
  }
  if (changed) {
    sendFullStatus();
  }
}

// -- Status Updates (Kitchen Semicolon Format) --
void sendAlert(String p) {
  String pkt = "ALERT|Bedroom|" + p;
  pkt.toCharArray(outBuf, PKT_SIZE);
  esp_now_send(masterMAC, (uint8_t*)outBuf, PKT_SIZE);
}

// Queue status for sending (used for continuous loop transitions)
void sendFullStatus() {
  pendingStatusSend = true;
}

// Sends status immediately (used for manual operations to keep UI in sync)
void sendFullStatusInstant() {
  unsigned long now = millis();
  lastStatusSent = now;
  pendingStatusSend = false; // Clear any pending queued updates

 String fanLevel = (fanSpeed == FAN_OFF) ? "0" : (fanSpeed == FAN_LOW) ? "1" : "2";
 sendAlert(
   "LIGHT=" + String(ledState ? "1" : "0") +
   ";FAN=" + fanLevel +
   ";AUTO=" + String(autoMode ? "1" : "0") +
   ";TEMP=" + (mcpFound ? String(cachedTemp, 1) : "error") +

                                                                                     Home Automation System — Project Documentation

         ";PIR=" + String(confirmedPIR ? "1" : "0")
    );
}

void processPendingStatus() {
  unsigned long now = millis();
  if (pendingStatusSend && (now - lastStatusSent >= STATUS_MIN_INTERVAL_MS)) {
    pendingStatusSend = false;
    lastStatusSent = now;

         String fanLevel = (fanSpeed == FAN_OFF) ? "0" : (fanSpeed == FAN_LOW) ? "1" : "2";
         sendAlert(
            "LIGHT=" + String(ledState ? "1" : "0") +
            ";FAN=" + fanLevel +
            ";AUTO=" + String(autoMode ? "1" : "0") +
            ";TEMP=" + (mcpFound ? String(cachedTemp, 1) : "error") +
            ";PIR=" + String(confirmedPIR ? "1" : "0")
         );
    }
}

// -- ESP-NOW Receive --
void onDataReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  char tempBuf[PKT_SIZE];
  int copyLen = (len < PKT_SIZE) ? len : (PKT_SIZE - 1);
  memcpy(tempBuf, data, copyLen);
  tempBuf[copyLen] = '\0';

    String msg = String(tempBuf);
    if (!masterAcked) {
      masterAcked = true;
    }

    int p1 = msg.indexOf('|'), p2 = msg.indexOf('|', p1+1);
    if (p1 < 0 || p2 < 0) return;
    String type = msg.substring(0, p1);
    String room = msg.substring(p1+1, p2);
    String cmd = msg.substring(p2+1);

    if (type != "CMD" || room != "Bedroom") return;
    if (millis() < bootIgnoreUntil) return;

    // Manual commands do not reset autoMode to false.
    if (cmd == "LED_ON") {
      setLED(true);
      sendFullStatusInstant();
    }
    else if (cmd == "LED_OFF") {
      setLED(false);
      sendFullStatusInstant();
    }
    else if (cmd == "FAN_ON" || cmd == "FAN_LOW") {
      setFanPWM(FAN_LOW);
      sendFullStatusInstant();
    }
    else if (cmd == "FAN_HIGH") {
      setFanPWM(FAN_HIGH);
      sendFullStatusInstant();
    }
    else if (cmd == "FAN_OFF") {
      setFanPWM(FAN_OFF);
      sendFullStatusInstant();
    }
    else if (cmd == "AUTO_ON") {
      autoMode = true;
      if (!confirmedPIR) {
        autoShutdown(); // Immediate initial shutdown if empty on activation
      }
      sendFullStatusInstant();
    }
    else if (cmd == "AUTO_OFF") {
      autoMode = false;
      sendFullStatusInstant();
    }

                                                                          Home Automation System — Project Documentation

    else if (cmd == "STATUS") {
      sendFullStatusInstant();
    }
}

void setup() {
  Serial.begin(115200);
  bootIgnoreUntil = millis() + BOOT_IGNORE_MS;

    pinMode(LED_PIN, OUTPUT);
    setLED(false);

    pinMode(PIR_PIN, INPUT);

    // Touch pads configured with pull-downs
    pinMode(TOUCH_LED, INPUT_PULLDOWN);
    pinMode(TOUCH_FAN, INPUT_PULLDOWN);

    ledcAttach(FAN_PWM, PWM_FREQ, PWM_RESOLUTION);
    ledcWrite(FAN_PWM, FAN_OFF);

    // Configure I2C with hardware safety timeouts
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setTimeOut(100);

    if (tempSensor.begin(0x18)) {
      mcpFound = true;
      tempSensor.setResolution(3);
      Serial.println("MCP9808 found!");
    } else {
      Serial.println("MCP9808 NOT found!");
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
    }

    if (esp_now_init() == ESP_OK) {
      esp_now_register_recv_cb(onDataReceived);
      esp_now_peer_info_t mp;
      memset(&mp, 0, sizeof(mp));
      memcpy(mp.peer_addr, masterMAC, 6);
      esp_now_add_peer(&mp);
    }

    Serial.println("Bedroom ESP32-S3 System Ready.");
}

void loop() {
  unsigned long now = millis();

    // Update temperature reading periodically in the background
    updateTemperatureCached();

    // Handshake loop
    if (!masterAcked && now - lastHelloTime > HELLO_INTERVAL_MS) {
      lastHelloTime = now;
      String pkt = "HELLO|Bedroom|";
      esp_now_send(masterMAC, (uint8_t*)pkt.c_str(), pkt.length());
    }

    bool booting = (now < bootIgnoreUntil);

    if (!booting) {
      // 1. Debounced IR Sensor State Processing (Edge-Triggered Logic)
      bool rawSensorState = (digitalRead(PIR_PIN) == IR_ACTIVE_STATE);
      static bool lastRawSensorState = false;
      static unsigned long lastRawStateChange = 0;

                                                                             Home Automation System — Project Documentation


if (rawSensorState != lastRawSensorState) {
  lastRawSensorState = rawSensorState;
  lastRawStateChange = now;
}

if (now - lastRawStateChange > IR_DEBOUNCE_MS) {
  if (rawSensorState) {
    // Confirmed Motion Detected
    pirLowTimerRunning = false;
    if (!confirmedPIR) {
      confirmedPIR = true;
      Serial.println("Motion Detected!");
      sendFullStatus();
    }
  } else {
    // Confirmed Motion Cleared
    if (confirmedPIR) {
      if (!pirLowTimerRunning) {
        pirLowTimerRunning = true;
        pirLowSince = now;
      } else if (now - pirLowSince >= PIR_CLEAR_MS) {
        confirmedPIR = false;
        Serial.println("Motion Cleared");
        if (autoMode) {
          autoShutdown(); // Edge-Triggered Shutdown: Runs once when empty
        } else {
          sendFullStatus();
        }
        pirLowTimerRunning = false;
      }
    }
  }
}

// 2. Stable State-Machine Debouncer for TOUCH_LED
static bool debouncedTouchLED = false;
static bool lastRawTouchLED = false;
static unsigned long lastTouchLEDTime = 0;
#define DEBOUNCE_DELAY_MS 50

bool rawTouchLED = (digitalRead(TOUCH_LED) == HIGH);
if (rawTouchLED != lastRawTouchLED) {
  lastRawTouchLED = rawTouchLED;
  lastTouchLEDTime = now;
}

if ((now - lastTouchLEDTime) > DEBOUNCE_DELAY_MS) {
  if (rawTouchLED != debouncedTouchLED) {
    debouncedTouchLED = rawTouchLED;
    if (debouncedTouchLED) { // Triggers exactly once on rising edge (touch press)
      setLED(!ledState);
      sendFullStatusInstant();
    }
  }
}

// 3. Stable State-Machine Debouncer for TOUCH_FAN
static bool debouncedTouchFAN = false;
static bool lastRawTouchFAN = false;
static unsigned long lastTouchFANTime = 0;

bool rawTouchFAN = (digitalRead(TOUCH_FAN) == HIGH);
if (rawTouchFAN != lastRawTouchFAN) {
  lastRawTouchFAN = rawTouchFAN;
  lastTouchFANTime = now;
}

if ((now - lastTouchFANTime) > DEBOUNCE_DELAY_MS) {
  if (rawTouchFAN != debouncedTouchFAN) {
    debouncedTouchFAN = rawTouchFAN;
    if (debouncedTouchFAN) { // Triggers exactly once on rising edge (touch press)
      setFanPWM(fanSpeed == FAN_OFF ? FAN_LOW : FAN_OFF);
      sendFullStatusInstant();

                                                                         Home Automation System — Project Documentation

                }
            }
        }

        // 4. Auto Mode Dynamic Temp Adjustments
        if (autoMode && confirmedPIR) {
          if (fanSpeed != FAN_OFF && (now - lastTempCheck > 3000)) {
            if (cachedTemp >= TEMP_HIGH && fanSpeed != FAN_HIGH) {
              setFanPWM(FAN_HIGH);
              sendFullStatus();
            } else if (cachedTemp < TEMP_LOW && fanSpeed != FAN_LOW) {
              setFanPWM(FAN_LOW);
              sendFullStatus();
            }
          }
        }
    }

    // Handle status updates safely
    processPendingStatus();

    delay(20);
}

                                                                          Home Automation System — Project Documentation
