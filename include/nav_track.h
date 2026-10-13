// The boat's track: the positions it has been through, recorded by the node itself (a phone sleeps, changes
// network, closes its apps; the node is always on and has the GPS).
// This file MUST be identical in every NavPlotter that has it.
//
// A point is recorded every TRACK_STEP_M metres made good, none while the boat is still. The points live in
// memory and are saved to the node's storage now and then, so that a power cut loses a couple of minutes at
// most. When the track is full its oldest quarter is dropped. The web app (or an app) reads it through
// /api/track and can have it as a GPX file.
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <time.h>
#include <vector>

#ifndef TRACK_MAX_POINTS
#define TRACK_MAX_POINTS 4000        // 25 m apart: about 54 nautical miles
#endif
#define TRACK_STEP_M 25.0
#define TRACK_FILE "/track.bin"
#define TRACK_SAVE_MS 120000UL       // unsaved points are written after this long...
#define TRACK_SAVE_POINTS 20         // ...or when there are this many

struct __attribute__((packed)) TrackPoint {
  int32_t lat, lon;   // degrees * 1e7
  uint32_t time;      // seconds since 1970 (UTC, from the GPS), 0 = unknown
};

static std::vector<TrackPoint> trackPoints;
static uint32_t trackId = 1;          // changes when a new track is started: whoever holds a copy starts again
static uint32_t trackDropped = 0;     // points dropped from the front of this track, so that indexes keep their meaning
static size_t trackSaved = 0;         // how many points of trackPoints are in the file already
static bool trackRewrite = false;     // the file must be written again from the start
static unsigned long trackLastSave = 0;
static fs::FS* trackFs = nullptr;

static inline double trackMetres(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2) {
  double dy = (lat2 - lat1) / 1e7 * 110540.0;
  double dx = (lon2 - lon1) / 1e7 * 111320.0 * cos(lat1 / 1e7 * DEG_TO_RAD);
  return sqrt(dx * dx + dy * dy);
}

static inline double trackLengthNm() {
  double m = 0;
  for(size_t i = 1; i < trackPoints.size(); i++) m += trackMetres(trackPoints[i - 1].lat, trackPoints[i - 1].lon, trackPoints[i].lat, trackPoints[i].lon);
  return m / 1852.0;
}

// Seconds since 1970 from a UTC date and time (the GPS gives these)
static inline uint32_t trackEpoch(int year, int month, int day, int hour, int minute, int second) {
  if(year < 2020) return 0;
  year -= month <= 2;
  long era = year / 400, yoe = year - era * 400;
  long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097 + doe - 719468;
  return (uint32_t)(days * 86400L + hour * 3600L + minute * 60L + second);
}

// File: "NTRK", track id, points dropped, then the points
static inline void trackWriteFile() {
  if(!trackFs) return;
  File f = trackFs->open(TRACK_FILE, "w");
  if(!f) return;
  f.write((const uint8_t*)"NTRK", 4);
  f.write((const uint8_t*)&trackId, 4);
  f.write((const uint8_t*)&trackDropped, 4);
  if(!trackPoints.empty()) f.write((const uint8_t*)trackPoints.data(), trackPoints.size() * sizeof(TrackPoint));
  f.close();
  trackSaved = trackPoints.size();
  trackRewrite = false;
}

static inline void trackBegin(fs::FS& fs) {
  trackFs = &fs;
  trackPoints.clear();
  File f = fs.open(TRACK_FILE, "r");
  if(f) {
    char magic[4];
    if(f.read((uint8_t*)magic, 4) == 4 && memcmp(magic, "NTRK", 4) == 0 && f.read((uint8_t*)&trackId, 4) == 4 && f.read((uint8_t*)&trackDropped, 4) == 4) {
      TrackPoint p;
      while(trackPoints.size() < TRACK_MAX_POINTS && f.read((uint8_t*)&p, sizeof(p)) == sizeof(p)) trackPoints.push_back(p);
    } else {
      trackId = 1; trackDropped = 0;
    }
    f.close();
  }
  trackSaved = trackPoints.size();
  trackLastSave = millis();
}

// Writes what is not in the file yet. Called often: it does something only now and then (or at once with force).
static inline void trackSave(bool force = false) {
  if(!trackFs) return;
  size_t pending = trackPoints.size() - trackSaved;
  if(!trackRewrite && pending == 0) return;
  if(!force && !trackRewrite && pending < TRACK_SAVE_POINTS && millis() - trackLastSave < TRACK_SAVE_MS) return;
  trackLastSave = millis();
  if(trackRewrite || trackSaved == 0) { trackWriteFile(); return; }
  File f = trackFs->open(TRACK_FILE, "a");
  if(!f) return;
  f.write((const uint8_t*)&trackPoints[trackSaved], pending * sizeof(TrackPoint));
  f.close();
  trackSaved = trackPoints.size();
}

// A position from the GPS: becomes a point of the track when the boat has moved far enough from the last one
static inline bool trackAdd(double lat, double lon, uint32_t time) {
  TrackPoint p = {(int32_t)lround(lat * 1e7), (int32_t)lround(lon * 1e7), time};
  if(!trackPoints.empty()) {
    const TrackPoint& last = trackPoints.back();
    if(trackMetres(last.lat, last.lon, p.lat, p.lon) < TRACK_STEP_M) return false;
  }
  if(trackPoints.size() >= TRACK_MAX_POINTS) {
    // Full: the oldest quarter goes
    size_t drop = TRACK_MAX_POINTS / 4;
    trackPoints.erase(trackPoints.begin(), trackPoints.begin() + drop);
    trackDropped += drop;
    trackRewrite = true;
  }
  trackPoints.push_back(p);
  return true;
}

static inline void trackNew() {
  trackPoints.clear();
  trackId++;
  trackDropped = 0;
  trackRewrite = true;
  trackSave(true);
}

// ---- For the web app ----

// What goes in /api/state: the web app asks for points only when these numbers change
static inline String trackStateJson() {
  return String("{\"id\":") + trackId + ",\"n\":" + (trackDropped + trackPoints.size()) + ",\"first\":" + trackDropped + "}";
}

// The points from index "from" on (indexes count from the start of the track, dropped points included), as
// JSON in pieces: send(piece) is called several times. Positions in degrees * 1e6.
template<typename Send> static inline void trackWriteJson(uint32_t from, Send send) {
  if(from < trackDropped) from = trackDropped;
  String out = String("{\"id\":") + trackId + ",\"first\":" + trackDropped + ",\"from\":" + from + ",\"points\":[";
  out.reserve(1200);
  char b[48];
  for(size_t i = from - trackDropped; i < trackPoints.size(); i++) {
    const TrackPoint& p = trackPoints[i];
    snprintf(b, sizeof(b), "%s[%ld,%ld,%lu]", i > from - trackDropped ? "," : "", (long)(p.lat / 10), (long)(p.lon / 10), (unsigned long)p.time);
    out += b;
    if(out.length() > 1000) { send(out); out = ""; }
  }
  out += "]}";
  send(out);
}

// The whole track as a GPX file, in pieces
template<typename Send> static inline void trackWriteGpx(const char* name, Send send) {
  String out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"NavPlotter\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n<trk><name>";
  for(const char* c = name; *c; c++) if(*c != '<' && *c != '>' && *c != '&') out += *c;
  out += "</name><trkseg>\n";
  out.reserve(1300);
  char b[120], iso[24];
  for(const auto& p : trackPoints) {
    if(p.time) {
      time_t t = p.time;
      struct tm tmv;
      gmtime_r(&t, &tmv);
      strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);
      snprintf(b, sizeof(b), "<trkpt lat=\"%.6f\" lon=\"%.6f\"><time>%s</time></trkpt>\n", p.lat / 1e7, p.lon / 1e7, iso);
    } else {
      snprintf(b, sizeof(b), "<trkpt lat=\"%.6f\" lon=\"%.6f\"/>\n", p.lat / 1e7, p.lon / 1e7);
    }
    out += b;
    if(out.length() > 1100) { send(out); out = ""; }
  }
  out += "</trkseg></trk>\n</gpx>\n";
  send(out);
}
