// The black box: every raw reading of the boat, one JSON object per line, as in navplotter/blackbox.py of
// Navplotter-Linux, so that tools/stima_replay.py replays these recordings too.
// One file a day in /log on the SD card (2026-11-27.jsonl, by the UTC date of the GPS); days older than keepDays are
// deleted. Each line has the time `t` (seconds since 1970, UTC, from the GPS), the kind `k` and its values:
//   gps   lat, lon, sog (knots), cog (degrees or null), sats, hdop, src, utc
//   hdg   the compass: hdg (magnetic), roll, pitch, acc (calibration 0-3)
//   dr    the dead reckoning (nav_stima.h), every ten seconds
// Unlike the Raspberry the files are not compressed (no room or time for it on the T-Deck), and the download
// (/api/log.zip) holds only the last ZIP_DAYS days: while it is sent the node does nothing else.
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <vector>
#include <algorithm>

#define BOX_FLUSH_MS 10000UL
#define BOX_STOPPED_EVERY_S 60.0
#define BOX_ZIP_DAYS 3

class BlackBox {
public:
  bool ok = false;
  int keepDays = 30;

  void begin(bool sdOk, int keep) {
    ok = sdOk;
    keepDays = max(1, keep);
    if(ok && !SD.exists("/log")) ok = SD.mkdir("/log");
  }

  // One line; `values` is the rest of the object, starting with a comma (",\"lat\":45.1,...").
  // `moving` false thins the kinds that come every second (gps, hdg) down to one a minute.
  bool write(double epoch, const char* kind, bool moving, const String& values) {
    if(!ok || epoch < 1.6e9) return false;       // no time from the GPS yet
    int slot = strcmp(kind, "gps") == 0 ? 0 : strcmp(kind, "hdg") == 0 ? 1 : -1;
    if(slot >= 0) {
      double every = moving ? 0.9 : BOX_STOPPED_EVERY_S;
      if(epoch - last[slot] < every) return false;
      last[slot] = epoch;
    }
    char day[11];
    dayName(epoch, day);
    if(strcmp(day, pendingDay) != 0) {
      flush();
      strcpy(pendingDay, day);
      tidy(day);
    }
    char head[48];
    snprintf(head, sizeof(head), "{\"t\":%.2f,\"k\":\"%s\"", epoch, kind);
    pending += head;
    pending += values;
    pending += "}\n";
    if(pending.length() > 6000) flush();
    return true;
  }

  // Once a second: writes to the card what waits in memory
  void tick() {
    if(pending.length() && millis() - lastFlush >= BOX_FLUSH_MS) flush();
  }

  void flush() {
    lastFlush = millis();
    if(!ok || !pending.length() || !pendingDay[0]) { pending = ""; return; }
    File f = SD.open(String("/log/") + pendingDay + ".jsonl", FILE_APPEND);
    if(f) { f.print(pending); f.close(); }
    else Serial.println("Scatola nera: scrittura non riuscita");
    pending = "";
    cacheMs = 0;
  }

  // {"days":n,"bytes":n,"keepDays":n} as on the Raspberry, or "null" without a card
  String stateJson() {
    if(!ok) return "null";
    if(!cacheMs || millis() - cacheMs > 60000) {
      std::vector<FileInfo> fs = files();
      cacheDays = fs.size(); cacheBytes = 0;
      for(const auto& f : fs) cacheBytes += f.size;
      cacheMs = millis();
    }
    return String("{\"days\":") + cacheDays + ",\"bytes\":" + cacheBytes + ",\"keepDays\":" + keepDays + "}";
  }

  // The last BOX_ZIP_DAYS days as a zip without compression: first its exact size, then the bytes through `out`
  size_t zipSize() {
    flush();
    zipFiles = files();
    if(zipFiles.size() > BOX_ZIP_DAYS) zipFiles.erase(zipFiles.begin(), zipFiles.end() - BOX_ZIP_DAYS);
    size_t total = 22;
    for(const auto& f : zipFiles) total += 30 + f.name.length() + f.size + 16 + 46 + f.name.length();
    return total;
  }

  template<typename Out> void zipWrite(Out out) {
    std::vector<uint32_t> crcs, offsets;
    uint32_t offset = 0;
    static uint8_t buf[2048];
    for(const auto& f : zipFiles) {
      offsets.push_back(offset);
      uint8_t h[30];
      header(h, 0x04034b50, 0, 0, f.name.length(), false);
      out(h, 30); out((const uint8_t*)f.name.c_str(), f.name.length());
      uint32_t crc = 0xFFFFFFFF, left = f.size;
      File file = SD.open("/log/" + f.name);
      while(left && file) {
        int n = file.read(buf, min((uint32_t)sizeof(buf), left));
        if(n <= 0) break;
        crc = crc32(crc, buf, n);
        out(buf, n);
        left -= n;
      }
      if(file) file.close();
      while(left) { size_t n = min((uint32_t)sizeof(buf), left); memset(buf, 0, n); crc = crc32(crc, buf, n); out(buf, n); left -= n; }
      crc ^= 0xFFFFFFFF;
      crcs.push_back(crc);
      uint8_t d[16];
      put32(d, 0x08074b50); put32(d + 4, crc); put32(d + 8, f.size); put32(d + 12, f.size);
      out(d, 16);
      offset += 30 + f.name.length() + f.size + 16;
    }
    uint32_t dirStart = offset, dirSize = 0;
    for(size_t i = 0; i < zipFiles.size(); i++) {
      const auto& f = zipFiles[i];
      uint8_t c[46];
      memset(c, 0, 46);
      put32(c, 0x02014b50); put16(c + 4, 20); put16(c + 6, 20); put16(c + 8, 0x0008); put16(c + 10, 0);
      put16(c + 12, 0); put16(c + 14, 0x21); put32(c + 16, crcs[i]); put32(c + 20, f.size); put32(c + 24, f.size);
      put16(c + 28, f.name.length()); put32(c + 42, offsets[i]);
      out(c, 46); out((const uint8_t*)f.name.c_str(), f.name.length());
      dirSize += 46 + f.name.length();
    }
    uint8_t e[22];
    memset(e, 0, 22);
    put32(e, 0x06054b50); put16(e + 8, zipFiles.size()); put16(e + 10, zipFiles.size());
    put32(e + 12, dirSize); put32(e + 16, dirStart);
    out(e, 22);
  }

private:
  struct FileInfo { String name; uint32_t size; };
  String pending;
  char pendingDay[11] = "";
  unsigned long lastFlush = 0, cacheMs = 0;
  double last[2] = {0, 0};
  size_t cacheDays = 0;
  uint32_t cacheBytes = 0;
  std::vector<FileInfo> zipFiles;

  static void dayName(double epoch, char* out) {
    // Civil date from days since 1970 (Howard Hinnant's algorithm)
    long z = (long)(epoch / 86400.0) + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if(m <= 2) y++;
    snprintf(out, 11, "%04ld-%02u-%02u", y, m, d);
  }

  std::vector<FileInfo> files() {
    std::vector<FileInfo> out;
    File dir = SD.open("/log");
    if(!dir) return out;
    for(File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String n = f.name();
      int slash = n.lastIndexOf('/');
      if(slash >= 0) n = n.substring(slash + 1);
      if(!f.isDirectory() && n.length() == 16 && n.endsWith(".jsonl")) out.push_back({n, (uint32_t)f.size()});
      f.close();
    }
    dir.close();
    std::sort(out.begin(), out.end(), [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
    return out;
  }

  // Deletes the days older than keepDays (names compare as dates)
  void tidy(const char* today) {
    struct tm t = {};
    if(sscanf(today, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) return;
    long days = 0;
    {
      int y = t.tm_year, m = t.tm_mon, d = t.tm_mday;
      y -= m <= 2;
      long era = (y >= 0 ? y : y - 399) / 400;
      unsigned yoe = (unsigned)(y - era * 400);
      unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
      unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      days = era * 146097 + (long)doe - 719468;
    }
    char oldest[11];
    dayName((double)(days - (keepDays - 1)) * 86400.0 + 1, oldest);
    for(const auto& f : files()) {
      if(f.name.substring(0, 10) < String(oldest)) SD.remove("/log/" + f.name);
    }
  }

  static uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool made = false;
    if(!made) {
      for(uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for(int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        table[i] = c;
      }
      made = true;
    }
    while(n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
  }
  static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
  static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
  static void header(uint8_t* h, uint32_t sig, uint32_t crc, uint32_t size, uint16_t nameLen, bool) {
    memset(h, 0, 30);
    put32(h, sig); put16(h + 4, 20); put16(h + 6, 0x0008); put16(h + 8, 0);
    put16(h + 10, 0); put16(h + 12, 0x21); put32(h + 14, crc); put32(h + 18, size); put32(h + 22, size);
    put16(h + 26, nameLen);
  }
};
