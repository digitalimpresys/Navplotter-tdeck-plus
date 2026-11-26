// The black box: every raw reading of the boat, as it comes, one JSON object per line, on the SD card.
// The same files as navplotter/blackbox.py in Navplotter-Linux, so that tools/stima_replay.py there replays them:
// one file a day in /log (2026-11-26.jsonl, by the UTC date of the GPS), each line with the time "t" (seconds since
// 1970, from the GPS clock carried on by the board's own between fixes), the kind "k" and its values:
//
//   gps   lat, lon, sog (knots), cog (degrees or null), sats, hdop, src, utc (the GPS's own time)
//   dr    the dead reckoning (nav_stima.h), every ten seconds
//   hdg   the compass (nav_bno08x.h), once a second: hdg (magnetic), roll, pitch, acc (calibration, 0-3)
//
// Differences from the Linux node: the closed days are not compressed (the board would take too long), and
// nothing is written before the GPS has given the date once, since the board has no clock of its own.
// The days older than keepDays are deleted. /api/log.zip gives them all in one zip, written as it is sent.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace blackbox {

// CRC-32 as zip wants it
inline uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static bool ready = false;
  if(!ready) {
    for(uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for(int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  while(n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

inline void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// A zip written as it is sent, stored (no compression), with the sizes after each file: the board can send it
// without knowing them first. out(buf, n) sends bytes; the caller gives each file with begin / data / end.
template <class Out>
class ZipWriter {
public:
  explicit ZipWriter(Out& o) : out(o) {}

  void begin(const char* name, uint16_t dosTime, uint16_t dosDate) {
    if(count >= MAX_FILES) return;
    Entry& e = entries[count];
    strncpy(e.name, name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = 0;
    e.offset = offset; e.crc = 0; e.size = 0; e.time = dosTime; e.date = dosDate;
    uint16_t nl = strlen(e.name);
    uint8_t h[30];
    put32(h, 0x04034b50); put16(h + 4, 20); put16(h + 6, 0x0008); put16(h + 8, 0);
    put16(h + 10, dosTime); put16(h + 12, dosDate); put32(h + 14, 0); put32(h + 18, 0); put32(h + 22, 0);
    put16(h + 26, nl); put16(h + 28, 0);
    send(h, 30); send((const uint8_t*)e.name, nl);
  }
  void data(const uint8_t* p, size_t n) {
    if(count >= MAX_FILES) return;
    Entry& e = entries[count];
    e.crc = crc32(e.crc, p, n);
    e.size += n;
    send(p, n);
  }
  void end() {
    if(count >= MAX_FILES) return;
    Entry& e = entries[count];
    uint8_t d[16];
    put32(d, 0x08074b50); put32(d + 4, e.crc); put32(d + 8, e.size); put32(d + 12, e.size);
    send(d, 16);
    count++;
  }
  // The table of contents at the end
  void finish() {
    uint32_t cdStart = offset;
    for(int i = 0; i < count; i++) {
      Entry& e = entries[i];
      uint16_t nl = strlen(e.name);
      uint8_t h[46];
      put32(h, 0x02014b50); put16(h + 4, 20); put16(h + 6, 20); put16(h + 8, 0x0008); put16(h + 10, 0);
      put16(h + 12, e.time); put16(h + 14, e.date); put32(h + 16, e.crc); put32(h + 20, e.size); put32(h + 24, e.size);
      put16(h + 28, nl); put16(h + 30, 0); put16(h + 32, 0); put16(h + 34, 0); put16(h + 36, 0); put32(h + 38, 0);
      put32(h + 42, e.offset);
      send(h, 46); send((const uint8_t*)e.name, nl);
    }
    uint8_t z[22];
    put32(z, 0x06054b50); put16(z + 4, 0); put16(z + 6, 0); put16(z + 8, count); put16(z + 10, count);
    put32(z + 12, offset - cdStart); put32(z + 16, cdStart); put16(z + 20, 0);
    send(z, 22);
  }

private:
  static const int MAX_FILES = 64;
  struct Entry { char name[24]; uint32_t offset, crc, size; uint16_t time, date; };
  Out& out;
  Entry entries[MAX_FILES];
  int count = 0;
  uint32_t offset = 0;
  void send(const uint8_t* p, size_t n) { out(p, n); offset += n; }
};

// Days since 1970 of a date, and back (proleptic Gregorian, as the GPS gives it)
inline int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}
inline void civilFromDays(int32_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = (int)yoe + era * 400 + (m <= 2);
}
// "2026-11-26" of a time in seconds since 1970
inline void dayName(double t, char* out) {
  int y; unsigned m, d;
  civilFromDays((int32_t)(t / 86400.0), y, m, d);
  snprintf(out, 11, "%04d-%02u-%02u", y, m, d);
}

}  // namespace blackbox
