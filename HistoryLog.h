// History for the web app:
//   MinuteLog - one record per minute for all probes, a month of it, in its own flash partition
//   AliveLog  - a "still alive" mark every few seconds, in its own flash partition, to time power cuts
//   SegmentLog - fixed-size records appended to two files on LittleFS (daily summaries, journal, coal refills)
// Nothing grows without bound: the newest data replaces the oldest, so the space never runs out.

#pragma once

#include <LittleFS.h>
#include <esp_partition.h>
#include <math.h>
#include <vector>

constexpr int16_t NO_VALUE = INT16_MIN;     // a probe without a reading in that minute / day

// Degrees x 100 in an int16: a flue thermocouple can pass 327 degrees, which is kept as 327 (not wrapped around,
// and clear of 0xFFFF, the minute log's "nothing written").
inline int16_t toHundredths(float v) {
  return isnan(v) ? NO_VALUE : (int16_t)lroundf(fminf(fmaxf(v, -327.0f), 327.0f) * 100);
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

    int32_t top = -1, prev = -1;
    uint32_t prevBase = 0;
    for (uint32_t s = 0; s < sectors; s++) {
      Header h;
      esp_partition_read(part, s * SECTOR, &h, sizeof(h));
      if (h.magic != MAGIC) continue;
      if (!first || h.base < first) first = h.base;
      if (top < 0 || h.base > topBase) {
        prev = top; prevBase = topBase;
        top = s; topBase = h.base;
      } else if (prev < 0 || h.base > prevBase) {
        prev = s; prevBase = h.base;
      }
    }
    if (top >= 0) last = lastWritten(top, topBase);
    // Power gone right after a sector was started, before its first record: the newest data is in the one before.
    if (!last && prev >= 0) last = lastWritten(prev, prevBase);
    return true;
  }

  uint32_t newest() const { return last; }
  uint32_t capacityMinutes() const { return capacity; }   // how far back the ring reaches when full

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
  // "MIN2" for 4 probes (the classic board's history stays readable); another width has its own, so a ring written
  // with other records never reads as this one's.
  static constexpr uint32_t MAGIC  = N == 4 ? 0x4D494E32 : 0x4D490000 | N;
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

  // Minute of the last written slot of a sector, 0 when it has none.
  uint32_t lastWritten(uint32_t sector, uint32_t base) const {
    uint32_t found = 0;
    Raw buf[32];
    for (uint32_t k = 0; k < SLOTS; k += 32) {
      uint32_t n = min((uint32_t)32, SLOTS - k);
      esp_partition_read(part, sector * SECTOR + sizeof(Header) + k * sizeof(Raw), buf, n * sizeof(Raw));
      for (uint32_t i = 0; i < n; i++)
        if (written(buf[i])) found = base + k + i;
    }
    return found;
  }

  const char*            label;
  const esp_partition_t* part     = nullptr;
  uint32_t               sectors  = 0;
  uint32_t               capacity = 0;
  uint32_t               first    = 0;   // oldest minute seen
  uint32_t               last     = 0;   // newest stored minute, 0 = empty
  uint32_t               topBase  = 0;
};

// "Still alive" marks: the Unix time every few seconds, one uint32 per slot of its own flash partition ("alive"),
// written into erased flash and a sector erased only when the ring comes back to it. After a power cut the
// newest mark says when the power went - to the second, which the minute log cannot.
// 16 KB at a mark every 2 s: each sector is erased every ~2.3 hours, ~4k times a year (flash lasts 100k).
class AliveLog {
public:
  explicit AliveLog(const char* label) : label(label) {}

  // Finds the newest mark - call before the first mark() of this boot. False when the partition is missing.
  bool begin() {
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!part) return false;
    slots = part->size / sizeof(uint32_t);
    uint32_t buf[64];
    for (uint32_t k = 0; k < slots; k += 64) {
      esp_partition_read(part, k * sizeof(uint32_t), buf, sizeof(buf));
      for (uint32_t i = 0; i < 64; i++)
        if (buf[i] != EMPTY && buf[i] > last) { last = buf[i]; next = (k + i + 1) % slots; }
    }
    return true;
  }

  // Time of the newest mark from before this boot's marks, 0 = none.
  uint32_t newest() const { return last; }

  void mark(uint32_t t) {
    if (!part) return;
    if (next % SLOTS_PER_SECTOR == 0) esp_partition_erase_range(part, next * sizeof(uint32_t), SECTOR);
    esp_partition_write(part, next * sizeof(uint32_t), &t, sizeof(t));
    next = (next + 1) % slots;
  }

  void clear() {
    if (part) esp_partition_erase_range(part, 0, part->size);
    last = next = 0;
  }

private:
  static constexpr uint32_t EMPTY            = 0xFFFFFFFF;
  static constexpr uint32_t SECTOR           = 4096;
  static constexpr uint32_t SLOTS_PER_SECTOR = SECTOR / sizeof(uint32_t);

  const char*            label;
  const esp_partition_t* part  = nullptr;
  uint32_t               slots = 0;
  uint32_t               last  = 0;
  uint32_t               next  = 0;
};

// Fixed-size records whose first field is a uint32 `time`, appended to two segment files <base>0.bin and
// <base>1.bin. LittleFS rewrites a file from the point of a write to its end (copy on write), so the old ring
// with its header at the front copied the whole file twice per record and ran out of room on the 62 KB daily
// file. An append touches only the last block. When the segment being written is full, the other one is emptied
// and writing goes on there - between perSegment and 2 x perSegment records are kept.
template <typename T> class SegmentLog {
public:
  SegmentLog(const char* base, uint32_t perSegment) : base(base), per(perSegment) {}

  void begin() {
    for (uint8_t s = 0; s < 2; s++) {
      File f = LittleFS.open(path(s), "r");
      size_t size = f ? f.size() : 0;
      if (f) f.close();
      count[s] = size / sizeof(T);
      if (size % sizeof(T)) rewrite(s, count[s]);         // a record cut short (no room) - drop it
      last[s] = count[s] ? at(s, count[s] - 1).time : 0;
    }
    cur = last[1] > last[0] ? 1 : 0;
  }

  void add(const T& r) {
    if (count[cur] >= per) {                              // full: the older segment makes room
      cur ^= 1;
      LittleFS.remove(path(cur));
      count[cur] = last[cur] = 0;
    }
    File f = LittleFS.open(path(cur), "a");
    if (!f) return;
    size_t n = f.write((const uint8_t*)&r, sizeof(T));
    f.close();
    if (n != sizeof(T)) {
      if (n) rewrite(cur, count[cur]);
      return;
    }
    count[cur]++;
    last[cur] = r.time;
  }

  // Several records in time order, one file open per segment they reach (a log that writes in batches).
  void addMany(const T* r, size_t n) {
    while (n) {
      if (count[cur] >= per) {
        cur ^= 1;
        LittleFS.remove(path(cur));
        count[cur] = last[cur] = 0;
      }
      size_t k = min(n, (size_t)(per - count[cur]));
      File f = LittleFS.open(path(cur), "a");
      if (!f) return;
      size_t written = f.write((const uint8_t*)r, k * sizeof(T));
      f.close();
      if (written != k * sizeof(T)) {                     // no room: whole records only
        rewrite(cur, count[cur] + written / sizeof(T));
        if (count[cur]) last[cur] = at(cur, count[cur] - 1).time;
        return;
      }
      count[cur] += k;
      last[cur] = r[k - 1].time;
      r += k;
      n -= k;
    }
  }

  // For a log written in time order: f(record) for since <= time < until, oldest first - the start found by a binary
  // search, not by reading everything. Returns the newest record before `since` (all zero when none): the state the
  // range begins in.
  template <typename F> T forRange(uint32_t since, uint32_t until, F f) const {
    T before = {};
    for (uint8_t s : {(uint8_t)(cur ^ 1), cur}) {
      if (!count[s]) continue;
      uint32_t lo = 0, hi = count[s];
      while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (at(s, mid).time < since) lo = mid + 1;
        else hi = mid;
      }
      if (lo) {
        T b = at(s, lo - 1);
        if (b.time >= before.time) before = b;
      }
      File file = LittleFS.open(path(s), "r");
      if (!file) continue;
      if (lo < count[s] && file.seek(lo * sizeof(T))) {
        T buf[16];
        for (uint32_t k = lo; k < count[s];) {
          uint32_t n = min((uint32_t)16, count[s] - k);
          if (file.read((uint8_t*)buf, n * sizeof(T)) != n * sizeof(T)) break;
          uint32_t i = 0;
          for (; i < n && buf[i].time < until; i++) f(buf[i]);
          if (i < n) break;
          k += n;
        }
      }
      file.close();
    }
    return before;
  }

  // Drops the record added last when its time is `time` (0 = whichever it is) - one entered by mistake.
  bool removeLast(uint32_t time = 0) {
    uint8_t s = count[cur] ? cur : cur ^ 1;
    if (!count[s] || (time && at(s, count[s] - 1).time != time)) return false;
    rewrite(s, count[s] - 1);
    last[s] = count[s] ? at(s, count[s] - 1).time : 0;
    return true;
  }

  // Every record through f once - a change of what a field means (a few hundred records, at boot). LittleFS
  // keeps the old file until the new one is closed.
  template <typename F> void update(F f) {
    for (uint8_t s = 0; s < 2; s++) {
      if (!count[s]) continue;
      std::vector<T> all(count[s]);
      File file = LittleFS.open(path(s), "r");
      if (!file) continue;
      size_t n = file.read((uint8_t*)all.data(), all.size() * sizeof(T));
      file.close();
      if (n != all.size() * sizeof(T)) continue;
      for (T& r : all) f(r);
      file = LittleFS.open(path(s), "w");
      if (!file) continue;
      file.write((const uint8_t*)all.data(), n);
      file.close();
    }
  }

  // All records at once, oldest segment first: f gets them as a std::vector<T>& to drop, add or sort, and they are
  // written back (at most 2 x perSegment, the newest kept) - a one-time repair at boot. Each segment goes to a new
  // file renamed over the old one, so a power cut leaves the old file or the new one.
  template <typename F> void edit(F f) {
    std::vector<T> all;
    forEach(0, [&](const T& r) { all.push_back(r); });
    f(all);
    size_t skip = all.size() > 2 * per ? all.size() - 2 * per : 0, n0 = all.size() - skip;
    if (n0 > per) n0 = per;
    const T* part[2] = {all.data() + skip, all.data() + skip + n0};
    uint32_t n[2] = {(uint32_t)n0, (uint32_t)(all.size() - skip - n0)};
    for (uint8_t s = 0; s < 2; s++) {
      String fresh = path(s) + ".new";
      File file = LittleFS.open(fresh, "w");
      if (!file) return;
      file.write((const uint8_t*)part[s], n[s] * sizeof(T));
      file.close();
      LittleFS.rename(fresh, path(s));
      count[s] = n[s];
      last[s] = n[s] ? part[s][n[s] - 1].time : 0;
    }
    cur = n[1] ? 1 : 0;
  }

  // Time of the record added last, 0 when empty.
  uint32_t newestTime() const { return count[cur] ? last[cur] : last[cur ^ 1]; }

  // f(record) for every record with time >= since, in the order they were added.
  template <typename F> void forEach(uint32_t since, F f) const {
    for (uint8_t s : {(uint8_t)(cur ^ 1), cur}) {
      if (!count[s]) continue;
      File file = LittleFS.open(path(s), "r");
      if (!file) continue;
      T buf[16];
      for (uint32_t k = 0; k < count[s];) {
        uint32_t n = min((uint32_t)16, count[s] - k);
        if (file.read((uint8_t*)buf, n * sizeof(T)) != n * sizeof(T)) break;
        for (uint32_t i = 0; i < n; i++)
          if (buf[i].time >= since) f(buf[i]);
        k += n;
      }
      file.close();
    }
  }

private:
  const char* base;
  uint32_t    per;
  uint32_t    count[2] = {};
  uint32_t    last[2]  = {};
  uint8_t     cur      = 0;

  String path(uint8_t s) const { return String(base) + s + ".bin"; }

  T at(uint8_t s, uint32_t k) const {
    T r = {};
    File f = LittleFS.open(path(s), "r");
    if (f && f.seek(k * sizeof(T))) f.read((uint8_t*)&r, sizeof(T));
    if (f) f.close();
    return r;
  }

  // Keeps the first n records of segment s (a small file - the refills - or a rare repair).
  void rewrite(uint8_t s, uint32_t n) {
    std::vector<T> keep(n);
    File f = LittleFS.open(path(s), "r");
    if (f) {
      f.read((uint8_t*)keep.data(), n * sizeof(T));
      f.close();
    }
    f = LittleFS.open(path(s), "w");
    if (f) {
      f.write((const uint8_t*)keep.data(), n * sizeof(T));
      f.close();
    }
    count[s] = n;
  }
};