// Dead reckoning (stima): with the GPS alone, and with the compass when there is one.
// The same filter as navplotter/stima.py in Navplotter-Linux, line by line, with the same numbers, so that the
// two nodes estimate alike and a recording of one can be replayed on the other.
//
// A Kalman filter keeps the position and the velocity over the ground of the boat, with how uncertain they are.
// While the GPS has a fix the filter runs in its shadow: it follows the GPS, smooths it and notices when the GPS
// jumps. When the fix is lost the filter carries the boat on along its last course and speed, and the uncertainty
// grows into an ellipse. After maxMin minutes, or when the ellipse is wider than two miles, the estimate is
// declared not reliable. With a compass (nav_bno08x.h) the velocity is turned every second by as much as the
// heading has turned, so the estimate follows the turns; only the change of the heading is used.
//
// The estimate never replaces the position of the node: it is not transmitted, not added to the track. It goes
// out only in the "dr" block of /api/state (docs/PROTOCOL.md in Navplotter-Linux).
//
// Pure C++: no Arduino, so that it can be checked on a computer against the Python one.
#pragma once
#include <math.h>
#include <stdio.h>

namespace stima {

const double KN = 0.514444;
const double EARTH_M = 6371000.0;
const double FIX_S = 5.0;
const double MAX_HALF_AXIS_M = 2 * 1852.0;
const double RECENTRE_M = 5000.0;
const double Q_ALONG = 0.0015, Q_ACROSS = 0.0035, Q_STOPPED = 0.0025;
const double Q_ACROSS_HDG = 0.0008;
const double HDG_FRESH_S = 3.0;
const double STOPPED_MS = 0.25;
const double GPS_SPEED_SD = 0.25;
const double JUMP_MIN_M = 50.0;
const double JUMP_SIGMA = 5.0;
const double JUMP_ACCEPT_S = 20.0;
const double CHI2_95 = 5.991;
const double D2R = M_PI / 180.0;

// Metres of uncertainty of a fix from its HDOP; no HDOP (negative) is taken as fair
inline double gpsSd(double hdop) {
  if(!(hdop > 0) || hdop >= 50) return 8.0;
  double h = hdop < 20.0 ? hdop : 20.0;
  double sd = h * 4.0;
  return sd > 3.0 ? sd : 3.0;
}

// Python's % for floats: the result has the sign of the divisor
inline double pmod(double a, double b) {
  double r = fmod(a, b);
  if(r != 0 && ((r < 0) != (b < 0))) r += b;
  return r;
}

struct State {
  const char* mode;     // "gps", "dr" or "off"
  bool hasAge;          // false: never had a fix
  int age;
  double lat, lon, sog, cog;
  int a, b, th;
  bool hasGap;
  int gap;
  bool jump, hdg;
};

class Stima {
public:
  explicit Stima(double maxMin = 30) {
    maxS = (maxMin > 1.0 ? maxMin : 1.0) * 60.0;
  }

  // A fix: sog in knots, cog in degrees or NAN when the receiver has none, hdop negative when unknown
  void gps(double now, double lat, double lon, double sog, double cog, double hdop) {
    if(!(sog == sog)) sog = 0.0;
    double sd = gpsSd(hdop);
    // After a stretch without fixes the filter was only guessing: the GPS that comes back is believed at once
    bool alone = hasFix && now - fixTime >= FIX_S;
    hasFix = true;
    fixTime = now;
    gpsLat = lat; gpsLon = lon;
    if(!ready || expired || alone) { start(now, lat, lon, sog, cog, sd); return; }
    predict(now);
    double z[2]; toEn(lat, lon, z);
    double y[2], si[3];
    innovation(0, z, sd, y, si);
    double far_ = hypot(y[0], y[1]);
    double q = y[0] * (si[0] * y[0] + si[1] * y[1]) + y[1] * (si[1] * y[0] + si[2] * y[1]);
    double sigmas = sqrt(q > 0 ? q : 0);
    if(far_ > JUMP_MIN_M && sigmas > JUMP_SIGMA) {
      if(!jump) { jump = true; jumpSince = now; }
      else if(now - jumpSince >= JUMP_ACCEPT_S) start(now, lat, lon, sog, cog, sd);  // the filter was wrong
      return;
    }
    jump = false;
    update(0, y, si);
    if(cog == cog && sog * KN >= STOPPED_MS) {
      double r = cog * D2R, zv[2] = {sog * KN * sin(r), sog * KN * cos(r)};
      innovation(2, zv, GPS_SPEED_SD, y, si);
      update(2, y, si);
    } else if(sog * KN < STOPPED_MS) {
      double zv[2] = {0.0, 0.0};
      innovation(2, zv, 0.3 + sog * KN, y, si);
      update(2, y, si);
    }
  }

  // The heading of the compass in degrees: only its changes count
  void heading(double now, double h) {
    hdgV = pmod(h, 360.0);
    hdgTime = now;
    hasHdg = true;
  }

  // Once a second: carries the estimate on, and gives it up when it is too old or too wide
  void step(double now) {
    if(!ready || expired) return;
    predict(now);
    double a, b, th;
    ellipse(a, b, th);
    if(now - fixTime >= FIX_S && (now - fixTime > maxS || a > MAX_HALF_AXIS_M)) expired = true;
  }

  bool steered(double now) const { return hasHdg && now - hdgTime < HDG_FRESH_S; }

  void ellipse(double& a, double& b, double& th) const {
    double pee = p[0][0], pen = p[0][1], pnn = p[1][1];
    double mid = (pee + pnn) / 2.0, half = hypot((pee - pnn) / 2.0, pen);
    double u = mid + half, v = mid - half;
    a = sqrt(CHI2_95 * (u > 0 ? u : 0));
    b = sqrt(CHI2_95 * (v > 0 ? v : 0));
    double east = 0.5 * atan2(2.0 * pen, pee - pnn);
    th = pmod(90.0 - east / D2R, 180.0);
  }

  State state(double now) const {
    State s = {};
    s.mode = "off";
    if(!ready) return s;
    double age = now - fixTime; if(age < 0) age = 0;
    s.hasAge = true;
    s.age = (int)age;
    if(expired) return s;
    bool fix = age < FIX_S;
    position(s.lat, s.lon);
    double a, b, th;
    ellipse(a, b, th);
    double speed = hypot(x[2], x[3]);
    s.mode = fix ? "gps" : "dr";
    s.sog = round(speed / KN * 100.0) / 100.0;
    s.cog = round(pmod(atan2(x[2], x[3]) / D2R, 360.0) * 10.0) / 10.0;
    s.a = (int)lround(a); s.b = (int)lround(b); s.th = (int)lround(th);
    if(fix) {
      double en[2]; toEn(gpsLat, gpsLon, en);
      s.hasGap = true;
      s.gap = (int)lround(hypot(en[0] - x[0], en[1] - x[1]));
    }
    s.jump = jump;
    s.hdg = steered(now);
    return s;
  }

  // The "dr" block of /api/state, in the same form as the Linux node
  int json(double now, char* buf, int size) const {
    State s = state(now);
    if(s.mode[0] == 'o') {
      if(s.hasAge) return snprintf(buf, size, "{\"mode\":\"off\",\"age\":%d}", s.age);
      return snprintf(buf, size, "{\"mode\":\"off\",\"age\":null}");
    }
    char gap[12];
    if(s.hasGap) snprintf(gap, sizeof(gap), "%d", s.gap); else snprintf(gap, sizeof(gap), "null");
    return snprintf(buf, size,
      "{\"mode\":\"%s\",\"lat\":%.7f,\"lon\":%.7f,\"sog\":%.2f,\"cog\":%.1f,\"age\":%d,\"a\":%d,\"b\":%d,\"th\":%d,\"gap\":%s,\"jump\":%s,\"hdg\":%s}",
      s.mode, s.lat, s.lon, s.sog, s.cog, s.age, s.a, s.b, s.th, gap, s.jump ? "true" : "false", s.hdg ? "true" : "false");
  }

  void position(double& lat, double& lon) const { toLl(x[0], x[1], lat, lon); }

private:
  double maxS;
  bool ready = false, expired = false;
  double x[4] = {0, 0, 0, 0};
  double p[4][4] = {};
  double lat0 = 0, lon0 = 0, t = 0;
  bool hasFix = false;
  double fixTime = 0, gpsLat = 0, gpsLon = 0;
  bool jump = false;
  double jumpSince = 0;
  bool hasHdg = false;
  double hdgV = 0, hdgTime = -1e9;
  bool hasRef = false;
  double hdgRef = 0;

  void toEn(double lat, double lon, double* en) const {
    en[0] = (lon - lon0) * D2R * EARTH_M * cos(lat0 * D2R);
    en[1] = (lat - lat0) * D2R * EARTH_M;
  }
  void toLl(double e, double n, double& lat, double& lon) const {
    lat = lat0 + n / EARTH_M / D2R;
    lon = lon0 + e / (EARTH_M * cos(lat0 * D2R)) / D2R;
  }
  void recentre() {
    if(hypot(x[0], x[1]) > RECENTRE_M) {
      position(lat0, lon0);
      x[0] = x[1] = 0.0;
    }
  }

  void start(double now, double lat, double lon, double sog, double cog, double sd) {
    lat0 = lat; lon0 = lon;
    bool mov = cog == cog && sog * KN >= STOPPED_MS;
    double v = sog * KN, r = (cog == cog ? cog : 0.0) * D2R;
    x[0] = x[1] = 0.0;
    x[2] = mov ? v * sin(r) : 0.0;
    x[3] = mov ? v * cos(r) : 0.0;
    double vv = mov ? GPS_SPEED_SD * GPS_SPEED_SD : 1.0;
    for(int i = 0; i < 4; i++) for(int j = 0; j < 4; j++) p[i][j] = 0;
    p[0][0] = p[1][1] = sd * sd;
    p[2][2] = p[3][3] = vv;
    t = now;
    ready = true; expired = false; jump = false;
    hasRef = false;
  }

  void turn(double degrees) {
    double c = cos(degrees * D2R), s = sin(degrees * D2R);
    double x2 = x[2], x3 = x[3];
    x[2] = x2 * c + x3 * s;
    x[3] = -x2 * s + x3 * c;
    for(int j = 0; j < 4; j++) {
      double a = p[2][j], b = p[3][j];
      p[2][j] = a * c + b * s;
      p[3][j] = -a * s + b * c;
    }
    for(int i = 0; i < 4; i++) {
      double a = p[i][2], b = p[i][3];
      p[i][2] = a * c + b * s;
      p[i][3] = -a * s + b * c;
    }
  }

  void predict(double now) {
    double dt = now - t;
    if(dt <= 0) return;
    t = now;
    bool st = steered(now);
    if(!st) hasRef = false;                 // a turn made meanwhile is lost
    else if(!hasRef) { hasRef = true; hdgRef = hdgV; }
    else {
      double tr = pmod(hdgV - hdgRef + 180.0, 360.0) - 180.0;
      hdgRef = hdgV;
      if(tr != 0) turn(tr);
    }
    x[0] += x[2] * dt;
    x[1] += x[3] * dt;
    // P = F P F' with F = [[I, dt I], [0, I]]
    for(int i = 0; i < 2; i++) for(int j = 0; j < 4; j++) p[i][j] += dt * p[i + 2][j];
    for(int j = 0; j < 2; j++) for(int i = 0; i < 4; i++) p[i][j] += dt * p[i][j + 2];
    double speed = hypot(x[2], x[3]);
    double a[2][2];
    if(speed >= STOPPED_MS) {
      double ue = x[2] / speed, un = x[3] / speed;
      double qc = st ? Q_ACROSS_HDG : Q_ACROSS;
      a[0][0] = Q_ALONG * ue * ue + qc * un * un;
      a[0][1] = a[1][0] = (Q_ALONG - qc) * ue * un;
      a[1][1] = Q_ALONG * un * un + qc * ue * ue;
    } else {
      a[0][0] = a[1][1] = Q_STOPPED;
      a[0][1] = a[1][0] = 0.0;
    }
    double d3 = dt * dt * dt / 3.0, d2 = dt * dt / 2.0;
    for(int i = 0; i < 2; i++) for(int j = 0; j < 2; j++) {
      p[i][j] += a[i][j] * d3;
      p[i][j + 2] += a[i][j] * d2;
      p[i + 2][j] += a[i][j] * d2;
      p[i + 2][j + 2] += a[i][j] * dt;
    }
    recentre();
  }

  void innovation(int k, const double* z, double sd, double* y, double* si) const {
    y[0] = z[0] - x[k]; y[1] = z[1] - x[k + 1];
    double s00 = p[k][k] + sd * sd, s01 = p[k][k + 1], s11 = p[k + 1][k + 1] + sd * sd;
    double det = s00 * s11 - s01 * s01;
    si[0] = s11 / det; si[1] = -s01 / det; si[2] = s00 / det;
  }

  void update(int k, const double* y, const double* si) {
    double g[4][2];
    for(int i = 0; i < 4; i++) {
      g[i][0] = p[i][k] * si[0] + p[i][k + 1] * si[1];
      g[i][1] = p[i][k] * si[1] + p[i][k + 1] * si[2];
    }
    for(int i = 0; i < 4; i++) x[i] += g[i][0] * y[0] + g[i][1] * y[1];
    double r0[4], r1[4];
    for(int j = 0; j < 4; j++) { r0[j] = p[k][j]; r1[j] = p[k + 1][j]; }
    for(int i = 0; i < 4; i++) for(int j = 0; j < 4; j++) p[i][j] -= g[i][0] * r0[j] + g[i][1] * r1[j];
    for(int i = 0; i < 4; i++) for(int j = 0; j < i; j++) p[i][j] = p[j][i] = (p[i][j] + p[j][i]) / 2.0;
  }
};

}  // namespace stima
