/*
  Navigate IMU - TRASMETTITORE ESP-NOW
  Scheda: ESP32-WROOM-32 ("ESP32 Dev Module")
  Sensore: BNO085 / BNO08x su I2C

  Legge prua magnetica, beccheggio, rollio e velocita' di accostata dal BNO08x
  e li trasmette in ESP-NOW "broadcast" (a tutti): qualsiasi ricevitore sullo
  stesso canale li riceve (T-Deck, ponte per Raspberry Pi, altri ESP32).

  I dati viaggiano gia' come frasi NMEA 0183, cosi' piu' avanti il Raspberry
  li puo' passare direttamente a OpenCPN / Signal K / Navplotter-Linux:
    $HCHDM,231.4,M*hh                      prua magnetica
    $IIXDR,A,2.3,D,PITCH,A,-12.1,D,ROLL*hh beccheggio e rollio (gradi)
    $TIROT,35.2,A*hh                       accostata (gradi/minuto, + = a dritta)
    $PNVIMU,1234,3*hh                      contatore pacchetti, calibrazione 0-3

  Collegamenti BNO08x -> ESP32:
    VIN -> 3V3      GND -> GND      SDA -> GPIO21      SCL -> GPIO22

  Librerie (Gestore librerie Arduino): "Adafruit BNO08x" (con le dipendenze)
  Pacchetto schede: esp32 di Espressif, versione 3.x
*/

#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <Adafruit_BNO08x.h>

// ------------------ IMPOSTAZIONI ------------------
#define CANALE_ESPNOW   1       // deve essere uguale su trasmettitore e ricevitori
#define PIN_SDA         21
#define PIN_SCL         22
#define INDIRIZZO_BNO   0x4A    // 0x4A schede Adafruit, 0x4B schede SparkFun
#define FREQUENZA_HZ    10      // pacchetti al secondo

const float OFFSET_PRUA  = 0.0;   // gradi da sommare se il sensore non e' allineato alla prua
const bool  INVERTI_PRUA = false; // true se la prua aumenta girando a sinistra
// --------------------------------------------------

Adafruit_BNO08x bno(-1);
sh2_SensorValue_t ev;
uint8_t indirizzoBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

float qr = 1, qi = 0, qj = 0, qk = 0;  // quaternione
float gz = 0;                          // velocita' angolare asse verticale (rad/s)
uint8_t calibrazione = 0;
bool datiValidi = false;
uint32_t contatore = 0, ultimoInvio = 0, ultimoDato = 0;

// Aggiunge una frase NMEA con checksum al buffer
int aggiungiFrase(char *buf, int pos, int cap, const char *corpo) {
  uint8_t ck = 0;
  for (const char *c = corpo; *c; c++) ck ^= (uint8_t)*c;
  return pos + snprintf(buf + pos, cap - pos, "$%s*%02X\r\n", corpo, ck);
}

void attivaReport() {
  bno.enableReport(SH2_ROTATION_VECTOR, 20000);       // 50 Hz, con magnetometro
  bno.enableReport(SH2_GYROSCOPE_CALIBRATED, 20000);  // 50 Hz
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nNavigate IMU - trasmettitore ESP-NOW");

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  while (!bno.begin_I2C(INDIRIZZO_BNO, &Wire)) {
    Serial.println("BNO08x non trovato: controlli i fili e l'indirizzo (0x4A/0x4B). Riprovo...");
    delay(2000);
  }
  Serial.println("BNO08x trovato.");
  attivaReport();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(CANALE_ESPNOW, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Errore avvio ESP-NOW, riavvio...");
    delay(2000);
    ESP.restart();
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, indirizzoBroadcast, 6);
  peer.channel = CANALE_ESPNOW;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.print("MAC di questa scheda: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("Trasmetto sul canale %d, %d volte al secondo.\n", CANALE_ESPNOW, FREQUENZA_HZ);
}

void loop() {
  if (bno.wasReset()) {
    Serial.println("BNO08x riavviato, riattivo i report.");
    attivaReport();
  }

  while (bno.getSensorEvent(&ev)) {
    if (ev.sensorId == SH2_ROTATION_VECTOR) {
      qr = ev.un.rotationVector.real;
      qi = ev.un.rotationVector.i;
      qj = ev.un.rotationVector.j;
      qk = ev.un.rotationVector.k;
      calibrazione = ev.status & 0x03;
      datiValidi = true;
      ultimoDato = millis();
    } else if (ev.sensorId == SH2_GYROSCOPE_CALIBRATED) {
      gz = ev.un.gyroscope.z;
    }
  }

  if (datiValidi && millis() - ultimoDato > 2000) {
    Serial.println("Nessun dato dal BNO08x da 2 secondi.");
    datiValidi = false;
  }

  if (!datiValidi || millis() - ultimoInvio < 1000 / FREQUENZA_HZ) return;
  ultimoInvio = millis();

  // Quaternione -> angoli (gradi)
  float sqr = qr * qr, sqi = qi * qi, sqj = qj * qj, sqk = qk * qk;
  float imbardata  = atan2(2.0 * (qi * qj + qk * qr), sqi - sqj - sqk + sqr) * RAD_TO_DEG;
  float beccheggio = asin(constrain(-2.0 * (qi * qk - qj * qr) / (sqi + sqj + sqk + sqr), -1.0, 1.0)) * RAD_TO_DEG;
  float rollio     = atan2(2.0 * (qj * qk + qi * qr), -sqi - sqj + sqk + sqr) * RAD_TO_DEG;

  // Prua in senso orario da 0 a 360 (bussola)
  float prua = INVERTI_PRUA ? imbardata : -imbardata;
  prua = fmod(prua + OFFSET_PRUA + 720.0, 360.0);

  // Accostata in gradi al minuto, positiva verso dritta
  float accostata = (INVERTI_PRUA ? gz : -gz) * RAD_TO_DEG * 60.0;

  char corpo[80], pacchetto[240];
  int n = 0;
  snprintf(corpo, sizeof corpo, "HCHDM,%.1f,M", prua);
  n = aggiungiFrase(pacchetto, n, sizeof pacchetto, corpo);
  snprintf(corpo, sizeof corpo, "IIXDR,A,%.1f,D,PITCH,A,%.1f,D,ROLL", beccheggio, rollio);
  n = aggiungiFrase(pacchetto, n, sizeof pacchetto, corpo);
  snprintf(corpo, sizeof corpo, "TIROT,%.1f,A", accostata);
  n = aggiungiFrase(pacchetto, n, sizeof pacchetto, corpo);
  snprintf(corpo, sizeof corpo, "PNVIMU,%lu,%u", (unsigned long)contatore++, calibrazione);
  n = aggiungiFrase(pacchetto, n, sizeof pacchetto, corpo);

  esp_now_send(indirizzoBroadcast, (uint8_t *)pacchetto, n);
  Serial.print(pacchetto);
}
