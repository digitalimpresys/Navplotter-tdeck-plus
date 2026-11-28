// The compass: a BNO08x (BNO080, BNO085, BNO086) on an I2C bus.
// The same reader as navplotter/imu.py in Navplotter-Linux: the few messages of the chip's protocol (SHTP) that
// are needed, written out, with no library. The chip fuses its accelerometer, gyroscope and magnetometer by itself
// and gives the orientation of the board as a quaternion (the "rotation vector", referred to magnetic north and to
// gravity). From it come, once a second and averaged over that second:
//
//   hdg    heading in degrees, magnetic, of the X axis of the board (the arrow printed on it), plus the offset
//   roll   degrees, positive when the right side of the board goes down (X forward: heel to starboard)
//   pitch  degrees, positive when the X axis points up (X forward: bow up)
//   acc    how well the chip thinks it is calibrated: 0 not at all, 1 low, 2 medium, 3 high
//
// The bus is a template parameter with read(buf, n) and write(buf, n), so that this file has no Arduino in it and
// can be checked on a computer against the Python one.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace bno {

const uint8_t ADDRESSES[2] = {0x4A, 0x4B};
const uint8_t CH_EXE = 1, CH_CONTROL = 2, CH_REPORTS = 3;
const uint8_t SET_FEATURE = 0xFD, COMMAND_REQUEST = 0xF2, BASE_TIMESTAMP = 0xFB, TIMESTAMP_REBASE = 0xFA;
const uint8_t ROTATION_VECTOR = 0x05, ME_CALIBRATE = 0x07;
const uint32_t REPORT_US = 100000;   // ten readings a second
const double R2D = 180.0 / M_PI;

inline double pmod(double a, double b) {
  double r = fmod(a, b);
  if(r != 0 && ((r < 0) != (b < 0))) r += b;
  return r;
}

// (heading, roll, pitch) in degrees of a board whose quaternion, in the east-north-up frame, is this.
// False for a null quaternion.
inline bool angles(double i, double j, double k, double real, double& hdg, double& roll, double& pitch) {
  double n = sqrt(i * i + j * j + k * k + real * real);
  if(n < 1e-6) return false;
  i /= n; j /= n; k /= n; real /= n;
  double east = 1.0 - 2.0 * (j * j + k * k), north = 2.0 * (i * j + real * k), up = 2.0 * (i * k - real * j);
  hdg = pmod(atan2(east, north) * R2D, 360.0);
  pitch = asin(up < -1.0 ? -1.0 : up > 1.0 ? 1.0 : up) * R2D;
  roll = atan2(2.0 * (real * i + j * k), 1.0 - 2.0 * (i * i + j * j)) * R2D;
  return true;
}

// The average of the readings of a second: headings as arrows, so that 359 and 1 give 0 and not 180
struct Mean {
  int n = 0, acc = 3;
  double s = 0, c = 0, roll = 0, pitch = 0;
  void clear() { n = 0; s = c = roll = pitch = 0; acc = 3; }
  void add(double hdg, double r, double p, int a) {
    n++;
    s += sin(hdg / R2D);
    c += cos(hdg / R2D);
    roll += r;
    pitch += p;
    if(a < acc) acc = a;          // the worst of the second
  }
  // False without readings; then starts again
  bool take(double& hdg, double& r, double& p, int& a) {
    if(!n) return false;
    hdg = pmod(atan2(s, c) * R2D, 360.0);
    r = roll / n;
    p = pitch / n;
    a = acc;
    clear();
    return true;
  }
};

// Bytes of each report that may come in a packet of channel 3: anything else ends the reading of that packet
inline int reportBytes(uint8_t id) {
  if(id == ROTATION_VECTOR) return 14;
  if(id == BASE_TIMESTAMP || id == TIMESTAMP_REBASE) return 5;
  return -1;
}

template <class Bus>
class Bno08x {
public:
  explicit Bno08x(Bus& b) : bus(b) { memset(seq, 0, sizeof(seq)); }

  void send(uint8_t channel, const uint8_t* data, int n) {
    uint8_t out[32];
    int len = n + 4;
    out[0] = len & 0xFF; out[1] = len >> 8; out[2] = channel; out[3] = seq[channel];
    memcpy(out + 4, data, n);
    bus.write(out, len);
    seq[channel]++;
  }

  // One packet: its channel, and its data (without the header) in buf, at most bufSize bytes.
  // Returns the length of the data, or -1 when the chip has nothing to say.
  int read(uint8_t& channel, uint8_t* buf, int bufSize) {
    uint8_t head[4];
    if(bus.read(head, 4) < 4) return -1;
    int size = (head[0] | (head[1] << 8)) & 0x7FFF;
    if(size <= 4 || size == 0x7FFF) return -1;     // nothing, or the bus read as all ones: no chip answering
    // The chip sends the packet again from its header when it is read again
    int got = bus.readPacket(size, buf, bufSize, channel);
    if(got < 0 || channel > 5) return -1;
    return got;
  }

  // Resets the chip and asks for the rotation vector. `sleep` waits that many milliseconds.
  template <class Sleep>
  void start(Sleep sleep) {
    uint8_t reset = 1;
    send(CH_EXE, &reset, 1);
    sleep(500);
    uint8_t ch, buf[64];
    for(int i = 0; i < 20; i++) if(read(ch, buf, sizeof(buf)) < 0) break;   // what it says after a reset is of no use
    uint8_t cal[12] = {COMMAND_REQUEST, 0, ME_CALIBRATE, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    send(CH_CONTROL, cal, sizeof(cal));
    sleep(50);
    uint8_t feat[17] = {SET_FEATURE, ROTATION_VECTOR, 0, 0, 0,
                        (uint8_t)(REPORT_US & 0xFF), (uint8_t)((REPORT_US >> 8) & 0xFF), (uint8_t)((REPORT_US >> 16) & 0xFF), (uint8_t)(REPORT_US >> 24),
                        0, 0, 0, 0, 0, 0, 0, 0};
    send(CH_CONTROL, feat, sizeof(feat));
  }

  // The readings waiting in the chip, oldest first: calls f(hdg, roll, pitch, acc) for each. Returns how many.
  template <class F>
  int readings(F f) {
    int count = 0;
    uint8_t ch, d[64];
    for(int p = 0; p < 8; p++) {
      int n = read(ch, d, sizeof(d));
      if(n < 0) break;
      if(ch != CH_REPORTS) continue;
      int at = 0;
      while(at < n) {
        int size = reportBytes(d[at]);
        if(size < 0 || at + size > n) break;
        if(d[at] == ROTATION_VECTOR) {
          int16_t q[4];
          for(int k = 0; k < 4; k++) q[k] = (int16_t)(d[at + 4 + 2 * k] | (d[at + 5 + 2 * k] << 8));
          double h, r, pt;
          if(angles(q[0] / 16384.0, q[1] / 16384.0, q[2] / 16384.0, q[3] / 16384.0, h, r, pt)) {
            f(h, r, pt, d[at + 2] & 0x03);
            count++;
          }
        }
        at += size;
      }
    }
    return count;
  }

private:
  Bus& bus;
  uint8_t seq[6];
};

}  // namespace bno
