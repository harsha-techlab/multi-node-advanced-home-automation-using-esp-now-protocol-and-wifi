/*
  ============================================================
  Home Automation - HALL + DOOR SLAVE (ESP32-S3) - FINAL (REVISED)
  ============================================================
  HALL:
    Light    : GPIO37 | Fan           : GPIO38
    TLight   : GPIO39 | TFan          : GPIO40
    IR       : GPIO41 | LDR(digital): GPIO3
    Buzzer   : GPIO9
  DOOR:
    Solenoid : GPIO1   | Touch        : GPIO2
  WiFi : Reyy Ra / [REDACTED]
  Master: [REDACTED]
  ============================================================
*/
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>

#define WIFI_SSID       "Reyy Ra"
#define WIFI_PASSWORD   "[REDACTED]"

#define LIGHT_PIN       37
#define FAN_PIN         38
#define TOUCH_LIGHT_PIN 39
#define TOUCH_FAN_PIN   40
#define IR_PIN          41
#define LDR_PIN         3
#define BUZZER_PIN      9
#define SOLENOID_PIN    1
#define DOOR_TOUCH_PIN 2

uint8_t masterMAC[] = {0xXX,0xXX,0xXX,0xXX,0xXX,0xXX}; // [REDACTED]

// -- State --
bool lightOn      = false;
bool fanOn        = false;
bool autoMode     = false;
bool buzzerActive = false;
bool motionNow    = false;
bool isDark       = false;
bool isLocked     = true;

// -- Echo guards --
int lastSentLight = -1;
int lastSentFan      = -1;
int lastSentLocked = 1;
unsigned long lastLocalLightChange = 0;
unsigned long lastLocalFanChange   = 0;
unsigned long lastDoorLocalChange = 0;
#define LOCAL_GUARD_MS 3000

// -- Timers --
unsigned long lastLightOffTime    = 0;
unsigned long lastMotionTime      = 0;
unsigned long lastStateChangeTime = 0;
unsigned long lastDoorTouchTime   = 0;

                                                                                Home Automation System — Project Documentation

unsigned long lastHelloTime         = 0;
unsigned long bootIgnoreUntil       = 0;

#define BOOT_IGNORE_MS      5000
#define TOUCH_DEBOUNCE_MS   500
#define DEBOUNCE_LOCKOUT_MS 500
#define MOTION_TIMEOUT_MS   5000
#define LDR_RECOVERY_MS     15000
#define HELLO_INTERVAL_MS   4000

bool masterAcked = false;

char rxBuffer[128];
char processBuf[128];
volatile bool newMsgAvailable = false;

Preferences preferences;

// -----------------------------------------------
void triggerBeep(int ms) {
  digitalWrite(BUZZER_PIN, HIGH); delay(ms); digitalWrite(BUZZER_PIN, LOW);
}

void setBuzzer(bool on) {
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  buzzerActive = on;
}

void setLight(bool on) {
  if (lightOn == on) return;
  lightOn = on;
  digitalWrite(LIGHT_PIN, on ? HIGH : LOW);
  lastStateChangeTime = millis();
  preferences.putBool("lightOn", lightOn);
  if (!on) lastLightOffTime = millis();
}

void setFan(bool on) {
  if (fanOn == on) return;
  fanOn = on;
  digitalWrite(FAN_PIN, on ? HIGH : LOW);
  lastStateChangeTime = millis();
  preferences.putBool("fanOn", fanOn);
}

void setLock(bool locked) {
  isLocked = locked;
  digitalWrite(SOLENOID_PIN, locked ? LOW : HIGH);
  preferences.putBool("isLocked", isLocked);
  Serial.println(locked ? "LOCKED" : "UNLOCKED");
}

void sendHallStatus() {
  String pkt = "ALERT|Hall|"
    "LIGHT=" + String(lightOn    ?"1":"0") +
    ";FAN=" + String(fanOn       ?"1":"0") +
    ";AUTO=" + String(autoMode   ?"1":"0") +
    ";BUZZER="+ String(buzzerActive?"1":"0");
  esp_now_send(masterMAC, (uint8_t*)pkt.c_str(), pkt.length()+1);
  Serial.println("TX Hall: " + pkt.substring(11));
  lastSentLight = lightOn ? 1 : 0;
  lastSentFan   = fanOn   ? 1 : 0;
}

void sendDoorStatus() {
  String pkt = "ALERT|Door|LOCKED=" + String(isLocked?"1":"0") + ";CAMERA=1";
  esp_now_send(masterMAC, (uint8_t*)pkt.c_str(), pkt.length()+1);
  Serial.println("TX Door: LOCKED=" + String(isLocked?"1":"0"));
  lastSentLocked = isLocked ? 1 : 0;
}

void sendHello() {

                                                                                   Home Automation System — Project Documentation

    String h = "HELLO|Hall|";
    esp_now_send(masterMAC, (uint8_t*)h.c_str(), h.length()+1);
    delay(50);
    String d = "HELLO|Door|";
    esp_now_send(masterMAC, (uint8_t*)d.c_str(), d.length()+1);
    Serial.println("HELLO Hall+Door -> Master");
}

// -----------------------------------------------
void onDataReceived(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len) {
  masterAcked = true;
  int n = min(len, 127);
  memcpy(rxBuffer, data, n);
  rxBuffer[n] = '\0';
  newMsgAvailable = true;
}

void onDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  // Serial.println(status==ESP_NOW_SEND_SUCCESS?"OK":"FAIL");
}

// -----------------------------------------------
void processCommand(String msg) {
  Serial.println("RX: " + msg);

    int p1 = msg.indexOf('|'), p2 = msg.indexOf('|', p1+1);
    if (p1 < 0 || p2 < 0) return;
    String type = msg.substring(0, p1);
    String room = msg.substring(p1+1, p2);
    String cmd = msg.substring(p2+1);

    if (type != "CMD") return;

    if (millis() < bootIgnoreUntil) {
      Serial.println("Boot window - skip: " + cmd); return;
    }

    Serial.println("[" + room + "] CMD: " + cmd);

    // -- DOOR --
    if (room == "Door") {
      if (cmd == "LOCK") {
        if (millis()-lastDoorLocalChange<LOCAL_GUARD_MS && lastSentLocked==1) {
          Serial.println("Door echo ignored"); return;
        }
        setLock(true); lastSentLocked=1; sendDoorStatus();
      }
      else if (cmd == "UNLOCK") {
        if (millis()-lastDoorLocalChange<LOCAL_GUARD_MS && lastSentLocked==0) {
          Serial.println("Door echo ignored"); return;
        }
        setLock(false); lastSentLocked=0; sendDoorStatus();
      }
      else if (cmd == "STATUS") { sendDoorStatus(); }
      return;
    }

  // -- HALL --
  if (room == "Hall") {
    bool changed = false;
    if (cmd == "LED_ON") {
      if (millis()-lastLocalLightChange<LOCAL_GUARD_MS && lastSentLight==1) { Serial.println("Light echo ignored");
return; }
      setLight(true); changed=true;
    }
    else if (cmd == "LED_OFF") {
      if (millis()-lastLocalLightChange<LOCAL_GUARD_MS && lastSentLight==0) { Serial.println("Light echo ignored");
return; }
      setLight(false); changed=true;
    }
    else if (cmd == "FAN_ON") {
      if (millis()-lastLocalFanChange<LOCAL_GUARD_MS && lastSentFan==1) { Serial.println("Fan echo ignored"); return; }

                                                                                Home Automation System — Project Documentation

      setFan(true); changed=true;
    }
    else if (cmd == "FAN_OFF") {
      if (millis()-lastLocalFanChange<LOCAL_GUARD_MS && lastSentFan==0) { Serial.println("Fan echo ignored"); return; }
      setFan(false); changed=true;
    }
    else if (cmd == "AUTO_ON") { autoMode=true; preferences.putBool("autoMode",true); Serial.println("Auto ON");
changed=true; }
    else if (cmd == "AUTO_OFF") { autoMode=false; preferences.putBool("autoMode",false); Serial.println("Auto OFF");
changed=true; }
    else if (cmd == "BUZZER_ON") { setBuzzer(true); preferences.putBool("buzzerActive",true); changed=true; }
    else if (cmd == "BUZZER_OFF") { setBuzzer(false); preferences.putBool("buzzerActive",false); changed=true; }
    else if (cmd == "STATUS") { sendHallStatus(); return; }
    if (changed) sendHallStatus();
  }
}

// -----------------------------------------------
void setup() {
  Serial.begin(115200); delay(500);
  bootIgnoreUntil = millis() + BOOT_IGNORE_MS;
  Serial.println("HALL+DOOR SLAVE BOOTING");

 pinMode(LIGHT_PIN,          OUTPUT);
 pinMode(FAN_PIN,            OUTPUT);
 pinMode(BUZZER_PIN,         OUTPUT);
 pinMode(SOLENOID_PIN,       OUTPUT);

 pinMode(TOUCH_LIGHT_PIN, INPUT_PULLDOWN);
 pinMode(TOUCH_FAN_PIN,   INPUT_PULLDOWN);
 pinMode(DOOR_TOUCH_PIN, INPUT_PULLDOWN);

 pinMode(IR_PIN,             INPUT);
 pinMode(LDR_PIN,            INPUT);

 preferences.begin("home-node", false);
 autoMode     = preferences.getBool("autoMode",     false);
 lightOn      = preferences.getBool("lightOn",      false);
 fanOn        = preferences.getBool("fanOn",        false);
 buzzerActive = preferences.getBool("buzzerActive", false);
 isLocked     = preferences.getBool("isLocked",     true);

 digitalWrite(LIGHT_PIN, lightOn       ? HIGH : LOW);
 digitalWrite(FAN_PIN,    fanOn        ? HIGH : LOW);
 digitalWrite(BUZZER_PIN, buzzerActive ? HIGH : LOW);
 setLock(isLocked);

 lastSentLight = lightOn ? 1 : 0;
 lastSentFan    = fanOn    ? 1 : 0;
 lastSentLocked = isLocked ? 1 : 0;

 triggerBeep(150);

 WiFi.mode(WIFI_STA);
 WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
 int cnt=0;
 while (WiFi.status()!=WL_CONNECTED && cnt<30) { delay(500); cnt++; }

 esp_wifi_set_ps(WIFI_PS_NONE);

 if (esp_now_init()!=ESP_OK) { Serial.println("ESP-NOW failed!"); return; }
 esp_now_register_recv_cb(onDataReceived);
 esp_now_register_send_cb(onDataSent);

 esp_now_peer_info_t peer={};
 memcpy(peer.peer_addr, masterMAC, 6);
 peer.channel=0; peer.encrypt=false;
 esp_now_add_peer(&peer);

 sendHello();
 lastHelloTime = millis();

                                                                                  Home Automation System — Project Documentation

}

// -----------------------------------------------
void loop() {
  if (newMsgAvailable) {
    noInterrupts();
    strcpy(processBuf, rxBuffer);
    newMsgAvailable = false;
    interrupts();
    processCommand(String(processBuf));
  }

    if (!masterAcked && millis()-lastHelloTime > HELLO_INTERVAL_MS) {
      lastHelloTime=millis(); sendHello();
    }

    motionNow = (digitalRead(IR_PIN)==LOW);
    bool rawDark = (digitalRead(LDR_PIN)==HIGH);
    isDark = (millis()-lastLightOffTime < LDR_RECOVERY_MS) ? true : rawDark;
    if (motionNow) lastMotionTime = millis();

    if (autoMode) {
      if (motionNow && isDark && !lightOn) {
        lastLocalLightChange=millis();
        setLight(true); sendHallStatus();
      }
      else if (!motionNow && lightOn && millis()-lastMotionTime > MOTION_TIMEOUT_MS) {
        lastLocalLightChange=millis();
        setLight(false); sendHallStatus();
      }
    }

    if (millis()-lastStateChangeTime > DEBOUNCE_LOCKOUT_MS) {
      static bool lastTL=false;
      bool tL=(digitalRead(TOUCH_LIGHT_PIN)==HIGH);
      if (tL && !lastTL) {
        lastLocalLightChange=millis();
        triggerBeep(80); setLight(!lightOn); sendHallStatus();
      }
      lastTL=tL;

        static bool lastTF=false;
        bool tF=(digitalRead(TOUCH_FAN_PIN)==HIGH);
        if (tF && !lastTF) {
          lastLocalFanChange=millis();
          triggerBeep(80); setFan(!fanOn); sendHallStatus();
        }
        lastTF=tF;
    }

    static bool lastTD=false;
    bool tD=(digitalRead(DOOR_TOUCH_PIN)==HIGH);
    if (tD && !lastTD && millis()-lastDoorTouchTime > TOUCH_DEBOUNCE_MS) {
      lastDoorTouchTime    = millis();
      lastDoorLocalChange = millis();
      bool ns = !isLocked;
      setLock(ns); lastSentLocked=ns?1:0;
      sendDoorStatus(); triggerBeep(80);
    }
    lastTD=tD;
}

                                                                               Home Automation System — Project Documentation
