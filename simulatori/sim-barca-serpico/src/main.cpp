// Simulatore di barca per il plotter T-Deck - Heltec WiFi LoRa 32 V3
// Prende un punto di riferimento (il proprio GPS, oppure la posizione ricevuta dal plotter), si mette a una
// certa distanza da lì e poi "naviga" trasmettendo nome, posizione, rotta e velocità come farebbe un'altra
// barca con il nostro firmware. Il tasto PRG cambia scenario; l'ultimo trasmette la posizione GPS vera.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <TinyGPS++.h>
#include <math.h>
#include "traffic_packet.h"

// ---- Da personalizzare ----
#define SIM_BOAT_NAME "Serpico"   // nome mostrato sul plotter (massimo 11 caratteri)
#define SIM_TX_POWER_DBM 2        // potenza bassa: basta per le prove sul tavolo; in mare fino a 22
#define SIM_TX_INTERVAL_MS 10000UL

// ---- Pin della Heltec WiFi LoRa 32 V3 ----
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
#define PIN_VEXT 36   // LOW = accende l'alimentazione del display
#define PIN_BUTTON 0  // tasto PRG
#define PIN_GPS_TX 47 // TX della Heltec -> RX del modulo GPS
#define PIN_GPS_RX 48 // RX della Heltec <- TX del modulo GPS

// ---- Scenari ----
// Posizione di partenza rispetto al plotter (distanza e rilevamento), poi rotta e velocità.
// "aim" = punta verso il plotter passando a "missNm" miglia da lui.
struct Scenario {
  const char* name;
  double startNm;     // distanza iniziale dal plotter
  double startBrg;    // rilevamento iniziale dal plotter, gradi
  double speedKn;
  double missNm;      // distanza minima di passaggio (0 = rotta di collisione)
};
const Scenario scenarios[] = {
  {"COLLISIONE", 0.60, 315, 5.0, 0.00},   // deve far scattare l'allarme di abbordo
  {"PASSA A 0.3", 0.60, 315, 5.0, 0.30},  // passa largo: nessun allarme
  {"FERMA A 0.2", 0.20, 270, 0.0, 0.00},  // barca ferma vicina: nessun allarme
  {"GPS REALE", 0, 0, 0, 0},             // nessuna simulazione: posizione, rotta e velocità del GPS
};
const int scenarioCount = sizeof(scenarios) / sizeof(scenarios[0]);
#define SCENARIO_REAL_GPS (scenarioCount - 1)

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);

TinyGPSPlus gps;
uint32_t gpsBaud = 0;       // 0 = nessun dato dal GPS
unsigned long gpsChars = 0;

bool radioOk = false;
volatile bool radioIrq = false;
bool transmitting = false;
unsigned long txStart = 0, nextTx = 0;
uint32_t txCount = 0, rxCount = 0, relayCount = 0;
bool txIsRelay = false;
// Prova della mesh: si accettano solo i pacchetti arrivati tramite un ripetitore, come se le barche
// fossero fuori portata diretta. Si attiva tenendo premuto PRG per due secondi.
bool onlyRelayed = false;
uint32_t myId = 0;
uint8_t seq = 0;

// Punto di riferimento della simulazione: il proprio GPS se ha il fix, altrimenti la posizione del plotter
bool haveRef = false;
double refLat = 0, refLon = 0;

// Plotter: ultima posizione ricevuta
bool havePlotter = false;
double plotterLat = 0, plotterLon = 0;
char plotterName[BOAT_NAME_LEN] = "";
float plotterRssi = 0;
unsigned long plotterSeen = 0;

// Barca simulata
int scenario = 0;
bool simStarted = false;
double simLat = 0, simLon = 0, simCog = 0, simSog = 0;
unsigned long lastStep = 0;
unsigned long scenarioStart = 0;

void IRAM_ATTR onRadioIrq() {
  radioIrq = true;
}

// Sposta un punto di "nm" miglia lungo il rilevamento "brg" (piano locale: basta per poche miglia)
void movePoint(double& lat, double& lon, double brg, double nm) {
  double m = nm * 1852.0, r = radians(brg);
  lat += m * cos(r) / 110540.0;
  lon += m * sin(r) / (111320.0 * cos(radians(lat)));
}

double distanceNm(double lat1, double lon1, double lat2, double lon2) {
  double dy = (lat2 - lat1) * 110540.0;
  double dx = (lon2 - lon1) * 111320.0 * cos(radians(lat1));
  return hypot(dx, dy) / 1852.0;
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  double dy = (lat2 - lat1) * 110540.0;
  double dx = (lon2 - lon1) * 111320.0 * cos(radians(lat1));
  double b = degrees(atan2(dx, dy));
  return b < 0 ? b + 360 : b;
}

// Mette la barca simulata alla partenza dello scenario, rispetto alla posizione attuale del plotter
void startScenario() {
  const Scenario& s = scenarios[scenario];
  simLat = refLat;
  simLon = refLon;
  movePoint(simLat, simLon, s.startBrg, s.startNm);
  simSog = s.speedKn;
  // Rotta verso il plotter, deviata di quanto serve per passare a missNm da lui
  double toPlotter = fmod(s.startBrg + 180.0, 360.0);
  double offset = (s.missNm > 0 && s.missNm < s.startNm) ? degrees(asin(s.missNm / s.startNm)) : 0;
  simCog = fmod(toPlotter + offset, 360.0);
  simStarted = true;
  lastStep = millis();
  scenarioStart = millis();
  Serial.printf("Scenario %s: parto a %.2f NM per %03.0f dal riferimento, rotta %03.0f, %.1f kt\n",
    s.name, s.startNm, s.startBrg, simCog, simSog);
}

// Legge il GPS e aggiorna il punto di riferimento
void readGps() {
  while(Serial1.available()) {
    gps.encode(Serial1.read());
    gpsChars++;
  }
  if(gps.location.isValid() && gps.location.age() < 5000) {
    refLat = gps.location.lat();
    refLon = gps.location.lng();
    if(!haveRef) {
      haveRef = true;
      Serial.printf("Fix GPS: %.5f %.5f\n", refLat, refLon);
      startScenario();
    }
  }
}

// Cerca la velocità del modulo GPS provando quelle più comuni
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
      Serial.printf("GPS trovato a %lu baud\n", (unsigned long)baud);
      return;
    }
  }
  Serial.println("GPS non trovato sui pin 47/48: uso la posizione del plotter");
}

void stepSimulation() {
  if(!simStarted) return;
  if(scenario == SCENARIO_REAL_GPS) {
    // Nessuna simulazione: si trasmette quello che dice il GPS
    simLat = refLat;
    simLon = refLon;
    bool moving = gps.speed.isValid() && gps.speed.knots() >= 0.5;
    simSog = moving ? gps.speed.knots() : 0;
    if(moving && gps.course.isValid()) simCog = gps.course.deg();
    lastStep = millis();
    return;
  }
  unsigned long now = millis();
  double dt = (now - lastStep) / 1000.0;
  lastStep = now;
  movePoint(simLat, simLon, simCog, simSog * dt / 3600.0);

  // Superato il plotter di oltre un miglio (o dopo 20 minuti): si ricomincia
  if(simSog > 0 && haveRef &&
     (distanceNm(simLat, simLon, refLat, refLon) > scenarios[scenario].startNm + 0.5 || now - scenarioStart > 1200000UL)) {
    startScenario();
  }
}

void sendPosition() {
  TrafficPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.magic[0] = 'N'; pkt.magic[1] = 'P';
  pkt.version = 1;
  pkt.ttl = MESH_TTL;
  pkt.id = myId;
  pkt.seq = seq++;
  pkt.flags = TRAFFIC_FLAG_SIMULATED;
  pkt.lat = (int32_t)lround(simLat * 1e7);
  pkt.lon = (int32_t)lround(simLon * 1e7);
  pkt.cog = (uint16_t)lround(simCog * 10);
  pkt.sog = (uint16_t)lround(simSog * 100);
  strncpy(pkt.name, SIM_BOAT_NAME, BOAT_NAME_LEN - 1);

  if(radio.startTransmit((uint8_t*)&pkt, sizeof(pkt)) == RADIOLIB_ERR_NONE) {
    transmitting = true;
    txIsRelay = false;
    txStart = millis();
  } else {
    radio.startReceive();
  }
  nextTx = millis() + SIM_TX_INTERVAL_MS + random(0, 2000);
}

void handleRadio() {
  if(!radioOk) return;

  if(radioIrq) {
    radioIrq = false;
    if(transmitting) {
      radio.finishTransmit();
      transmitting = false;
      if(txIsRelay) {
        relayCount++;
      } else {
        txCount++;
        Serial.printf("TX %lu: %.5f %.5f rotta %03.0f %.1f kt\n", (unsigned long)txCount, simLat, simLon, simCog, simSog);
      }
    } else {
      TrafficPacket pkt;
      if(radio.getPacketLength() == sizeof(pkt) && radio.readData((uint8_t*)&pkt, sizeof(pkt)) == RADIOLIB_ERR_NONE &&
         pkt.magic[0] == 'N' && pkt.magic[1] == 'P' && pkt.version == 1 && pkt.id != myId) {
        bool relayed = pkt.ttl < MESH_TTL;
        if(onlyRelayed && !relayed) {
          // Prova mesh: il pacchetto diretto si finge non ricevuto (e non va annotato come visto)
          radio.startReceive();
          return;
        }
        if(meshAlreadySeen(pkt.id, pkt.seq)) {
          meshCancelRelay(pkt.id, pkt.seq);
          radio.startReceive();
          return;
        }
        rxCount++;
        meshQueueRelay(pkt);
        if(pkt.flags & TRAFFIC_FLAG_SIMULATED) {
          // Un altro simulatore: lo si sente, ma non può fare da punto di riferimento
          Serial.printf("RX %lu da %s (simulata)%s, RSSI %.0f dBm\n", (unsigned long)rxCount, pkt.name,
            relayed ? " VIA RIPETITORE" : "", radio.getRSSI());
          radio.startReceive();
          return;
        }
        plotterLat = pkt.lat / 1e7;
        plotterLon = pkt.lon / 1e7;
        strncpy(plotterName, pkt.name, BOAT_NAME_LEN - 1);
        plotterName[BOAT_NAME_LEN - 1] = '\0';
        plotterRssi = radio.getRSSI();
        plotterSeen = millis();
        Serial.printf("RX %lu da %s%s: %.5f %.5f rotta %.0f %.1f kt, RSSI %.0f dBm\n", (unsigned long)rxCount, plotterName,
          relayed ? " VIA RIPETITORE" : "", plotterLat, plotterLon, pkt.cog / 10.0, pkt.sog / 100.0, plotterRssi);
        havePlotter = true;
        // Senza un GPS proprio, il riferimento è la posizione del plotter
        if(!(gps.location.isValid() && gps.location.age() < 5000)) {
          refLat = plotterLat;
          refLon = plotterLon;
          if(!haveRef) {
            haveRef = true;
            startScenario();
          }
        }
      }
    }
    radio.startReceive();
  }

  if(transmitting && millis() - txStart > 3000) {
    radio.finishTransmit();
    transmitting = false;
    radio.startReceive();
  }

  if(simStarted && !transmitting && (long)(millis() - nextTx) >= 0) sendPosition();

  // Mesh: ritrasmette i pacchetti delle altre barche che hanno ancora ttl
  TrafficPacket relay;
  if(!transmitting && meshNextRelay(relay)) {
    if(radio.startTransmit((uint8_t*)&relay, sizeof(relay)) == RADIOLIB_ERR_NONE) {
      transmitting = true;
      txIsRelay = true;
      txStart = millis();
      Serial.printf("RIPETO %s (seq %u, ttl %u)\n", relay.name, relay.seq, relay.ttl);
    } else {
      radio.startReceive();
    }
  }
}

void handleButton() {
  static bool last = HIGH;
  static unsigned long lastChange = 0;
  bool v = digitalRead(PIN_BUTTON);
  if(v != last && millis() - lastChange > 50) {
    unsigned long held = millis() - lastChange;
    last = v;
    lastChange = millis();
    if(v == HIGH) {
      // Al rilascio: pressione lunga = prova mesh, breve = scenario successivo
      if(held >= 1500) {
        onlyRelayed = !onlyRelayed;
        Serial.println(onlyRelayed ? "Prova mesh: accetto solo pacchetti ripetuti" : "Prova mesh spenta");
      } else {
        scenario = (scenario + 1) % scenarioCount;
        if(haveRef) startScenario();
      }
    }
  }
}

void drawDisplay() {
  static unsigned long lastDraw = 0;
  if(millis() - lastDraw < 500) return;
  lastDraw = millis();

  char line[32];
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  snprintf(line, sizeof(line), "%s %s%s", SIM_BOAT_NAME, radioOk ? "" : "RADIO KO ", onlyRelayed ? "SOLO RIPET." : "");
  oled.drawStr(0, 9, line);
  snprintf(line, sizeof(line), "%d/%d %s", scenario + 1, scenarioCount, scenarios[scenario].name);
  oled.drawStr(0, 20, line);

  if(!haveRef) {
    oled.drawStr(0, 32, gpsBaud ? "In attesa del fix GPS" : "GPS non trovato");
    oled.drawStr(0, 43, "o di un pacchetto");
    oled.drawStr(0, 54, "dal plotter");
  } else {
    double nm = distanceNm(refLat, refLon, simLat, simLon);
    double brg = bearingDeg(refLat, refLon, simLat, simLon);
    if(scenario == SCENARIO_REAL_GPS) snprintf(line, sizeof(line), "%.5f %.5f", simLat, simLon);
    else snprintf(line, sizeof(line), "Dist %.2fNM ril %03.0f", nm, brg);
    oled.drawStr(0, 32, line);
    snprintf(line, sizeof(line), "Rotta %03.0f  %.1f kt", simCog, simSog);
    oled.drawStr(0, 43, line);
    if(havePlotter) snprintf(line, sizeof(line), "%s %.0fdBm %lus", plotterName, plotterRssi, (millis() - plotterSeen) / 1000);
    else snprintf(line, sizeof(line), "plotter non ricevuto");
    oled.drawStr(0, 54, line);
  }
  snprintf(line, sizeof(line), "T%lu R%lu rip%lu GPS %s", (unsigned long)txCount, (unsigned long)rxCount, (unsigned long)relayCount,
    !gpsBaud ? "no" : (gps.location.isValid() && gps.location.age() < 5000) ? "fix" : "..");
  oled.drawStr(0, 64, line);
  oled.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW); // accende il display
  delay(50);
  oled.begin();

  uint64_t mac = ESP.getEfuseMac();
  myId = (uint32_t)(mac >> 16) ^ (uint32_t)mac;

  // Stessi parametri radio del plotter. La Heltec V3 alimenta il TCXO da DIO3 a 1,8 V e usa DIO2 per l'antenna.
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, SIM_TX_POWER_DBM, LORA_PREAMBLE, 1.8);
  if(state == RADIOLIB_ERR_NONE) {
    radio.setDio2AsRfSwitch(true);
    radio.setCRC(2);
    radio.setDio1Action(onRadioIrq);
    radioOk = radio.startReceive() == RADIOLIB_ERR_NONE;
  }
  Serial.printf("Simulatore %s, id %08lX, radio %s (%.3f MHz, SF%d, %d dBm)\n", SIM_BOAT_NAME, (unsigned long)myId,
    radioOk ? "pronta" : "NON disponibile", LORA_FREQ, LORA_SF, SIM_TX_POWER_DBM);
  nextTx = millis() + 2000;

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 12, SIM_BOAT_NAME);
  oled.drawStr(0, 30, "Cerco il GPS...");
  oled.sendBuffer();
  detectGpsBaud();
}

void loop() {
  readGps();
  handleButton();
  stepSimulation();
  handleRadio();
  drawDisplay();
}
