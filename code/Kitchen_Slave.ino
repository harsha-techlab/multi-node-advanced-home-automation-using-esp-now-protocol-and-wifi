#include <WiFi.h>
#include <esp_now.h>

#define WIFI_SSID           "AP_302"
#define WIFI_PASSWORD       "[REDACTED]"

#define LIGHT_PIN           45
#define EXHAUST_PIN         48
#define TOUCH_LIGHT_PIN     47
#define TOUCH_EXHST_PIN     21
#define MQ2_DOUT_PIN        13

uint8_t masterMAC[] = {0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX}; // [REDACTED]

// -- State --
bool lightOn     = false;
bool exhaustOn   = false;
bool gasDetected = false;
bool buzzerOn    = false;
bool lastGasState = false;

// -- Gas Logic --
int gasConfidence = 0;
#define GAS_THRESHOLD 3        // Reduced to 3 for faster automatic response
unsigned long warmUpFinishedTime = 0;
#define WARM_UP_DELAY_MS 30000 // 30s warmup to prevent false boot alarms

// -- Timers --
unsigned long lastTouchLightTime = 0, lastTouchExhstTime = 0;
#define TOUCH_DEBOUNCE_MS 500
unsigned long bootIgnoreUntil = 0;
#define BOOT_IGNORE_MS 6000
unsigned long lastGasCheck = 0;
#define GAS_CHECK_MS 200
unsigned long lastHelloTime = 0;
#define HELLO_INTERVAL_MS 5000

char outBuf[128];
bool masterAcked = false;

// -----------------------------------------------
void setLight(bool on) {
  lightOn = on;
  digitalWrite(LIGHT_PIN, on ? HIGH : LOW);
  Serial.println("Light -> " + String(on ? "ON" : "OFF"));
}

void setExhaust(bool on) {
  exhaustOn = on;
  digitalWrite(EXHAUST_PIN, on ? HIGH : LOW);
  Serial.println("Exhaust -> " + String(on ? "ON" : "OFF"));
}

void sendAlert(String p) {
  String pkt = "ALERT|Kitchen|" + p;
  pkt.toCharArray(outBuf, 128);
  esp_now_send(masterMAC, (uint8_t*)outBuf, 128);
}

void sendFullStatus() {
  sendAlert(
     "LIGHT="   + String(lightOn    ? "1" : "0") +
     ";EXHAUST="+ String(exhaustOn ? "1" : "0") +
     ";GAS="    + String(gasDetected ? "1" : "0") +
     ";BUZZER=" + String(buzzerOn   ? "1" : "0")
  );
}

// -- ESP-NOW Receive --

                                                                                  Home Automation System — Project Documentation

void onDataReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  String msg = String((char*)data);
  if (!masterAcked) { masterAcked = true; }

    int p1 = msg.indexOf('|'), p2 = msg.indexOf('|', p1+1);
    if (p1<0||p2<0) return;
    String type = msg.substring(0, p1);
    String room = msg.substring(p1+1, p2);
    String cmd = msg.substring(p2+1);
    if (type!="CMD"||room!="Kitchen") return;

    if (millis() < bootIgnoreUntil) return;

    if (cmd == "LED_ON") { setLight(true); sendFullStatus(); }
    else if (cmd == "LED_OFF") { setLight(false); sendFullStatus(); }
    else if (cmd == "EXHAUST_ON") { setExhaust(true); sendFullStatus(); }
    else if (cmd == "EXHAUST_OFF") {
      // BLOCK "OFF" command if gas is still present
      if (!gasDetected) { setExhaust(false); sendFullStatus(); }
      else { Serial.println("Command Ignored: Gas detected, Exhaust must stay ON"); }
    }
    else if (cmd == "STATUS") { sendFullStatus(); }
}

// -- AUTOMATIC GAS LOGIC --
void checkGas() {
  if (millis() < warmUpFinishedTime) return;

    bool rawGas = (digitalRead(MQ2_DOUT_PIN) == LOW); // LOW means Gas detected

    if (rawGas) {
      if (gasConfidence < GAS_THRESHOLD) gasConfidence++;
    } else {
      if (gasConfidence > 0) gasConfidence--;
    }

    bool currentGasStatus = (gasConfidence >= GAS_THRESHOLD);

    if (currentGasStatus != lastGasState) {
      lastGasState = currentGasStatus;
      gasDetected = currentGasStatus;

        if (gasDetected) {
          // GAS DETECTED - ACTING AUTOMATICALLY
          buzzerOn = true;
          setExhaust(true); // <--- AUTOMATIC ON
          Serial.println("GAS DETECTED! Exhaust turned ON automatically.");
          sendFullStatus();
        } else {
          // GAS CLEARED - ACTING AUTOMATICALLY
          buzzerOn = false;
          setExhaust(false); // <--- AUTOMATIC OFF
          Serial.println("GAS CLEARED. Exhaust turned OFF automatically.");
          sendFullStatus();
        }
    }
}

void setup() {
  Serial.begin(115200);
  bootIgnoreUntil = millis() + BOOT_IGNORE_MS;
  warmUpFinishedTime = millis() + WARM_UP_DELAY_MS;

    pinMode(LIGHT_PIN, OUTPUT);
    pinMode(EXHAUST_PIN, OUTPUT);
    pinMode(TOUCH_LIGHT_PIN, INPUT);
    pinMode(TOUCH_EXHST_PIN, INPUT);
    pinMode(MQ2_DOUT_PIN,    INPUT_PULLUP);

    setLight(false);
    setExhaust(false);

                                                                      Home Automation System — Project Documentation


    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status()!=WL_CONNECTED) { delay(500); }

    if (esp_now_init()==ESP_OK) {
      esp_now_register_recv_cb(onDataReceived);
      esp_now_peer_info_t mp; memset(&mp,0,sizeof(mp));
      memcpy(mp.peer_addr, masterMAC, 6);
      esp_now_add_peer(&mp);
    }

    Serial.println("System Ready. MQ-2 Warming up...");
}

void loop() {
  if (!masterAcked && millis()-lastHelloTime > HELLO_INTERVAL_MS) {
    lastHelloTime = millis();
    String pkt = "HELLO|Kitchen|";
    esp_now_send(masterMAC, (uint8_t*)pkt.c_str(), pkt.length());
  }

    // 1. Always check gas first for safety
    if (millis() - lastGasCheck > GAS_CHECK_MS) {
      lastGasCheck = millis();
      checkGas();
    }

    // 2. Touch Light
    bool tL = (digitalRead(TOUCH_LIGHT_PIN)==HIGH);
    if (tL && millis()-lastTouchLightTime > TOUCH_DEBOUNCE_MS) {
      lastTouchLightTime = millis();
      setLight(!lightOn);
      sendFullStatus();
    }

    // 3. Touch Exhaust (Blocked if gas is present)
    bool tE = (digitalRead(TOUCH_EXHST_PIN)==HIGH);
    if (tE && millis()-lastTouchExhstTime > TOUCH_DEBOUNCE_MS) {
      lastTouchExhstTime = millis();
      if (!gasDetected) {
        setExhaust(!exhaustOn);
        sendFullStatus();
      } else {
        Serial.println("Touch ignored: Safety lock active (Gas).");
      }
    }
}

                                                                                Home Automation System — Project Documentation
