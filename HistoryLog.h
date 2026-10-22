// History for the web app:
//   MinuteLog - one record per minute for all probes, a month of it, in its own flash partition
//   RingFile  - fixed-size records in a ring file on LittleFS (daily summaries for 5 years, the event journal)
// Everything is a ring: when full, the newest record overwrites the oldest, so the space never runs out.

#pragma once

#include <LittleFS.h>
#include <esp_partition.h>
#include <math.h>

constexpr int16_t NO_VALUE = INT16_MIN;     // a probe without a reading in that minute / day

inline int16_t toHundredths(float v) {
  return isnan(v) ? NO_VALUE : (int16_t)lroundf(v * 100);
}

// One record per minute for N probes, kept straight in its own flash partition ("minutes" in partitions.csv):
// a month is too big for LittleFS, which copies the whole rest of a file on every write into its middle.
// Each 4 KB sector holds an 8-byte header (magic, minute of its first slot) and 511 records of N x uint16
// (degrees x 100 + 32768; 0xFFFF = nothing written). The ring erases a sector when it reaches it again, and a
// slot whose sector header belongs to another lap reads as empty - old data never shows up as new.
template <size_t N> class MinuteLog {
public:
  struct Record {
    int16_t v[N];       // degrees C x 100 or NO_VALUE
  };

  explicit MinuteLog(const char* label) : label(label) {}

  // Finds the partition and the newest stored minute. False when the partition table has no "minutes".
  bool begin() {
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!part) return false;
    sectors = part->size / SECTOR;
    capacity = sectors * SLOTS;

    int32_t top = -1;
    for (uint32_t s = 0; s < sectors; s++) {
      Header h;
      esp_partition_read(part, s * SECTOR, &h, sizeof(h));
      if (h.magic != MAGIC) continue;
      if (!first || h.base < first) first = h.base;
      if (top < 0 || h.base > topBase) { top = s; topBase = h.base; }
    }
    if (top >= 0) {
      Raw buf[32];
      for (uint32_t k = 0; k < SLOTS; k += 32) {
        uint32_t n = min((uint32_t)32, SLOTS - k);
        esp_partition_read(part, top * SECTOR + sizeof(Header) + k * sizeof(Raw), buf, n * sizeof(Raw));
        for (uint32_t i = 0; i < n; i++)
          if (written(buf[i])) last = topBase + k + i;
      }
    }
    return true;
  }

  uint32_t newest() const { return last; }

  uint32_t oldest() const {
    if (!last) return 0;
    return max(first, last >= capacity ? last - capacity + 1 : 1);
  }

  // Erases the whole partition (history wiped from the web app or by a full reset).
  void clear() {
    if (part) esp_partition_erase_range(part, 0, part->size);
    first = last = topBase = 0;
  }

  // Stores the record of `minute` (Unix time / 60). Flash is written once per lap: a minute already stored
  // (two readings in one minute) or older than the ring is skipped.
  void add(uint32_t minute, const Record& r) {
    if (!part || (last && minute + capacity <= last)) return;
    uint32_t s = minute % capacity, sector = s / SLOTS, slot = s % SLOTS, base = minute - slot;
    Header h;
    esp_partition_read(part, sector * SECTOR, &h, sizeof(h));
    if (h.magic != MAGIC || h.base != base) {              // first minute of this lap in the sector
      esp_partition_erase_range(part, sector * SECTOR, SECTOR);
      h = {MAGIC, base};
      esp_partition_write(part, sector * SECTOR, &h, sizeof(h));
    }
    size_t at = sector * SECTOR + sizeof(Header) + slot * sizeof(Raw);
    Raw raw;
    esp_partition_read(part, at, &raw, sizeof(raw));
    if (written(raw)) return;
    for (size_t i = 0; i < N; i++) raw.v[i] = r.v[i] == NO_VALUE ? 0xFFFF : (uint16_t)(r.v[i] + 32768);
    esp_partition_write(part, at, &raw, sizeof(raw));
    if (minute > last) last = minute;
    if (!first || minute < first) first = minute;
  }

  // f(minute, record) for every minute in [from, to] the ring still covers, oldest first; empty minutes included.
  template <typename F> void forEach(uint32_t from, uint32_t to, F f) const {
    if (!part || !last) return;
    from = max(from, oldest());
    to = min(to, last);
    Raw buf[32];
    for (uint32_t m = from; m <= to && m >= from;) {
      uint32_t s = m % capacity, sector = s / SLOTS, slot = s % SLOTS;
      uint32_t n = min(SLOTS - slot, to - m + 1);          // the rest of this sector
      Header h;
      esp_partition_read(part, sector * SECTOR, &h, sizeof(h));
      bool valid = h.magic == MAGIC && h.base == m - slot;
      for (uint32_t done = 0; done < n;) {
        uint32_t k = min(n - done, (uint32_t)32);
        if (valid) esp_partition_read(part, sector * SECTOR + sizeof(Header) + (slot + done) * sizeof(Raw), buf, k * sizeof(Raw));
        for (uint32_t i = 0; i < k; i++) {
          Record r;
          for (size_t p = 0; p < N; p++)
            r.v[p] = !valid || buf[i].v[p] == 0xFFFF ? NO_VALUE : (int16_t)(buf[i].v[p] - 32768);
          f(m + done + i, r);
        }
        done += k;
      }
      m += n;
    }
  }

private:
  static constexpr uint32_t MAGIC  = 0x4D494E32;   // "MIN2"
  static constexpr uint32_t SECTOR = 4096;

  struct Header {
    uint32_t magic;
    uint32_t base;      // minute of the sector's first slot in its current lap
  };
  struct Raw {
    uint16_t v[N];
  };
  static constexpr uint32_t SLOTS = (SECTOR - sizeof(Header)) / sizeof(Raw);

  static bool written(const Raw& r) {
    for (size_t i = 0; i < N; i++)
      if (r.v[i] != 0xFFFF) return true;
    return false;
  }

  const char*            label;
  const esp_partition_t* part     = nullptr;
  uint32_t               sectors  = 0;
  uint32_t               capacity = 0;
  uint32_t               first    = 0;   // oldest minute seen
  uint32_t               last     = 0;   // newest stored minute, 0 = empty
  uint32_t               topBase  = 0;
};

// Fixed-size records whose first field is a uint32 `time`, in a ring file [Header][capacity x T].
template <typename T> class RingFile {
public:
  RingFile(const char* path, uint32_t capacity) : path(path), capacity(capacity) {}

  void begin() {
    File f = LittleFS.open(path, "r");
    bool ok = f && f.size() == sizeof(Header) + (size_t)capacity * sizeof(T)
              && f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == MAGIC && h.capacity == capacity && h.recordSize == sizeof(T);
    if (f) f.close();
    if (ok) return;
    h = {MAGIC, (uint32_t)sizeof(T), capacity, 0};
    f = LittleFS.open(path, "w");
    if (!f) return;
    f.write((const uint8_t*)&h, sizeof(h));
    uint8_t zeros[256] = {};
    for (size_t left = (size_t)capacity * sizeof(T); left;) {
      size_t n = min(left, sizeof(zeros));
      f.write(zeros, n);
      left -= n;
    }
    f.close();
  }

  void add(const T& r) {
    File f = LittleFS.open(path, "r+");
    if (!f) return;
    f.seek(sizeof(Header) + (h.count % capacity) * sizeof(T));
    f.write((const uint8_t*)&r, sizeof(r));
    h.count++;
    f.seek(0);
    f.write((const uint8_t*)&h, sizeof(h));
    f.close();
  }

  // Time of the newest record, 0 when empty.
  uint32_t newestTime() const {
    if (!h.count) return 0;
    T r = {};
    File f = LittleFS.open(path, "r");
    if (f && f.seek(sizeof(Header) + ((h.count - 1) % capacity) * sizeof(T))) f.read((uint8_t*)&r, sizeof(r));
    if (f) f.close();
    return r.time;
  }

  // f(record) for every record with time >= since, oldest first.
  template <typename F> void forEach(uint32_t since, F f) const {
    File file = LittleFS.open(path, "r");
    if (!file) return;
    uint32_t stored = min(h.count, capacity);
    T buf[16];
    for (uint32_t k = h.count - stored; k < h.count;) {
      uint32_t slot = k % capacity;
      uint32_t n = min({(uint32_t)16, h.count - k, capacity - slot});
      file.seek(sizeof(Header) + slot * sizeof(T));
      file.read((uint8_t*)buf, n * sizeof(T));
      for (uint32_t i = 0; i < n; i++)
        if (buf[i].time >= since) f(buf[i]);
      k += n;
    }
    file.close();
  }

private:
  static constexpr uint32_t MAGIC = 0x52494E47;   // "RING"

  struct Header {
    uint32_t magic;
    uint32_t recordSize;
    uint32_t capacity;
    uint32_t count;       // records ever written
  };

  const char* path;
  uint32_t    capacity;
  Header      h = {};
};
