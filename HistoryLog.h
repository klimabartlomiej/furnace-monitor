// Long-term history for the web app: per probe, hourly and daily min / avg / max in ring files on LittleFS.
// A ring file is [Header][capacity x AggRecord]; once full, the oldest record is overwritten.

#pragma once

#include <LittleFS.h>
#include <math.h>

struct __attribute__((packed)) AggRecord {
  uint32_t time;     // Unix time of the period start
  int16_t  min100;   // degrees C x 100
  int16_t  avg100;
  int16_t  max100;
};

// Running min / avg / max of the readings in one period.
struct Aggregate {
  float    sum = 0;
  float    min = INFINITY;
  float    max = -INFINITY;
  uint16_t count = 0;

  void add(float t) {
    sum += t;
    min = fminf(min, t);
    max = fmaxf(max, t);
    count++;
  }

  void add(const AggRecord& r) {     // merge a finer record (hours into a day)
    sum += r.avg100 / 100.0f;
    min = fminf(min, r.min100 / 100.0f);
    max = fmaxf(max, r.max100 / 100.0f);
    count++;
  }

  AggRecord record(uint32_t start) const {
    return {start, (int16_t)lroundf(min * 100), (int16_t)lroundf(sum / count * 100), (int16_t)lroundf(max * 100)};
  }

  void reset() { *this = Aggregate(); }
};

class RingLog {
public:
  RingLog(const char* path, uint16_t capacity) : path(path), capacity(capacity) {}

  void begin() {
    File f = LittleFS.open(path, "r");
    bool ok = f && f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == MAGIC && h.capacity == capacity;
    if (f) f.close();
    if (ok) return;
    h = {MAGIC, capacity, 0};
    f = LittleFS.open(path, "w");
    if (!f) return;
    f.write((const uint8_t*)&h, sizeof(h));
    uint8_t zeros[256] = {};
    for (size_t left = (size_t)capacity * sizeof(AggRecord); left;) {
      size_t n = min(left, sizeof(zeros));
      f.write(zeros, n);
      left -= n;
    }
    f.close();
  }

  void add(const AggRecord& r) {
    File f = LittleFS.open(path, "r+");
    if (!f) return;
    f.seek(sizeof(Header) + (h.count % capacity) * sizeof(AggRecord));
    f.write((const uint8_t*)&r, sizeof(r));
    h.count++;
    f.seek(0);
    f.write((const uint8_t*)&h, sizeof(h));
    f.close();
  }

  // Calls f(record) for every record with time >= since, oldest first.
  template <typename F> void forEach(uint32_t since, F f) const {
    File file = LittleFS.open(path, "r");
    if (!file) return;
    uint32_t stored = min(h.count, (uint32_t)capacity);
    uint32_t first  = h.count - stored;                 // oldest record still in the ring
    AggRecord buf[32];
    for (uint32_t k = first; k < h.count;) {
      uint32_t slot = k % capacity;
      uint32_t n = min({(uint32_t)32, h.count - k, capacity - slot});   // stop at the end of the file, then wrap
      file.seek(sizeof(Header) + slot * sizeof(AggRecord));
      file.read((uint8_t*)buf, n * sizeof(AggRecord));
      for (uint32_t i = 0; i < n; i++)
        if (buf[i].time >= since) f(buf[i]);
      k += n;
    }
    file.close();
  }

private:
  static constexpr uint32_t MAGIC = 0x41474731;   // "AGG1"

  struct Header {
    uint32_t magic;
    uint32_t capacity;
    uint32_t count;      // records ever written
  };

  const char* path;
  uint16_t    capacity;
  Header      h = {};
};
