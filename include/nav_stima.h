// Dead reckoning (stima): the same filter as navplotter/stima.py of Navplotter-Linux, line by line, with the same
// numbers, so that the nodes estimate alike and the web module (web/navplotter-stima.js) reads the same `dr` block.
// Keep the two in step: a change made to one is made to the other.
//
// A Kalman filter keeps the position and the velocity over the ground of the boat, with how uncertain they are.
// While the GPS has a fix the filter runs in its shadow: it follows the GPS, smooths it and notices when the GPS
// jumps. When the fix is lost the filter carries the boat on along its last course and speed, and the uncertainty
// grows into an ellipse. After `maxMin` minutes, or when the ellipse is wider than two miles, the estimate is
// declared not reliable.
// With a compass the velocity is turned every second by as much as the heading has turned: only the change of the
// heading counts, so magnetic variation, a compass mounted askew, deviation or leeway need no setting.
// The estimate never replaces the GPS position: it is not transmitted over LoRa, not added to the track and not
// used by the anchor watch or the collision alarm.
// Times are seconds (double), from any clock that does not jump.
#pragma once
#include <Arduino.h>
#include <math.h>

namespace stima {
const double KN = 0.514444, EARTH_M = 6371000.0;
const double FIX_S = 5.0;
const double MAX_HALF_AXIS_M = 2 * 1852.0;
const double RECENTRE_M = 5000.0;
const double Q_ALONG = 0.0015, Q_ACROSS = 0.0035, Q_STOPPED = 0.0025;
const double Q_ACROSS_HDG = 0.0008;
const double HDG_FRESH_S = 3.0;
const double STOPPED_MS = 0.25;
const double GPS_SPEED_SD = 0.25;
const double JUMP_MIN_M = 50.0, JUMP_SIGMA = 5.0, JUMP_ACCEPT_S = 20.0;
const double CHI2_95 = 5.991;
}

struct StimaState {
  char mode[4] = "off";   // "gps", "dr" or "off"
  bool hasAge = false;    // false: never had a fix
  long age = 0;
  double lat = 0, lon = 0, sog = 0, cog = 0;
  long a = 0, b = 0, th = 0;
  bool hasGap = false;
  long gap = 0;
  bool jump = false, hdg = false;
};

class Stima {
public:
  explicit Stima(double maxMin = 30) { maxS = max(1.0, maxMin) * 60.0; }

  // A fix: sog in knots, cog in degrees or NAN when the receiver gives none, hdop < 0 when unknown
  void gps(double now, double lat, double lon, double sog, double cog, double hdop) {
    if(isnan(sog)) sog = 0;
    double sd = gpsSd(hdop);
    bool alone = hasFix && now - fixTime >= stima::FIX_S;
    fixTime = now; hasFix = true;
    gpsLat = lat; gpsLon = lon;
    if(!ready || expired || alone) { start(now, lat, lon, sog, cog, sd); return; }
    predict(now);
    double z[2]; toEn(lat, lon, z);
    double y[2], si[3];
    innovation(0, z, sd, y, si);
    double far = hypot(y[0], y[1]);
    double sigmas = sqrt(max(0.0, y[0] * (si[0] * y[0] + si[1] * y[1]) + y[1] * (si[1] * y[0] + si[2] * y[1])));
    if(far > stima::JUMP_MIN_M && sigmas > stima::JUMP_SIGMA) {
      if(!jump) { jump = true; jumpSince = now; }
      else if(now - jumpSince >= stima::JUMP_ACCEPT_S) start(now, lat, lon, sog, cog, sd); // the filter was wrong
      return;
    }
    jump = false;
    update(0, y, si);
    if(!isnan(cog) && sog * stima::KN >= stima::STOPPED_MS) {
      double r = cog * DEG_TO_RAD, zv[2] = {sog * stima::KN * sin(r), sog * stima::KN * cos(r)};
      innovation(2, zv, stima::GPS_SPEED_SD, y, si);
      update(2, y, si);
    } else if(sog * stima::KN < stima::STOPPED_MS) {
      double zv[2] = {0, 0};
      innovation(2, zv, 0.3 + sog * stima::KN, y, si);
      update(2, y, si);
    }
  }

  // The heading of the compass in degrees, magnetic or true: only its changes count
  void heading(double now, double h) { hdg = fmod(fmod(h, 360.0) + 360.0, 360.0); hdgTime = now; hasHdg = true; }

  // Once a second: carries the estimate on, and gives it up when it is too old or too wide
  void step(double now) {
    if(!ready || expired) return;
    predict(now);
    double a, b, th; ellipse(a, b, th);
    if(now - fixTime >= stima::FIX_S && (now - fixTime > maxS || a > stima::MAX_HALF_AXIS_M)) expired = true;
  }

  bool steered(double now) const { return hasHdg && now - hdgTime < stima::HDG_FRESH_S; }

  StimaState state(double now) {
    StimaState s;
    if(!ready) return s;
    s.hasAge = true;
    s.age = (long)max(0.0, now - fixTime);
    if(expired) return s;
    bool fix = now - fixTime < stima::FIX_S;
    strcpy(s.mode, fix ? "gps" : "dr");
    position(s.lat, s.lon);
    double a, b, th; ellipse(a, b, th);
    s.a = lround(a); s.b = lround(b); s.th = lround(th);
    double speed = hypot(x[2], x[3]);
    s.sog = speed / stima::KN;
    s.cog = fmod(atan2(x[2], x[3]) * RAD_TO_DEG + 360.0, 360.0);
    if(fix) {
      double e[2]; toEn(gpsLat, gpsLon, e);
      s.hasGap = true; s.gap = lround(hypot(e[0] - x[0], e[1] - x[1]));
    }
    s.jump = jump;
    s.hdg = steered(now);
    return s;
  }

private:
  double maxS;
  bool ready = false, expired = false;
  double x[4] = {0, 0, 0, 0};       // east, north (m from lat0, lon0), speed east, speed north (m/s)
  double p[4][4] = {{0}};
  double lat0 = 0, lon0 = 0, t = 0;
  bool hasFix = false;
  double fixTime = 0, gpsLat = 0, gpsLon = 0;
  bool jump = false;
  double jumpSince = 0;
  bool hasHdg = false, hasRef = false;
  double hdg = 0, hdgTime = -1e9, hdgRef = 0;

  static double gpsSd(double hdop) {
    if(hdop <= 0 || hdop >= 50 || isnan(hdop)) return 8.0;
    return max(3.0, min(hdop, 20.0) * 4.0);
  }

  void toEn(double lat, double lon, double* en) const {
    en[0] = (lon - lon0) * DEG_TO_RAD * stima::EARTH_M * cos(lat0 * DEG_TO_RAD);
    en[1] = (lat - lat0) * DEG_TO_RAD * stima::EARTH_M;
  }
  void toLl(double e, double n, double& lat, double& lon) const {
    lat = lat0 + n / stima::EARTH_M * RAD_TO_DEG;
    lon = lon0 + e / (stima::EARTH_M * cos(lat0 * DEG_TO_RAD)) * RAD_TO_DEG;
  }
  void position(double& lat, double& lon) const { toLl(x[0], x[1], lat, lon); }

  void recentre() {
    if(hypot(x[0], x[1]) > stima::RECENTRE_M) { position(lat0, lon0); x[0] = x[1] = 0; }
  }

  void start(double now, double lat, double lon, double sog, double cog, double sd) {
    lat0 = lat; lon0 = lon;
    bool moving = !isnan(cog) && sog * stima::KN >= stima::STOPPED_MS;
    double v = sog * stima::KN, r = (isnan(cog) ? 0 : cog) * DEG_TO_RAD;
    x[0] = x[1] = 0;
    x[2] = moving ? v * sin(r) : 0;
    x[3] = moving ? v * cos(r) : 0;
    double vv = moving ? stima::GPS_SPEED_SD * stima::GPS_SPEED_SD : 1.0;
    memset(p, 0, sizeof(p));
    p[0][0] = p[1][1] = sd * sd;
    p[2][2] = p[3][3] = vv;
    t = now;
    ready = true; expired = false; jump = false;
    hasRef = false;
  }

  // Turns the velocity, and what is known about it, clockwise
  void turn(double degrees) {
    double c = cos(degrees * DEG_TO_RAD), s = sin(degrees * DEG_TO_RAD);
    double x2 = x[2] * c + x[3] * s, x3 = -x[2] * s + x[3] * c;
    x[2] = x2; x[3] = x3;
    for(int j = 0; j < 4; j++) { double a = p[2][j] * c + p[3][j] * s, b = -p[2][j] * s + p[3][j] * c; p[2][j] = a; p[3][j] = b; }
    for(int i = 0; i < 4; i++) { double a = p[i][2] * c + p[i][3] * s, b = -p[i][2] * s + p[i][3] * c; p[i][2] = a; p[i][3] = b; }
  }

  void predict(double now) {
    double dt = now - t;
    if(dt <= 0) return;
    t = now;
    bool st = steered(now);
    if(!st) hasRef = false;             // a turn made meanwhile is lost
    else if(!hasRef) { hdgRef = hdg; hasRef = true; }
    else {
      double tr = fmod(hdg - hdgRef + 540.0, 360.0) - 180.0;
      hdgRef = hdg;
      if(tr != 0) turn(tr);
    }
    x[0] += x[2] * dt;
    x[1] += x[3] * dt;
    // P = F P F' with F = [[I, dt I], [0, I]]
    for(int i = 0; i < 2; i++) for(int j = 0; j < 4; j++) p[i][j] += dt * p[i + 2][j];
    for(int j = 0; j < 2; j++) for(int i = 0; i < 4; i++) p[i][j] += dt * p[i][j + 2];
    double speed = hypot(x[2], x[3]), a[2][2];
    if(speed >= stima::STOPPED_MS) {
      double ue = x[2] / speed, un = x[3] / speed, qc = st ? stima::Q_ACROSS_HDG : stima::Q_ACROSS;
      a[0][0] = stima::Q_ALONG * ue * ue + qc * un * un;
      a[0][1] = a[1][0] = (stima::Q_ALONG - qc) * ue * un;
      a[1][1] = stima::Q_ALONG * un * un + qc * ue * ue;
    } else {
      a[0][0] = a[1][1] = stima::Q_STOPPED; a[0][1] = a[1][0] = 0;
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

  // For a measure z of the states k and k+1: the difference y and the inverse of its covariance si
  void innovation(int k, const double* z, double sd, double* y, double* si) const {
    y[0] = z[0] - x[k]; y[1] = z[1] - x[k + 1];
    double s00 = p[k][k] + sd * sd, s01 = p[k][k + 1], s11 = p[k + 1][k + 1] + sd * sd;
    double det = s00 * s11 - s01 * s01;
    si[0] = s11 / det; si[1] = -s01 / det; si[2] = s00 / det;
  }

  void update(int k, const double* y, const double* si) {
    double g[4][2], rows[2][4];
    for(int i = 0; i < 4; i++) {
      g[i][0] = p[i][k] * si[0] + p[i][k + 1] * si[1];
      g[i][1] = p[i][k] * si[1] + p[i][k + 1] * si[2];
    }
    for(int i = 0; i < 4; i++) x[i] += g[i][0] * y[0] + g[i][1] * y[1];
    for(int j = 0; j < 4; j++) { rows[0][j] = p[k][j]; rows[1][j] = p[k + 1][j]; }
    for(int i = 0; i < 4; i++) for(int j = 0; j < 4; j++) p[i][j] -= g[i][0] * rows[0][j] + g[i][1] * rows[1][j];
    for(int i = 0; i < 4; i++) for(int j = 0; j < i; j++) p[i][j] = p[j][i] = (p[i][j] + p[j][i]) / 2.0;
  }

public:
  // Half axes in metres of the 95% ellipse and the bearing of the long one (0-180 degrees from north)
  void ellipse(double& a, double& b, double& th) const {
    double pee = p[0][0], pen = p[0][1], pnn = p[1][1];
    double mid = (pee + pnn) / 2.0, half = hypot((pee - pnn) / 2.0, pen);
    a = sqrt(stima::CHI2_95 * max(0.0, mid + half));
    b = sqrt(stima::CHI2_95 * max(0.0, mid - half));
    double east = 0.5 * atan2(2.0 * pen, pee - pnn);
    th = fmod(90.0 - east * RAD_TO_DEG + 360.0, 180.0);
  }
};
