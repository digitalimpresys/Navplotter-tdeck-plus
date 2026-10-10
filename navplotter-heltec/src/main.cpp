// NavPlotter node - Heltec WiFi LoRa 32 V3 with an external GPS module
//
// The board has no usable screen for charts, so it does the work and a phone shows it:
//  - reads the GPS, exchanges positions with the other nodes over LoRa (same packet as the T-Deck plotter),
//    relays packets (mesh) and computes the collision alarm;
//  - creates its own WiFi network and serves a web app (web/index.html) plus a small JSON API.
// The small OLED only shows status.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SD.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <TinyGPS++.h>
#include <vector>
#include <math.h>
#include "traffic_packet.h"
#include "web_page.h"

#define FIRMWARE_VERSION "0.2.0"

// ---- Heltec WiFi LoRa 32 V3 pins ----
#define PIN_LORA_NSS 8
#define PIN_LORA_SCK 9
#define PIN_LORA_MOSI 10
#define PIN_LORA_MISO 11
#define PIN_LORA_RST 12
#define PIN_LORA_BUSY 13
#define PIN_LORA_DIO1 14
#define PIN_OLED_SDA 17
#define PIN_OLED_SCL 18
#define PIN_OLED_RST 21
#define PIN_VEXT 36      // LOW = display power on
#define PIN_GPS_TX 47    // board TX -> GPS RX
#define PIN_GPS_RX 48    // board RX <- GPS TX
#define PIN_BAT_ADC 1    // battery voltage through a divider
#define PIN_BAT_CTRL 37  // LOW = divider connected
// Optional micro SD module (chart tiles), on its own SPI bus
#define PIN_SD_CS 4
#define PIN_SD_SCK 5
#define PIN_SD_MOSI 6
#define PIN_SD_MISO 7

// ---- Radio and traffic ----
#define LORA_POWER_DBM 22
#define LORA_TX_MOVING_MS 10000UL
#define LORA_TX_STOPPED_MS 30000UL
#define MOVING_MIN_KNOTS 0.5
#define TARGET_STALE_MS 60000UL
#define TARGET_EXPIRE_MS 180000UL
#define MAX_TARGETS 30
// Collision alarm: closest point of approach below CPA_ALARM_NM within CPA_ALARM_S seconds,
// for targets inside the alarm range chosen in the setup page
#define CPA_ALARM_NM 0.1
#define CPA_ALARM_S 300

// Network scenarios chosen in the setup page
#define NET_OFFGRID 0    // the node's own WiFi, charts from the SD card
#define NET_DIRECT 1     // the node's own WiFi, charts from the internet (through the phone)
#define NET_BOAT 2       // the node joins the boat WiFi; its own WiFi stays on as a way back in
#define STA_CONNECT_MS 20000UL   // how long to wait for the boat WiFi
#define STA_RETRY_MS 180000UL    // then try again this often

#define USERDATA_FILE "/userdata.json"
#define USERDATA_MAX 32768

struct Config {
  char name[BOAT_NAME_LEN] = "";
  uint8_t type = NODE_TYPE_BOAT;
  uint8_t alarmRangeNm = 3;     // 3, 6 or 12
  char lang[3] = "en";          // "en" or "it"
  uint8_t tiles = 0;            // 0 = online, 1 = SD card, 2 = none
  bool sound = true;            // alarm sound on the phone
  char wifiPass[33] = "navplotter";
  uint8_t net = NET_DIRECT;
  char staSsid[33] = "";        // boat WiFi name and password, for NET_BOAT
  char staPass[65] = "";
};

struct Target {
  uint32_t id = 0;
  char name[BOAT_NAME_LEN] = "";
  uint8_t type = NODE_TYPE_BOAT;
  bool simulated = false;
  double lat = 0, lon = 0;
  float cog = 0, sog = 0;   // degrees, knots
  unsigned long lastSeen = 0;
  float rssi = 0;
  bool relayed = false;
  bool cpaValid = false;
  bool alarm = false;
  double cpaNm = 0, tcpaS = 0, distNm = 0, brg = 0;
};

Config config;
Preferences prefs;
WebServer server(80);
SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
SPIClass sdSpi(HSPI);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
TinyGPSPlus gps;

std::vector<Target> targets;
uint32_t myId = 0;
uint32_t gpsBaud = 0;
bool radioOk = false, sdOk = false, fsOk = false;
volatile bool radioIrq = false;
bool transmitting = false, txIsRelay = false;
unsigned long txStart = 0, nextTx = 0;
uint32_t txCount = 0, rxCount = 0, relayCount = 0;
uint8_t seq = 0;
float lastCourse = 0;
bool courseKnown = false;
bool alarmActive = false;
char alarmName[BOAT_NAME_LEN] = "";
String apSsid, hostName;
bool staTrying = false;
unsigned long staStart = 0, staNextTry = 0;

// ============================================
// Helpers
// ============================================

bool gpsFix() {
  return gps.location.isValid() && gps.location.age() < 5000;
}

bool moving() {
  return gps.speed.isValid() && gps.speed.knots() >= MOVING_MIN_KNOTS;
}

// Heading = GPS course. When stopped the GPS course is noise, so the last good one is kept.
float heading() {
  if(gps.course.isValid() && moving()) {
    lastCourse = gps.course.deg();
    courseKnown = true;
  }
  return lastCourse;
}

// Names travel inside JSON: keep only plain printable characters
void sanitizeName(char* name) {
  for(char* c = name; *c; c++) {
    if(*c < ' ' || *c > '~' || *c == '"' || *c == '\\' || *c == '<' || *c == '>' || *c == '&') *c = '?';
  }
}

// Text typed by the user (WiFi names) placed inside JSON
String jsonEscape(const char* text) {
  String out;
  for(const char* c = text; *c; c++) {
    if(*c == '"' || *c == '\\') { out += '\\'; out += *c; }
    else if((uint8_t)*c >= ' ') out += *c;
  }
  return out;
}

bool staConnected() {
  return config.net == NET_BOAT && WiFi.status() == WL_CONNECTED;
}

float batteryVolts() {
  digitalWrite(PIN_BAT_CTRL, LOW);
  delay(2);
  uint32_t sum = 0;
  for(int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);
  digitalWrite(PIN_BAT_CTRL, HIGH);
  return sum / 8 * 4.9 / 1000.0; // the divider on the board is 390k/100k
}

// ============================================
// Configuration
// ============================================

void loadConfig() {
  prefs.begin("navplotter", true);
  prefs.getString("name", config.name, sizeof(config.name));
  config.type = prefs.getUChar("type", NODE_TYPE_BOAT);
  config.alarmRangeNm = prefs.getUChar("range", 3);
  prefs.getString("lang", config.lang, sizeof(config.lang));
  config.tiles = prefs.getUChar("tiles", 0);
  config.sound = prefs.getBool("sound", true);
  String pass = prefs.getString("pass", "navplotter");
  config.net = prefs.getUChar("net", NET_DIRECT);
  prefs.getString("stassid", config.staSsid, sizeof(config.staSsid));
  prefs.getString("stapass", config.staPass, sizeof(config.staPass));
  prefs.end();
  if(config.net > NET_BOAT) config.net = NET_DIRECT;

  if(config.name[0] == '\0') snprintf(config.name, sizeof(config.name), "NODE-%04X", (unsigned)(myId & 0xFFFF));
  sanitizeName(config.name);
  if(config.type > NODE_TYPE_OBJECT) config.type = NODE_TYPE_BOAT;
  if(config.alarmRangeNm != 3 && config.alarmRangeNm != 6 && config.alarmRangeNm != 12) config.alarmRangeNm = 3;
  if(strcmp(config.lang, "it") != 0) strcpy(config.lang, "en");
  if(config.tiles > 2) config.tiles = 0;
  if(pass.length() < 8 || pass.length() > 32) pass = "navplotter";
  strcpy(config.wifiPass, pass.c_str());
}

void saveConfig() {
  prefs.begin("navplotter", false);
  prefs.putString("name", config.name);
  prefs.putUChar("type", config.type);
  prefs.putUChar("range", config.alarmRangeNm);
  prefs.putString("lang", config.lang);
  prefs.putUChar("tiles", config.tiles);
  prefs.putBool("sound", config.sound);
  prefs.putString("pass", config.wifiPass);
  prefs.putUChar("net", config.net);
  prefs.putString("stassid", config.staSsid);
  prefs.putString("stapass", config.staPass);
  prefs.end();
}

// ============================================
// GPS
// ============================================

// Finds the GPS module speed by trying the common ones
void detectGpsBaud() {
  const uint32_t bauds[] = {9600, 38400, 115200, 4800, 57600};
  for(uint32_t baud : bauds) {
    Serial1.end();
    Serial1.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    unsigned long start = millis();
    int dollars = 0, printable = 0, total = 0;
    while(millis() - start < 1200) {
      while(Serial1.available()) {
        char c = Serial1.read();
        total++;
        if(c == '$') dollars++;
        if((c >= ' ' && c <= '~') || c == '\r' || c == '\n') printable++;
      }
    }
    if(dollars >= 2 && printable > total * 9 / 10) {
      gpsBaud = baud;
      Serial.printf("GPS found at %lu baud\n", (unsigned long)baud);
      return;
    }
  }
  Serial.println("GPS not found on pins 47/48");
}

void readGps() {
  static unsigned long lastRetry = 0;
  while(Serial1.available()) gps.encode(Serial1.read());
  // No module found at boot: try again every now and then (it may have been plugged in later)
  if(!gpsBaud && millis() - lastRetry > 60000) {
    lastRetry = millis();
    detectGpsBaud();
  }
}

// ============================================
// LoRa traffic
// ============================================

void IRAM_ATTR onRadioIrq() {
  radioIrq = true;
}

void initRadio() {
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  // The Heltec V3 powers the radio TCXO from DIO3 at 1.8 V and switches the antenna with DIO2
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_POWER_DBM, LORA_PREAMBLE, 1.8);
  if(state == RADIOLIB_ERR_NONE) {
    radio.setDio2AsRfSwitch(true);
    radio.setCRC(2);
    radio.setDio1Action(onRadioIrq);
    radioOk = radio.startReceive() == RADIOLIB_ERR_NONE;
  }
  nextTx = millis() + 5000 + random(0, 3000);
  Serial.printf("LoRa %s: %.3f MHz, SF%d, %d dBm\n", radioOk ? "ready" : "NOT available", LORA_FREQ, LORA_SF, LORA_POWER_DBM);
}

void sendPosition() {
  TrafficPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.magic[0] = 'N'; pkt.magic[1] = 'P';
  pkt.version = 1;
  pkt.ttl = MESH_TTL;
  pkt.id = myId;
  pkt.seq = seq++;
  pkt.flags = (config.type & TRAFFIC_TYPE_MASK) << TRAFFIC_TYPE_SHIFT;
  pkt.lat = (int32_t)lround(gps.location.lat() * 1e7);
  pkt.lon = (int32_t)lround(gps.location.lng() * 1e7);
  pkt.cog = (uint16_t)lround(heading() * 10);
  pkt.sog = moving() ? (uint16_t)lround(gps.speed.knots() * 100) : 0;
  strncpy(pkt.name, config.name, BOAT_NAME_LEN - 1);

  if(radio.startTransmit((uint8_t*)&pkt, sizeof(pkt)) == RADIOLIB_ERR_NONE) {
    transmitting = true;
    txIsRelay = false;
    txStart = millis();
  } else {
    radio.startReceive();
  }
  // A little random delay keeps two nodes from always transmitting at the same instant
  nextTx = millis() + (moving() ? LORA_TX_MOVING_MS : LORA_TX_STOPPED_MS) + random(0, 2000);
}

void processPacket(const TrafficPacket& pkt, float rssi) {
  if(pkt.magic[0] != 'N' || pkt.magic[1] != 'P' || pkt.version != 1 || pkt.id == myId) return;

  // Mesh: drop copies already received (and if somebody else relayed it, we do not need to)
  if(meshAlreadySeen(pkt.id, pkt.seq)) {
    meshCancelRelay(pkt.id, pkt.seq);
    return;
  }
  rxCount++;
  meshQueueRelay(pkt);

  Target* t = nullptr;
  for(auto& existing : targets) {
    if(existing.id == pkt.id) t = &existing;
  }
  if(!t) {
    if((int)targets.size() >= MAX_TARGETS) return;
    targets.push_back(Target());
    t = &targets.back();
    t->id = pkt.id;
  }
  strncpy(t->name, pkt.name, BOAT_NAME_LEN - 1);
  t->name[BOAT_NAME_LEN - 1] = '\0';
  sanitizeName(t->name);
  t->type = (pkt.flags >> TRAFFIC_TYPE_SHIFT) & TRAFFIC_TYPE_MASK;
  if(t->type > NODE_TYPE_OBJECT) t->type = NODE_TYPE_BOAT;
  t->simulated = pkt.flags & TRAFFIC_FLAG_SIMULATED;
  t->lat = pkt.lat / 1e7;
  t->lon = pkt.lon / 1e7;
  t->cog = pkt.cog / 10.0;
  t->sog = pkt.sog / 100.0;
  t->lastSeen = millis();
  t->rssi = rssi;
  t->relayed = pkt.ttl < MESH_TTL;
  Serial.printf("RX %s%s %.5f %.5f %.1f kn %.0f deg, RSSI %.0f\n", t->name, t->relayed ? " (relayed)" : "",
    t->lat, t->lon, t->sog, t->cog, rssi);
}

void handleRadio() {
  if(!radioOk) return;

  if(radioIrq) {
    radioIrq = false;
    if(transmitting) {
      radio.finishTransmit();
      transmitting = false;
      if(txIsRelay) relayCount++;
      else txCount++;
    } else {
      TrafficPacket pkt;
      if(radio.getPacketLength() == sizeof(pkt) && radio.readData((uint8_t*)&pkt, sizeof(pkt)) == RADIOLIB_ERR_NONE) {
        processPacket(pkt, radio.getRSSI());
      }
    }
    radio.startReceive();
  }

  // Transmission stuck: go back to receiving
  if(transmitting && millis() - txStart > 3000) {
    radio.finishTransmit();
    transmitting = false;
    radio.startReceive();
  }

  if(!transmitting && (long)(millis() - nextTx) >= 0) {
    if(gpsFix()) sendPosition();
    else nextTx = millis() + 2000;
  }

  // Mesh: relay packets from other nodes that still have ttl
  TrafficPacket relay;
  if(!transmitting && meshNextRelay(relay)) {
    if(radio.startTransmit((uint8_t*)&relay, sizeof(relay)) == RADIOLIB_ERR_NONE) {
      transmitting = true;
      txIsRelay = true;
      txStart = millis();
    } else {
      radio.startReceive();
    }
  }
}

// Distance, bearing, closest point of approach (CPA) and time to it (TCPA), on a local flat plane in metres
void updateTarget(Target& t) {
  t.cpaValid = false;
  t.alarm = false;
  if(!gpsFix()) return;

  const double KN = 0.514444; // knots -> m/s
  bool stale = millis() - t.lastSeen > TARGET_STALE_MS;
  double age = (millis() - t.lastSeen) / 1000.0;
  double lat0 = gps.location.lat() * DEG_TO_RAD;

  double tgtSog = t.sog * KN, tgtCog = t.cog * DEG_TO_RAD;
  // Relative position; a fresh target is moved forward by the time since its last packet
  double rx = (t.lon - gps.location.lng()) * 111320.0 * cos(lat0) + (stale ? 0 : tgtSog * sin(tgtCog) * age);
  double ry = (t.lat - gps.location.lat()) * 110540.0 + (stale ? 0 : tgtSog * cos(tgtCog) * age);
  t.distNm = hypot(rx, ry) / 1852.0;
  double brg = atan2(rx, ry) * RAD_TO_DEG;
  t.brg = brg < 0 ? brg + 360 : brg;
  if(stale) return;

  double ownSog = moving() ? gps.speed.knots() * KN : 0;
  double ownCog = heading() * DEG_TO_RAD;
  double vx = tgtSog * sin(tgtCog) - ownSog * sin(ownCog);
  double vy = tgtSog * cos(tgtCog) - ownSog * cos(ownCog);
  double v2 = vx * vx + vy * vy;
  if(v2 < 0.06) return; // relative speed under half a knot: nothing is closing in
  double tcpa = -(rx * vx + ry * vy) / v2;
  if(tcpa < 0) return;  // moving apart

  t.cpaValid = true;
  t.tcpaS = tcpa;
  t.cpaNm = hypot(rx + vx * tcpa, ry + vy * tcpa) / 1852.0;
  t.alarm = t.distNm <= config.alarmRangeNm && t.cpaNm <= CPA_ALARM_NM && t.tcpaS <= CPA_ALARM_S;
}

// Once a second: drop lost targets and recompute approaches
void updateTraffic() {
  static unsigned long last = 0;
  if(millis() - last < 1000) return;
  last = millis();

  for(auto it = targets.begin(); it != targets.end(); ) {
    if(millis() - it->lastSeen > TARGET_EXPIRE_MS) it = targets.erase(it);
    else ++it;
  }
  const Target* worst = nullptr;
  for(auto& t : targets) {
    updateTarget(t);
    if(t.alarm && (!worst || t.tcpaS < worst->tcpaS)) worst = &t;
  }
  alarmActive = worst != nullptr;
  if(worst) strcpy(alarmName, worst->name);
}

// ============================================
// Web server
// ============================================

void sendJson(const String& json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleRoot() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

String configJson() {
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"type\":%u,\"alarmRange\":%u,\"lang\":\"%s\",\"tiles\":%u,\"sound\":%s,\"id\":\"%08lX\",\"version\":\"%s\",",
    config.name, config.type, config.alarmRangeNm, config.lang, config.tiles, config.sound ? "true" : "false",
    (unsigned long)myId, FIRMWARE_VERSION);
  String json = buf;
  // The boat WiFi password is never sent back, only whether one is stored
  json += "\"net\":" + String(config.net) + ",\"staSsid\":\"" + jsonEscape(config.staSsid) + "\",\"staPassSet\":" +
    (config.staPass[0] ? "true" : "false") + ",\"ssid\":\"" + jsonEscape(apSsid.c_str()) + "\",\"host\":\"" + hostName + ".local\"}";
  return json;
}

void handleGetConfig() {
  sendJson(configJson());
}

void handlePostConfig() {
  bool restartWifi = false;
  if(server.hasArg("name")) {
    String n = server.arg("name");
    n.trim();
    if(n.length() > 0) {
      strncpy(config.name, n.c_str(), BOAT_NAME_LEN - 1);
      config.name[BOAT_NAME_LEN - 1] = '\0';
      sanitizeName(config.name);
    }
  }
  if(server.hasArg("type")) {
    int v = server.arg("type").toInt();
    if(v >= NODE_TYPE_BOAT && v <= NODE_TYPE_OBJECT) config.type = v;
  }
  if(server.hasArg("alarmRange")) {
    int v = server.arg("alarmRange").toInt();
    if(v == 3 || v == 6 || v == 12) config.alarmRangeNm = v;
  }
  if(server.hasArg("lang")) {
    strcpy(config.lang, server.arg("lang") == "it" ? "it" : "en");
  }
  if(server.hasArg("tiles")) {
    int v = server.arg("tiles").toInt();
    if(v >= 0 && v <= 2) config.tiles = v;
  }
  if(server.hasArg("sound")) config.sound = server.arg("sound") == "1";
  if(server.hasArg("net")) {
    int v = server.arg("net").toInt();
    if(v >= NET_OFFGRID && v <= NET_BOAT && v != config.net) {
      config.net = v;
      restartWifi = true;
    }
  }
  if(server.hasArg("staSsid")) {
    String v = server.arg("staSsid");
    v.trim();
    if(v.length() <= 32 && v != config.staSsid) {
      strcpy(config.staSsid, v.c_str());
      if(config.net == NET_BOAT) restartWifi = true;
    }
  }
  // An empty password field means "keep the stored one"
  if(server.hasArg("staPass") && server.arg("staPass").length() > 0) {
    String v = server.arg("staPass");
    if(v.length() <= 64 && v != config.staPass) {
      strcpy(config.staPass, v.c_str());
      if(config.net == NET_BOAT) restartWifi = true;
    }
  }
  if(server.hasArg("wifiPass")) {
    String p = server.arg("wifiPass");
    if(p.length() >= 8 && p.length() <= 32 && p != config.wifiPass) {
      strcpy(config.wifiPass, p.c_str());
      restartWifi = true;
    }
  }
  saveConfig();
  String json = configJson();
  json.remove(json.length() - 1);
  json += restartWifi ? ",\"restart\":true}" : ",\"restart\":false}";
  sendJson(json);
  if(restartWifi) {
    // Network changes apply after a restart
    delay(500);
    ESP.restart();
  }
}

void handleState() {
  String json;
  json.reserve(512 + targets.size() * 260);
  char buf[320];

  bool fix = gpsFix();
  float hdg = heading();
  snprintf(buf, sizeof(buf),
    "{\"own\":{\"fix\":%s,\"lat\":%.7f,\"lon\":%.7f,\"sog\":%.2f,\"cog\":%.1f,\"cogValid\":%s,\"moving\":%s,\"sats\":%d,\"hdop\":%.1f,\"time\":\"%02d:%02d:%02d\"},",
    fix ? "true" : "false", fix ? gps.location.lat() : 0.0, fix ? gps.location.lng() : 0.0,
    gps.speed.isValid() ? gps.speed.knots() : 0.0, hdg, courseKnown ? "true" : "false", moving() ? "true" : "false",
    gps.satellites.isValid() ? (int)gps.satellites.value() : 0, gps.hdop.isValid() ? gps.hdop.hdop() : 99.9,
    gps.time.isValid() ? gps.time.hour() : 0, gps.time.isValid() ? gps.time.minute() : 0, gps.time.isValid() ? gps.time.second() : 0);
  json += buf;

  json += "\"targets\":[";
  bool first = true;
  for(const auto& t : targets) {
    snprintf(buf, sizeof(buf),
      "%s{\"id\":\"%08lX\",\"name\":\"%s\",\"type\":%u,\"sim\":%s,\"lat\":%.7f,\"lon\":%.7f,\"sog\":%.2f,\"cog\":%.1f,\"age\":%lu,"
      "\"rssi\":%.0f,\"relayed\":%s,\"stale\":%s,\"dist\":%.3f,\"brg\":%.0f,\"cpaValid\":%s,\"cpa\":%.3f,\"tcpa\":%.0f,\"alarm\":%s}",
      first ? "" : ",", (unsigned long)t.id, t.name, t.type, t.simulated ? "true" : "false", t.lat, t.lon, t.sog, t.cog,
      (millis() - t.lastSeen) / 1000, t.rssi, t.relayed ? "true" : "false",
      millis() - t.lastSeen > TARGET_STALE_MS ? "true" : "false", t.distNm, t.brg,
      t.cpaValid ? "true" : "false", t.cpaNm, t.tcpaS, t.alarm ? "true" : "false");
    json += buf;
    first = false;
  }
  json += "],";

  snprintf(buf, sizeof(buf),
    "\"alarm\":%s,\"alarmName\":\"%s\",\"lora\":{\"ok\":%s,\"tx\":%lu,\"rx\":%lu,\"relay\":%lu,\"freq\":%.3f},"
    "\"gpsBaud\":%lu,\"sd\":%s,\"battery\":%.2f,\"uptime\":%lu,\"sta\":%s,\"staIp\":\"%s\",\"apIp\":\"%s\"}",
    alarmActive ? "true" : "false", alarmActive ? alarmName : "", radioOk ? "true" : "false",
    (unsigned long)txCount, (unsigned long)rxCount, (unsigned long)relayCount, LORA_FREQ,
    (unsigned long)gpsBaud, sdOk ? "true" : "false", batteryVolts(), millis() / 1000,
    staConnected() ? "true" : "false", staConnected() ? WiFi.localIP().toString().c_str() : "",
    WiFi.softAPIP().toString().c_str());
  json += buf;
  sendJson(json);
}

// Waypoints and other data the web app keeps: stored as one JSON document on the internal flash
void handleGetUserdata() {
  if(fsOk && LittleFS.exists(USERDATA_FILE)) {
    File f = LittleFS.open(USERDATA_FILE, "r");
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(f, "application/json");
    f.close();
  } else {
    sendJson("{}");
  }
}

void handlePostUserdata() {
  String body = server.arg("plain");
  if(!fsOk || body.length() == 0 || body.length() > USERDATA_MAX || body[0] != '{') {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  File f = LittleFS.open(USERDATA_FILE, "w");
  bool ok = f && f.print(body) == body.length();
  if(f) f.close();
  server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// Chart tiles from the SD card: /tiles/z/x/y.jpg, same layout as the T-Deck card
void handleNotFound() {
  String uri = server.uri();
  if(sdOk && uri.startsWith("/tiles/") && uri.endsWith(".jpg") && uri.indexOf("..") < 0 && uri.length() < 48) {
    File f = SD.open(uri);
    if(f && !f.isDirectory()) {
      server.sendHeader("Cache-Control", "max-age=86400");
      server.streamFile(f, "image/jpeg");
      f.close();
      return;
    }
    if(f) f.close();
  }
  server.send(404, "text/plain", "Not found");
}

// Starts (or restarts) the attempt to join the boat WiFi
void startSta() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(hostName.c_str());
  WiFi.begin(config.staSsid, config.staPass);
  staTrying = true;
  staStart = millis();
  Serial.printf("Joining boat WiFi \"%s\"...\n", config.staSsid);
}

// Boat WiFi: while it is being searched the node's own network gets unreliable, so the search is
// stopped after a while and repeated every few minutes
void handleNetwork() {
  if(config.net != NET_BOAT || config.staSsid[0] == '\0') return;
  static bool wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;

  if(connected && !wasConnected) {
    staTrying = false;
    Serial.printf("Boat WiFi joined: http://%s or http://%s.local\n", WiFi.localIP().toString().c_str(), hostName.c_str());
  }
  if(!connected && wasConnected) staNextTry = millis() + 5000; // connection lost: try again shortly
  wasConnected = connected;
  if(connected) return;

  if(staTrying && millis() - staStart > STA_CONNECT_MS) {
    staTrying = false;
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
    staNextTry = millis() + STA_RETRY_MS;
    Serial.println("Boat WiFi not reachable, only the node WiFi is available");
  } else if(!staTrying && (long)(millis() - staNextTry) >= 0) {
    startSta();
  }
}

void initWeb() {
  apSsid = String("NavPlotter-") + config.name;
  apSsid.replace(" ", "_");
  // Name on the boat network: http://<node name>.local
  hostName = "";
  for(const char* c = config.name; *c; c++) {
    if(isalnum((unsigned char)*c)) hostName += (char)tolower((unsigned char)*c);
    else if(hostName.length() && !hostName.endsWith("-")) hostName += '-';
  }
  while(hostName.endsWith("-")) hostName.remove(hostName.length() - 1);
  if(hostName.length() == 0) hostName = "navplotter";

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), config.wifiPass);
  Serial.printf("WiFi network %s, open http://%s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  if(config.net == NET_BOAT && config.staSsid[0]) startSta();
  MDNS.begin(hostName.c_str());
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/userdata", HTTP_GET, handleGetUserdata);
  server.on("/api/userdata", HTTP_POST, handlePostUserdata);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ============================================
// OLED status
// ============================================

void drawDisplay() {
  static unsigned long last = 0;
  if(millis() - last < 1000) return;
  last = millis();

  const char* types[] = {"boat", "person", "object"};
  char line[32];
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  snprintf(line, sizeof(line), "%s (%s)", config.name, types[config.type]);
  oled.drawStr(0, 9, line);
  if(staConnected()) {
    // On the boat network: show how to reach the node from there
    snprintf(line, sizeof(line), "%s.local", hostName.c_str());
    oled.drawStr(0, 20, line);
    snprintf(line, sizeof(line), "http://%s", WiFi.localIP().toString().c_str());
    oled.drawStr(0, 31, line);
  } else {
    oled.drawStr(0, 20, apSsid.c_str());
    snprintf(line, sizeof(line), "http://%s%s", WiFi.softAPIP().toString().c_str(), config.net == NET_BOAT ? " *" : "");
    oled.drawStr(0, 31, line);
  }
  if(!gpsBaud) snprintf(line, sizeof(line), "GPS: not found");
  else if(gpsFix()) snprintf(line, sizeof(line), "GPS: fix, %d sat", gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
  else snprintf(line, sizeof(line), "GPS: waiting for fix");
  oled.drawStr(0, 42, line);
  snprintf(line, sizeof(line), "LoRa %s T%lu R%lu", radioOk ? "ok" : "KO", (unsigned long)txCount, (unsigned long)rxCount);
  oled.drawStr(0, 53, line);
  if(alarmActive) snprintf(line, sizeof(line), "ALARM: %s", alarmName);
  else snprintf(line, sizeof(line), "Nodes: %d  WiFi: %d", (int)targets.size(), WiFi.softAPgetStationNum());
  oled.drawStr(0, 64, line);
  oled.sendBuffer();
}

// ============================================
// Setup and loop
// ============================================

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_BAT_CTRL, OUTPUT);
  digitalWrite(PIN_BAT_CTRL, HIGH);
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  delay(50);
  oled.begin();
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 12, "NavPlotter " FIRMWARE_VERSION);
  oled.drawStr(0, 30, "Starting...");
  oled.sendBuffer();

  uint64_t mac = ESP.getEfuseMac();
  myId = (uint32_t)(mac >> 16) ^ (uint32_t)mac;
  loadConfig();
  Serial.printf("NavPlotter %s, node %s, id %08lX\n", FIRMWARE_VERSION, config.name, (unsigned long)myId);

  fsOk = LittleFS.begin(true);
  if(!fsOk) Serial.println("Internal storage not available");

  // Optional SD card with chart tiles
  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOk = SD.begin(PIN_SD_CS, sdSpi, 20000000U);
  Serial.println(sdOk ? "SD card ready" : "SD card not present");

  initRadio();
  initWeb();
  detectGpsBaud();
}

void loop() {
  readGps();
  handleRadio();
  updateTraffic();
  handleNetwork();
  server.handleClient();
  drawDisplay();
}
