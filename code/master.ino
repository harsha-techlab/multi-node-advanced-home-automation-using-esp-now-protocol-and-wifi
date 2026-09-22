/*
  ============================================================
  Home Automation - ESP32 MASTER - FINAL (FULL INTEGRATION)
  ============================================================
  Master MAC : B8:F0:09:AD:C6:38
  WiFi       : AP_302 / F@rthebest
  
  SUPPORTED SLAVES:
  - Hall+Door : Light, Fan, Auto Mode, Buzzer, Motion, LDR, Solenoid Lock
  - Bedroom   : Light, Fan, Auto Mode, Temperature, Fan Speed, PIR
  - Kitchen   : Light, Exhaust, Gas Detection, Buzzer
  ============================================================
*/
#include <WiFi.h>
#include <esp_now.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

#define WIFI_SSID      "AP_302"
#define WIFI_PASSWORD  "F@rthebest"
#define API_KEY        "AIzaSyA2jnWiW8CmXdDI7FoqBCC2OLsxgM5ztEI"
#define DATABASE_URL   "https://autobot-38246-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define USER_EMAIL     "gunasyampentakota045@gmail.com"
#define USER_PASSWORD  "Guna@123"
#define CAM_STREAM_URL "http://192.168.31.205:8080/video"

FirebaseData   fbdoCmd;
FirebaseAuth   auth;
FirebaseConfig config;
bool firebaseReady = false;

#define MAX_SLAVES 20
struct Slave { uint8_t mac[6]; char name[20]; };
Slave slaves[MAX_SLAVES];
int slaveCount = 0;
#define PKT_SIZE 128
char outBuf[PKT_SIZE];

struct NewSlave { uint8_t mac[6]; char name[20]; bool pending; };
NewSlave pendingSlave;

#define MAX_EVENTS 50
struct SlaveEvent { String room; String payload; };
SlaveEvent eventQueue[MAX_EVENTS];
int eventCount = 0;

#define MAX_CMD_QUEUE 50
struct EspNowCmd { String room; String cmd; };
EspNowCmd cmdQueue[MAX_CMD_QUEUE];
int cmdQueueCount = 0;

#define MAX_PENDING 20
struct PendingCmd { String room; String cmd; unsigned long t; };
PendingCmd pendingCmds[MAX_PENDING];
int pendingCmdCount = 0;

bool hallStreamInit    = false;
bool bedroomStreamInit = false;
bool kitchenStreamInit = false;
bool doorStreamInit    = false;

// ============= ROOM STATES =============
struct RoomState {
  int light   = -1;
  int fan     = -1;
  int autoMode= -1;
  int locked  = -1;
  int exhaust = -1;
  int buzzer  = -1;
  int fanSpeed = -1;
  float temperature = -1;
  int gas = -1;
  int motion = -1;
  int ldr = -1;
  int camera = -1;
};
RoomState stateHall, stateBedroom, stateKitchen, stateDoor;

bool stateChanged(int &stored, int newVal) {
  if (stored==newVal) return false;
  stored=newVal; return true;
}

void queueEvent(String room, String payload) {
  if (eventCount<MAX_EVENTS) eventQueue[eventCount++]={room,payload};
}

void queueEspNowCmd(String room, String cmd) {
  if (cmdQueueCount<MAX_CMD_QUEUE) {
    cmdQueue[cmdQueueCount++]={room,cmd};
    Serial.println("  📥 Queued: "+room+" -> "+cmd);
  }
}

int findSlave(String name) {
  for (int i=0;i<slaveCount;i++)
    if (String(slaves[i].name)==name) return i;
  return -1;
}

void sendToRoom(String room, String cmd) {
  int idx=findSlave(room);
  if (idx==-1) {
    if (pendingCmdCount<MAX_PENDING)
      pendingCmds[pendingCmdCount++]={room,cmd,millis()};
    Serial.println("  ⚠️  "+room+" not found — queued: "+cmd);
    return;
  }
  String pkt="CMD|"+room+"|"+cmd;
  pkt.toCharArray(outBuf,PKT_SIZE);
  esp_now_send(slaves[idx].mac,(uint8_t*)outBuf,PKT_SIZE);
  Serial.println("  📡 TX -> "+room+" : "+cmd);
}

void printSlaveStatus() {
  Serial.println("┌──────────────┬──────────────────────┐");
  Serial.println("│ Room         │ MAC                  │");
  Serial.println("├──────────────┼──────────────────────┤");
  if (slaveCount==0) { Serial.println("│      No slaves connected yet        │"); }
  else for (int i=0;i<slaveCount;i++) {
    char mac[18];
    snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X",
      slaves[i].mac[0],slaves[i].mac[1],slaves[i].mac[2],
      slaves[i].mac[3],slaves[i].mac[4],slaves[i].mac[5]);
    Serial.printf("│ %-12s │ %-20s │\n",slaves[i].name,mac);
  }
  Serial.println("└──────────────┴──────────────────────┘");
}

void flushPendingCmds(String room, int idx) {
  for (int j=0;j<pendingCmdCount;j++) {
    if (pendingCmds[j].room==room) {
      String pkt="CMD|"+room+"|"+pendingCmds[j].cmd;
      pkt.toCharArray(outBuf,PKT_SIZE);
      esp_now_send(slaves[idx].mac,(uint8_t*)outBuf,PKT_SIZE);
      Serial.println("  📤 Flush: "+pendingCmds[j].cmd+" -> "+room);
      for (int k=j;k<pendingCmdCount-1;k++) pendingCmds[k]=pendingCmds[k+1];
      pendingCmdCount--; j--;
    }
  }
}

void onDataReceived(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len) {
  String msg=String((char*)data);
  int p1=msg.indexOf('|'),p2=msg.indexOf('|',p1+1);
  if (p1<0||p2<0) return;
  String type=msg.substring(0,p1);
  String room=msg.substring(p1+1,p2);
  String data2=msg.substring(p2+1);

  if (type=="HELLO") {
    for (int i=0;i<slaveCount;i++) {
      if (memcmp(slaves[i].mac,info->src_addr,6)==0) {
        Serial.println("🔁 "+room+" re-announced");
        flushPendingCmds(room,i); return;
      }
    }
    if (slaveCount<MAX_SLAVES) {
      memcpy(slaves[slaveCount].mac,info->src_addr,6);
      room.toCharArray(slaves[slaveCount].name,20);
      esp_now_peer_info_t peer; memset(&peer,0,sizeof(peer));
      memcpy(peer.peer_addr,info->src_addr,6);
      peer.channel=0; peer.encrypt=false;
      esp_now_add_peer(&peer);
      int idx=slaveCount++;
      memcpy(pendingSlave.mac,info->src_addr,6);
      room.toCharArray(pendingSlave.name,20);
      pendingSlave.pending=true;
      Serial.println("\n✅ [SLAVE] "+room+" joined!");
      flushPendingCmds(room,idx);
      printSlaveStatus();
    }
    return;
  }
  if (type=="ALERT") {
    Serial.println("\n📨 [ALERT] "+room+" | "+data2);
    queueEvent(room,data2);
  }
}

void pushSlaveDataToFirebase(String room, String payload) {
  if (!firebaseReady) return;
  String path=room; path.toLowerCase();
  FirebaseJson json;
  json.set("online",true);
  
  // Special handling for Door - add stream URL
  if (room=="Door") {
    json.set("streamUrl",CAM_STREAM_URL);
    // Camera trigger on lock/unlock
    if (payload.indexOf("CAMERA=1")>=0) {
      json.set("camera",true);
      json.set("lastCameraTrigger/.sv","timestamp");
    }
  }

  Serial.println("\n🔄 [SYNC] "+room+" -> Firebase");
  int start=0;
  while (start<payload.length()) {
    int sep=payload.indexOf(';',start);
    String pair=(sep==-1)?payload.substring(start):payload.substring(start,sep);
    int eq=pair.indexOf('=');
    if (eq>0) {
      String key=pair.substring(0,eq);
      String val=pair.substring(eq+1);
      
      // ===== HALL / BEDROOM COMMANDS =====
      if (key=="LIGHT") { 
        bool s=(val=="1"); 
        if (room=="Hall") json.set("light",s);
        else if (room=="Bedroom") json.set("bedLight",s);
        Serial.println("   💡 "+(String)(s?"ON":"OFF")); 
      }
      else if (key=="FAN") { 
        bool s=(val=="1"); 
        json.set("fan",s);
        Serial.println("   🌀 "+(String)(s?"ON":"OFF")); 
      }
      else if (key=="AUTO") { 
        bool s=(val=="1"); 
        json.set("autoMode",s);
        Serial.println("   🤖 "+(String)(s?"ON":"OFF")); 
      }
      
      // ===== KITCHEN SPECIFIC =====
      else if (key=="EXHAUST") { 
        bool s=(val=="1"); 
        json.set("exhaustFan",s);
        Serial.println("   💨 Exhaust "+(String)(s?"ON":"OFF")); 
      }
      else if (key=="GAS") { 
        bool s=(val=="1"); 
        json.set("gas",s);
        stateKitchen.gas = s ? 1 : 0;
        Serial.println("   💨 "+(String)(s?"GAS DETECTED ⚠️":"Safe ✅")); 
        if (s) {
          // Forward gas alert to Firebase as critical
          FirebaseJson alert;
          alert.set("gasAlert",true);
          alert.set("timestamp/.sv","timestamp");
          Firebase.RTDB.updateNodeSilent(&fbdoCmd,"/alerts",&alert);
        }
      }
      
      // ===== DOOR SPECIFIC =====
      else if (key=="LOCKED") { 
        bool s=(val=="1"); 
        json.set("locked",s);
        stateDoor.locked = s ? 1 : 0;
        Serial.println("   🔒 "+(String)(s?"LOCKED":"UNLOCKED")); 
        // Log door events
        FirebaseJson log;
        log.set("action", s ? "LOCK" : "UNLOCK");
        log.set("timestamp/.sv","timestamp");
        Firebase.RTDB.pushNodeSilent(&fbdoCmd,"/doorLogs",&log);
      }
      else if (key=="CAMERA") { 
        bool s=(val=="1"); 
        json.set("camera",s);
        Serial.println("   📸 Camera "+(String)(s?"Triggered":"Idle")); 
      }
      
      // ===== BEDROOM SPECIFIC =====
      else if (key=="TEMP") { 
        float temp = val.toFloat();
        json.set("temp",temp);
        stateBedroom.temperature = temp;
        Serial.println("   🌡️  Temp="+String(temp)+"°C"); 
      }
      else if (key=="FANSPEED"){ 
        int speed = val.toInt();
        json.set("fanSpeed",speed);
        stateBedroom.fanSpeed = speed;
        Serial.println("   💨 Fan Speed="+String(speed)+"%");
      }
      
      // ===== HALL SPECIFIC =====
      else if (key=="MOTION") { 
        bool s=(val=="1"); 
        json.set("motion",s);
        stateHall.motion = s ? 1 : 0;
        Serial.println("   🚶 "+(String)(s?"MOTION":"Clear")); 
      }
      else if (key=="LDR") { 
        json.set("ldr",val.toInt());
        stateHall.ldr = val.toInt();
        Serial.println("   ☀️  LDR="+val); 
      }
      
      // ===== BUZZER (All Rooms) =====
      else if (key=="BUZZER") {
        bool s=(val=="1"); 
        json.set("buzzer",s);
        Serial.println("   🔔 Buzzer "+(String)(s?"ON":"OFF"));
        // Kitchen buzzer forwards to Hall
        if (room=="Kitchen" && s) {
          Serial.println("   🔔 Forwarding BUZZER_ON to Hall");
          queueEspNowCmd("Hall","BUZZER_ON");
        }
        if (room=="Kitchen" && !s) {
          Serial.println("   🔔 Forwarding BUZZER_OFF to Hall");
          queueEspNowCmd("Hall","BUZZER_OFF");
        }
      }
    }
    if (sep==-1) break;
    start=sep+1;
  }

  // Update room state tracking
  if (room=="Hall") {
    if (payload.indexOf("LIGHT=1")>=0)  stateHall.light   =1;
    if (payload.indexOf("LIGHT=0")>=0)  stateHall.light   =0;
    if (payload.indexOf("FAN=1")>=0)    stateHall.fan     =1;
    if (payload.indexOf("FAN=0")>=0)    stateHall.fan     =0;
    if (payload.indexOf("AUTO=1")>=0)   stateHall.autoMode=1;
    if (payload.indexOf("AUTO=0")>=0)   stateHall.autoMode=0;
    if (payload.indexOf("BUZZER=1")>=0) stateHall.buzzer=1;
    if (payload.indexOf("BUZZER=0")>=0) stateHall.buzzer=0;
  }
  if (room=="Bedroom") {
    if (payload.indexOf("LIGHT=1")>=0)  stateBedroom.light=1;
    if (payload.indexOf("LIGHT=0")>=0)  stateBedroom.light=0;
    if (payload.indexOf("FAN=1")>=0)    stateBedroom.fan=1;
    if (payload.indexOf("FAN=0")>=0)    stateBedroom.fan=0;
    if (payload.indexOf("AUTO=1")>=0)   stateBedroom.autoMode=1;
    if (payload.indexOf("AUTO=0")>=0)   stateBedroom.autoMode=0;
  }
  if (room=="Kitchen") {
    if (payload.indexOf("LIGHT=1")>=0)   stateKitchen.light=1;
    if (payload.indexOf("LIGHT=0")>=0)   stateKitchen.light=0;
    if (payload.indexOf("EXHAUST=1")>=0) stateKitchen.exhaust=1;
    if (payload.indexOf("EXHAUST=0")>=0) stateKitchen.exhaust=0;
    if (payload.indexOf("BUZZER=1")>=0)  stateKitchen.buzzer=1;
    if (payload.indexOf("BUZZER=0")>=0)  stateKitchen.buzzer=0;
  }
  if (room=="Door") {
    if (payload.indexOf("LOCKED=1")>=0) stateDoor.locked=1;
    if (payload.indexOf("LOCKED=0")>=0) stateDoor.locked=0;
    if (payload.indexOf("CAMERA=1")>=0) stateDoor.camera=1;
  }

  // Push to Firebase
  if (!Firebase.RTDB.updateNodeSilent(&fbdoCmd,("/"+path).c_str(),&json))
    Serial.println("   ❌ "+fbdoCmd.errorReason());
  else
    Serial.println("   ✅ /"+path+" updated");

  // Update master online status
  FirebaseJson mj; 
  mj.set(path+"Online",true);
  mj.set("lastUpdate/.sv","timestamp");
  Firebase.RTDB.updateNodeSilent(&fbdoCmd,"/master",&mj);
}

// ============= HANDLE ROOM CHANGES FROM FIREBASE =============
void handleRoomChange(String room, String key, int newVal) {
  bool changed=false; String cmd="";
  
  if (room=="Hall") {
    if (key=="light"    && stateChanged(stateHall.light,    newVal)) { cmd=newVal?"LED_ON":"LED_OFF";   changed=true; queueEspNowCmd("Hall",cmd); }
    if (key=="fan"      && stateChanged(stateHall.fan,      newVal)) { cmd=newVal?"FAN_ON":"FAN_OFF";       changed=true; queueEspNowCmd("Hall",cmd); }
    if (key=="autoMode" && stateChanged(stateHall.autoMode, newVal)) { cmd=newVal?"AUTO_ON":"AUTO_OFF";     changed=true; queueEspNowCmd("Hall",cmd); }
    if (key=="buzzer"   && stateChanged(stateHall.buzzer,   newVal)) { cmd=newVal?"BUZZER_ON":"BUZZER_OFF"; changed=true; queueEspNowCmd("Hall",cmd); }
  }
  else if (room=="Bedroom") {
    if (key=="bedLight" && stateChanged(stateBedroom.light,    newVal)) { cmd=newVal?"LED_ON":"LED_OFF"; changed=true; queueEspNowCmd("Bedroom",cmd); }
    if (key=="fan"      && stateChanged(stateBedroom.fan,      newVal)) { cmd=newVal?"FAN_ON":"FAN_OFF";     changed=true; queueEspNowCmd("Bedroom",cmd); }
    if (key=="autoMode" && stateChanged(stateBedroom.autoMode, newVal)) { cmd=newVal?"AUTO_ON":"AUTO_OFF";   changed=true; queueEspNowCmd("Bedroom",cmd); }
  }
  else if (room=="Kitchen") {
    if (key=="light"      && stateChanged(stateKitchen.light,   newVal)) { cmd=newVal?"LED_ON":"LED_OFF";             changed=true; queueEspNowCmd("Kitchen",cmd); }
    if (key=="exhaustFan" && stateChanged(stateKitchen.exhaust, newVal)) { cmd=newVal?"EXHAUST_ON":"EXHAUST_OFF";     changed=true; queueEspNowCmd("Kitchen",cmd); }
    if (key=="buzzer"     && stateChanged(stateKitchen.buzzer,  newVal)) { 
      cmd=newVal?"BUZZER_ON":"BUZZER_OFF"; 
      changed=true; 
      queueEspNowCmd("Kitchen",cmd);
      // Also forward to Hall for Kitchen buzzer
      queueEspNowCmd("Hall",cmd);
    }
  }
  else if (room=="Door") {
    if (key=="locked" && stateChanged(stateDoor.locked,newVal)) {
      cmd=newVal?"LOCK":"UNLOCK"; 
      changed=true; 
      queueEspNowCmd("Door",cmd);
    }
  }
  
  if (changed) Serial.println("\n🌐 [WEBSITE->CMD] "+room+" | "+key+" -> "+(newVal?"ON":"OFF")+" -> "+cmd);
  else         Serial.println("\n⏭️  [SKIP] "+room+" | "+key+" no change");
}

void parseJsonNode(String room, String jsonStr) {
  auto extractBool=[](String j,String key)->int{
    int idx=j.indexOf("\""+key+"\""); if(idx<0) return -1;
    int colon=j.indexOf(':',idx);     if(colon<0) return -1;
    String rest=j.substring(colon+1); rest.trim();
    if(rest.startsWith("true"))  return 1;
    if(rest.startsWith("false")) return 0;
    return -1;
  };
  
  if (room=="Hall") {
    int l=extractBool(jsonStr,"light"),f=extractBool(jsonStr,"fan");
    int a=extractBool(jsonStr,"autoMode"),b=extractBool(jsonStr,"buzzer");
    if(l>=0) handleRoomChange("Hall","light",l);
    if(f>=0) handleRoomChange("Hall","fan",f);
    if(a>=0) handleRoomChange("Hall","autoMode",a);
    if(b>=0) handleRoomChange("Hall","buzzer",b);
  }
  else if (room=="Bedroom") {
    int l=extractBool(jsonStr,"bedLight"),f=extractBool(jsonStr,"fan");
    int a=extractBool(jsonStr,"autoMode");
    if(l>=0) handleRoomChange("Bedroom","bedLight",l);
    if(f>=0) handleRoomChange("Bedroom","fan",f);
    if(a>=0) handleRoomChange("Bedroom","autoMode",a);
  }
  else if (room=="Kitchen") {
    int l=extractBool(jsonStr,"light"),e=extractBool(jsonStr,"exhaustFan");
    int b=extractBool(jsonStr,"buzzer");
    if(l>=0) handleRoomChange("Kitchen","light",l);
    if(e>=0) handleRoomChange("Kitchen","exhaustFan",e);
    if(b>=0) handleRoomChange("Kitchen","buzzer",b);
  }
  else if (room=="Door") {
    int lk=extractBool(jsonStr,"locked");
    if(lk>=0) handleRoomChange("Door","locked",lk);
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("\n⏱️  Stream timeout...");
}

FirebaseData fbdoHall,fbdoBedroom,fbdoKitchen,fbdoDoor;

void hallStreamCallback(FirebaseStream data) {
  if (!hallStreamInit) { hallStreamInit=true; return; }
  if      (data.dataType()=="boolean") { String k=data.dataPath(); k.replace("/",""); handleRoomChange("Hall",k,data.boolData()?1:0); }
  else if (data.dataType()=="json")    { parseJsonNode("Hall",data.payload()); }
}

void bedroomStreamCallback(FirebaseStream data) {
  if (!bedroomStreamInit) { bedroomStreamInit=true; return; }
  if      (data.dataType()=="boolean") { String k=data.dataPath(); k.replace("/",""); handleRoomChange("Bedroom",k,data.boolData()?1:0); }
  else if (data.dataType()=="json")    { parseJsonNode("Bedroom",data.payload()); }
}

void kitchenStreamCallback(FirebaseStream data) {
  if (!kitchenStreamInit) { kitchenStreamInit=true; return; }
  if      (data.dataType()=="boolean") { String k=data.dataPath(); k.replace("/",""); handleRoomChange("Kitchen",k,data.boolData()?1:0); }
  else if (data.dataType()=="json")    { parseJsonNode("Kitchen",data.payload()); }
}

void doorStreamCallback(FirebaseStream data) {
  if (!doorStreamInit) { doorStreamInit=true; return; }
  if      (data.dataType()=="boolean") { String k=data.dataPath(); k.replace("/",""); handleRoomChange("Door",k,data.boolData()?1:0); }
  else if (data.dataType()=="json")    { parseJsonNode("Door",data.payload()); }
}

void setupFirebaseStreams() {
  Serial.println("\n📡 Setting up streams...");
  if (!Firebase.RTDB.beginStream(&fbdoHall,"/hall"))       Serial.println("❌ Hall");    else Serial.println("✅ Hall");
  Firebase.RTDB.setStreamCallback(&fbdoHall,    hallStreamCallback,    streamTimeoutCallback);
  if (!Firebase.RTDB.beginStream(&fbdoBedroom,"/bedroom")) Serial.println("❌ Bedroom"); else Serial.println("✅ Bedroom");
  Firebase.RTDB.setStreamCallback(&fbdoBedroom, bedroomStreamCallback, streamTimeoutCallback);
  if (!Firebase.RTDB.beginStream(&fbdoKitchen,"/kitchen")) Serial.println("❌ Kitchen"); else Serial.println("✅ Kitchen");
  Firebase.RTDB.setStreamCallback(&fbdoKitchen, kitchenStreamCallback, streamTimeoutCallback);
  if (!Firebase.RTDB.beginStream(&fbdoDoor,"/door"))       Serial.println("❌ Door");    else Serial.println("✅ Door");
  Firebase.RTDB.setStreamCallback(&fbdoDoor,    doorStreamCallback,    streamTimeoutCallback);
  Serial.println("📡 All streams active!\n");
}

unsigned long lastHeartbeat=0, lastStatusPrint=0;

void sendHeartbeat() {
  if (!firebaseReady) return;
  FirebaseJson json;
  json.set("online",true); 
  json.set("wifi",WiFi.RSSI());
  json.set("espnow",slaveCount); 
  json.set("lastUpdate/.sv","timestamp");
  
  // Track which rooms are online
  for (int i=0;i<slaveCount;i++) {
    String room = String(slaves[i].name);
    room.toLowerCase();
    json.set(room+"Online",true);
  }
  
  if (!Firebase.RTDB.updateNodeSilent(&fbdoCmd,"/master",&json))
    Serial.println("❌ HB failed");
  else
    Serial.println("💓 HB | RSSI:"+String(WiFi.RSSI())+" | Slaves:"+String(slaveCount));
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n╔══════════════════════════════════╗");
  Serial.println("║  HOME AUTOMATION MASTER - BOOT  ║");
  Serial.println("╚══════════════════════════════════╝");
  pendingSlave.pending=false;

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  Serial.print("📶 WiFi");
  while(WiFi.status()!=WL_CONNECTED){delay(500);Serial.print(".");}
  Serial.println("\n✅ IP:"+WiFi.localIP().toString());
  Serial.println("   MAC:B8:F0:09:AD:C6:38");
  Serial.println("   CH:"+String(WiFi.channel()));

  if(esp_now_init()!=ESP_OK){Serial.println("❌ ESP-NOW!"); return;}
  esp_now_register_recv_cb(onDataReceived);
  esp_now_peer_info_t bc; memset(&bc,0,sizeof(bc));
  uint8_t bcMac[]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  memcpy(bc.peer_addr,bcMac,6); bc.channel=0; bc.encrypt=false;
  esp_now_add_peer(&bc);
  Serial.println("✅ ESP-NOW ready!");

  config.api_key=API_KEY; config.database_url=DATABASE_URL;
  auth.user.email=USER_EMAIL; auth.user.password=USER_PASSWORD;
  config.token_status_callback=tokenStatusCallback;
  Firebase.reconnectWiFi(true);
  Firebase.begin(&config,&auth);
  Serial.print("🔥 Firebase");
  unsigned long t0=millis();
  while(!Firebase.ready()&&millis()-t0<20000){delay(300);Serial.print(".");}
  firebaseReady=Firebase.ready();
  if(firebaseReady){
    Serial.println("\n✅ Firebase connected!");
    FirebaseJson di; 
    di.set("streamUrl",CAM_STREAM_URL); 
    di.set("camera",false);
    Firebase.RTDB.updateNodeSilent(&fbdoCmd,"/door",&di);
    setupFirebaseStreams();
    sendHeartbeat();
  } else Serial.println("\n❌ Firebase NOT ready!");
  Serial.println("\n🏠 Master ready!\n");
}

void loop() {
  if(pendingSlave.pending){
    pendingSlave.pending=false;
    String sn=String(pendingSlave.name);
    Serial.println("\n🆕 "+sn+" joined!");
    if(firebaseReady){
      String r=sn; r.toLowerCase();
      FirebaseJson sj; sj.set("online",true);
      Firebase.RTDB.updateNodeSilent(&fbdoCmd,"/"+r,&sj);
      FirebaseJson mj; mj.set(r+"Online",true);
      Firebase.RTDB.updateNodeSilent(&fbdoCmd,"/master",&mj);
    }
    printSlaveStatus();
  }

  if(cmdQueueCount>0){
    sendToRoom(cmdQueue[0].room,cmdQueue[0].cmd);
    for(int i=0;i<cmdQueueCount-1;i++) cmdQueue[i]=cmdQueue[i+1];
    cmdQueueCount--;
  }

  for(int i=0;i<pendingCmdCount;i++){
    if(millis()-pendingCmds[i].t>2000){
      int idx=findSlave(pendingCmds[i].room);
      if(idx>=0){
        String pkt="CMD|"+pendingCmds[i].room+"|"+pendingCmds[i].cmd;
        pkt.toCharArray(outBuf,PKT_SIZE);
        esp_now_send(slaves[idx].mac,(uint8_t*)outBuf,PKT_SIZE);
        Serial.println("  📤 Retry: "+pendingCmds[i].cmd+" -> "+pendingCmds[i].room);
        for(int k=i;k<pendingCmdCount-1;k++) pendingCmds[k]=pendingCmds[k+1];
        pendingCmdCount--; i--;
      } else pendingCmds[i].t=millis();
    }
  }

  if(eventCount>0){
    pushSlaveDataToFirebase(eventQueue[0].room,eventQueue[0].payload);
    for(int i=0;i<eventCount-1;i++) eventQueue[i]=eventQueue[i+1];
    eventCount--;
  }

  if(millis()-lastHeartbeat>15000)  { lastHeartbeat=millis();  sendHeartbeat(); }
  if(millis()-lastStatusPrint>60000){
    lastStatusPrint=millis();
    Serial.println("\n╔══════════════╗");
    Serial.println("║ STATUS       ║");
    Serial.println("║ CH:"+String(WiFi.channel()));
    Serial.println("║ RSSI:"+String(WiFi.RSSI())+"dBm");
    Serial.println("║ Slaves:"+String(slaveCount));
    for(int i=0;i<slaveCount;i++) Serial.println("║  •"+String(slaves[i].name));
    Serial.println("╚══════════════╝\n");
  }
}