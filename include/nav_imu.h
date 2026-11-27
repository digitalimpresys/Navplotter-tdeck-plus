// External compass over ESP-NOW: an ESP32 with a BNO08x (bussola-imu/trasmettitore_imu) broadcasts NMEA
// sentences on WiFi channel IMU_CHANNEL; any NavPlotter on that channel receives them.
//   $HCHDM,231.4,M*hh                       magnetic heading
//   $IIXDR,A,2.3,D,PITCH,A,-12.1,D,ROLL*hh  pitch and roll, degrees
//   $TIROT,35.2,A*hh                        rate of turn, degrees/minute, + = to starboard
//   $PNVIMU,1234,3*hh                       packet counter, calibration 0-3
// ESP-NOW only works on the channel the radio is on: the plotter's own network (soft AP) is on channel 1
// like the transmitter, but while the plotter is joined to the boat WiFi the radio follows that router's
// channel and the compass is not heard: the heading then falls back to the GPS course.
// Written for the Arduino core 2.x (ESP-IDF 4.4) receive callback.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>

#define IMU_CHANNEL 1
#define IMU_TIMEOUT_MS 3000UL

struct ImuData {
  float hdgMag = 0, pitch = 0, roll = 0, rot = 0;
  uint8_t cal = 0;
  uint32_t seq = 0, packets = 0, badChecksum = 0;
  uint32_t lastMs = 0;          // millis() of the last valid heading, 0 = never
  bool hasAttitude = false, hasRot = false;
  uint8_t mac[6] = {0};
};

static ImuData imuData;
static portMUX_TYPE imuMux = portMUX_INITIALIZER_UNLOCKED;
static bool imuStarted = false;

// NMEA checksum: XOR of everything between '$' and '*'
static bool imuChecksumOk(const char* s) {
  if(*s != '$') return false;
  uint8_t ck = 0;
  const char* p = s + 1;
  while(*p && *p != '*') ck ^= (uint8_t)*p++;
  if(*p != '*' || !isxdigit((unsigned char)p[1]) || !isxdigit((unsigned char)p[2])) return false;
  return (uint8_t)strtoul(p + 1, nullptr, 16) == ck;
}

// Splits "$TTSSS,a,b,c*hh" in place into fields (field 0 = "TTSSS"); returns the number of fields
static int imuFields(char* s, char** f, int maxFields) {
  char* star = strchr(s, '*');
  if(star) *star = '\0';
  int n = 0;
  char* p = s + 1;
  while(n < maxFields) {
    f[n++] = p;
    char* c = strchr(p, ',');
    if(!c) break;
    *c = '\0';
    p = c + 1;
  }
  return n;
}

// Parses one sentence into d; returns true if it carried a heading
static bool imuParseSentence(char* line, ImuData& d) {
  if(!imuChecksumOk(line)) { d.badChecksum++; return false; }
  char* f[16];
  int n = imuFields(line, f, 16);
  if(n < 2) return false;
  const char* id = f[0];
  size_t len = strlen(id);
  if(strcmp(id, "PNVIMU") == 0) {
    d.seq = strtoul(f[1], nullptr, 10);
    if(n > 2) d.cal = constrain(atoi(f[2]), 0, 3);
    return false;
  }
  if(len != 5) return false;
  const char* type = id + 2; // the talker id in front does not matter
  if(strcmp(type, "HDM") == 0 || strcmp(type, "HDG") == 0) {
    if(!*f[1]) return false;
    d.hdgMag = fmod(atof(f[1]) + 360.0, 360.0);
    return true;
  }
  if(strcmp(type, "XDR") == 0) {
    // Groups of four: type, value, unit, name
    for(int i = 1; i + 3 < n; i += 4) {
      if(strcmp(f[i], "A") != 0 || !*f[i + 1]) continue;
      if(strcmp(f[i + 3], "PITCH") == 0) { d.pitch = atof(f[i + 1]); d.hasAttitude = true; }
      else if(strcmp(f[i + 3], "ROLL") == 0) { d.roll = atof(f[i + 1]); d.hasAttitude = true; }
    }
    return false;
  }
  if(strcmp(type, "ROT") == 0) {
    if(*f[1] && (n < 3 || f[2][0] != 'V')) { d.rot = atof(f[1]); d.hasRot = true; }
    return false;
  }
  return false;
}

// Called by the WiFi task for every ESP-NOW packet: parse a copy and store it under the lock
static void imuOnRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if(len <= 0) return;
  char buf[251];
  len = min(len, 250);
  memcpy(buf, data, len);
  buf[len] = '\0';
  ImuData d;
  portENTER_CRITICAL(&imuMux);
  d = imuData;
  portEXIT_CRITICAL(&imuMux);
  bool heading = false;
  char* save = nullptr;
  for(char* line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(nullptr, "\r\n", &save)) {
    if(imuParseSentence(line, d)) heading = true;
  }
  if(!heading) return;
  d.lastMs = millis();
  if(d.lastMs == 0) d.lastMs = 1;
  d.packets++;
  memcpy(d.mac, mac, 6);
  portENTER_CRITICAL(&imuMux);
  imuData = d;
  portEXIT_CRITICAL(&imuMux);
}

// Starts ESP-NOW; with the WiFi off the radio is switched on as a station on the compass channel
static void imuBegin() {
  if(WiFi.getMode() == WIFI_OFF) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // in modem sleep ESP-NOW packets get lost
    esp_wifi_set_channel(IMU_CHANNEL, WIFI_SECOND_CHAN_NONE);
  }
  if(esp_now_init() != ESP_OK) {
    Serial.println("Bussola ESP-NOW: avvio non riuscito");
    return;
  }
  esp_now_register_recv_cb(imuOnRecv);
  imuStarted = true;
  Serial.printf("Bussola ESP-NOW in ascolto sul canale %d\n", IMU_CHANNEL);
}

// To be called from loop(): when the plotter's WiFi is switched off the radio stops, and ESP-NOW with it
static void imuLoop() {
  static unsigned long nextCheck = 0;
  if((long)(millis() - nextCheck) < 0) return;
  nextCheck = millis() + 1000;
  if(WiFi.getMode() == WIFI_OFF) {
    if(imuStarted) { esp_now_deinit(); imuStarted = false; }
    imuBegin();
  } else if(!imuStarted) {
    imuBegin();
  }
}

static ImuData imuSnapshot() {
  portENTER_CRITICAL(&imuMux);
  ImuData d = imuData;
  portEXIT_CRITICAL(&imuMux);
  return d;
}

static bool imuFresh(const ImuData& d) {
  return d.lastMs != 0 && millis() - d.lastMs < IMU_TIMEOUT_MS;
}
