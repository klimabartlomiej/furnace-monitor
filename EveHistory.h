// Eve app history for one temperature sensor, kept on the ESP32 itself (no Homebridge needed).
//
// Implements the Elgato Eve history protocol as reverse-engineered by fakegato-history
// (github.com/simont77/fakegato-history), "weather" type with temperature only:
//   E863F116 status   - read : reference time, entry signature, used / total memory, first entry
//   E863F11C request  - write: Eve asks for entries starting at an address
//   E863F117 entries  - read : up to 11 entries per read, then "00" once everything was sent
//   E863F121 set time - write: Eve's clock; ignored, the ESP keeps time through NTP
// Entries live in a ring file on LittleFS (one per sensor), so the history survives reboots and updates.
// The Eve app keeps what it has downloaded, so the ring only has to cover the time between two app opens.

#pragma once

#include <HomeSpan.h>
#include <LittleFS.h>
#include <vector>

CUSTOM_SERV(EveHistoryService, E863F007-079E-48FF-8F27-9C2605A29F52);
CUSTOM_CHAR_DATA(EveHistoryStatus, E863F116-079E-48FF-8F27-9C2605A29F52, PR+EV+HD);
CUSTOM_CHAR_DATA(EveHistoryEntries, E863F117-079E-48FF-8F27-9C2605A29F52, PR+EV+HD);
CUSTOM_CHAR_DATA(EveHistoryRequest, E863F11C-079E-48FF-8F27-9C2605A29F52, PW+HD);
CUSTOM_CHAR_DATA(EveSetTime, E863F121-079E-48FF-8F27-9C2605A29F52, PW+HD);

class EveHistory : public Service::EveHistoryService {
public:
  static constexpr uint32_t EPOCH_OFFSET = 978307200;   // 2001-01-01 (Eve's epoch) in Unix time
  static constexpr uint16_t MEMORY_SIZE  = 4032;        // entries per sensor: 28 days at 10 min, same as fakegato
  static constexpr uint8_t  BATCH        = 11;          // entries per read, same as fakegato

  // name: "<accessory> History" - HAP-NodeJS (Homebridge) gives every service a Name and Eve ignores the
  // history service without one.
  EveHistory(const char* name, const char* path) : path(path) {
    new Characteristic::Name(name);
    status  = new Characteristic::EveHistoryStatus();
    entries = new Characteristic::EveHistoryEntries();
    request = new Characteristic::EveHistoryRequest();
    setTime = new Characteristic::EveSetTime();
    load();
    instances().push_back(this);
  }

  // Appends one measurement (Unix time, degrees C) - fakegato _addEntry.
  void add(uint32_t unixTime, float celsius) {
    if (h.usedMemory < MEMORY_SIZE) {
      h.usedMemory++;
      h.firstEntry = 0;
      h.lastEntry  = h.usedMemory;
    } else {
      h.firstEntry++;
      h.lastEntry = h.firstEntry + h.usedMemory;
    }
    if (h.refTime == 0) {                 // very first entry: a reference-time marker goes in front of it
      h.refTime = unixTime - EPOCH_OFFSET;
      writeRecord(h.lastEntry, {unixTime, REF_MARK});
      h.lastEntry++;
      h.usedMemory++;
    }
    writeRecord(h.lastEntry, {unixTime, (int16_t)lroundf(celsius * 100)});
    lastTime = unixTime;
    publishStatus(true);
    Serial.printf("[Eve] %s: entry %u = %.2f C\n", path, (unsigned)h.lastEntry, celsius);
  }

  uint32_t entryCount() const { return h.usedMemory; }

  // Calls f(unixTime, temp100) for every stored measurement with time >= since, oldest first (web app charts).
  template <typename F> void forEach(uint32_t since, F f) const {
    if (!h.refTime) return;
    File file = LittleFS.open(path, "r");
    if (!file) return;
    Record buf[32];
    for (uint32_t e = h.firstEntry + 1; e <= h.lastEntry;) {
      uint32_t slot = e % MEMORY_SIZE;
      uint32_t n = min({(uint32_t)32, h.lastEntry + 1 - e, (uint32_t)MEMORY_SIZE - slot});
      file.seek(recordOffset(e));
      file.read((uint8_t*)buf, n * sizeof(Record));
      for (uint32_t i = 0; i < n; i++)
        if (buf[i].temp100 != REF_MARK && buf[i].time >= since) f(buf[i].time, buf[i].temp100);
      e += n;
    }
    file.close();
  }

  // HomeSpan calls this before it answers any read; every history whose entries are being read gets
  // its next batch loaded first, so consecutive reads hand out consecutive batches.
  static void onGetCharacteristics(const char* url) {
    const char* p = strstr(url, "id=");
    if (!p) return;
    p += 3;
    while (*p && *p != '&' && *p != ' ') {
      unsigned aid = 0, iid = 0;
      if (sscanf(p, "%u.%u", &aid, &iid) == 2)
        for (EveHistory* hist : instances())
          if (hist->entries->getAID() == aid && hist->entries->getIID() == iid) hist->serveNextBatch();
      const char* next = strpbrk(p, ",& ");
      if (!next || *next != ',') break;
      p = next + 1;
    }
  }

  boolean update() override {
    if (request->updated()) {
      uint8_t buf[32];
      size_t n = request->getNewData(buf, sizeof(buf));
      // Bytes 2..5 hold the requested address, little-endian; 0 means "from the beginning".
      uint32_t address = n >= 6 ? (uint32_t)buf[2] | buf[3] << 8 | buf[4] << 16 | (uint32_t)buf[5] << 24 : 0;
      currentEntry = address ? address : 1;
      transfer = true;
      Serial.printf("[Eve] %s: Eve asks for entries from %u (last is %u)\n", path, (unsigned)currentEntry, (unsigned)h.lastEntry);
    }
    return true;
  }

private:
  static constexpr uint32_t MAGIC    = 0x45564833;   // "EVH3" - bump when the file layout or MEMORY_SIZE changes
  static constexpr int16_t  REF_MARK = INT16_MIN;    // stored instead of a temperature for a reference-time marker

  struct Header {
    uint32_t magic, refTime, usedMemory, firstEntry, lastEntry;
  };
  struct __attribute__((packed)) Record {
    uint32_t time;       // Unix time
    int16_t  temp100;    // degrees C x 100, or REF_MARK
  };

  const char*         path;
  SpanCharacteristic* status;
  SpanCharacteristic* entries;
  SpanCharacteristic* request;
  SpanCharacteristic* setTime;
  Header   h            = {};
  uint32_t lastTime     = 0;
  uint32_t currentEntry = 1;
  bool     transfer     = false;
  bool     sendRefTime  = true;    // the first batch after a reboot starts with a reference-time record

  static std::vector<EveHistory*>& instances() {
    static std::vector<EveHistory*> list;
    return list;
  }

  static size_t put16(uint8_t* p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
    return 2;
  }

  static size_t put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
    return 4;
  }

  size_t recordOffset(uint32_t entry) const {
    return sizeof(Header) + (entry % MEMORY_SIZE) * sizeof(Record);
  }

  void load() {
    File f = LittleFS.open(path, "r");
    bool ok = f && f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == MAGIC;
    if (f) f.close();
    if (!ok) {
      create();
      return;
    }
    if (h.refTime) {
      lastTime = readRecord(h.lastEntry).time;
      publishStatus(false);
    }
  }

  void create() {
    h = {MAGIC, 0, 0, 0, 0};
    File f = LittleFS.open(path, "w");
    if (!f) return;
    f.write((const uint8_t*)&h, sizeof(h));
    uint8_t zeros[256] = {};
    for (size_t left = (size_t)MEMORY_SIZE * sizeof(Record); left;) {
      size_t n = min(left, sizeof(zeros));
      f.write(zeros, n);
      left -= n;
    }
    f.close();
  }

  Record readRecord(uint32_t entry) const {
    Record r = {};
    File f = LittleFS.open(path, "r");
    if (f && f.seek(recordOffset(entry))) f.read((uint8_t*)&r, sizeof(r));
    if (f) f.close();
    return r;
  }

  void writeRecord(uint32_t entry, const Record& r) {
    File f = LittleFS.open(path, "r+");
    if (!f) return;
    f.seek(recordOffset(entry));
    f.write((const uint8_t*)&r, sizeof(r));
    f.seek(0);
    f.write((const uint8_t*)&h, sizeof(h));
    f.close();
  }

  // fakegato: '%s00000000%s%s%s%s%s000000000101'
  void publishStatus(bool notify) {
    static const uint8_t SIGNATURE[] = {0x03, 0x01, 0x02, 0x02, 0x02, 0x03, 0x02};   // weather: temp, humidity, pressure
    static const uint8_t TAIL[]      = {0x00, 0x00, 0x00, 0x00, 0x01, 0x01};
    bool full = h.usedMemory >= MEMORY_SIZE;

    uint8_t s[40];
    size_t n = 0;
    n += put32(s + n, lastTime - h.refTime - EPOCH_OFFSET);
    n += put32(s + n, 0);
    n += put32(s + n, h.refTime);
    memcpy(s + n, SIGNATURE, sizeof(SIGNATURE));
    n += sizeof(SIGNATURE);
    n += put16(s + n, full ? h.usedMemory : h.usedMemory + 1);
    n += put16(s + n, MEMORY_SIZE);
    n += put32(s + n, full ? h.firstEntry + 1 : h.firstEntry);
    memcpy(s + n, TAIL, sizeof(TAIL));
    n += sizeof(TAIL);
    status->setData(s, n, notify);
  }

  // fakegato getCurrentS2R2
  void serveNextBatch() {
    uint8_t out[BATCH * 21];
    size_t n = 0;

    if (transfer && currentEntry <= h.lastEntry) {
      File f = LittleFS.open(path, "r");
      for (uint8_t i = 0; i < BATCH && currentEntry <= h.lastEntry; i++, currentEntry++) {
        Record r = {};
        if (f && f.seek(recordOffset(currentEntry))) f.read((uint8_t*)&r, sizeof(r));

        if (r.temp100 == REF_MARK || sendRefTime || currentEntry == h.firstEntry + 1) {
          // ",15%s 0100 0000 81%s0000 0000 00 0000"
          out[n++] = 0x15;
          n += put32(out + n, currentEntry);
          n += put32(out + n, 1);
          out[n++] = 0x81;
          n += put32(out + n, h.refTime);
          memset(out + n, 0, 7);
          n += 7;
          sendRefTime = false;
        } else {
          // ",10 %s%s-%s:%s %s %s" - entry, seconds since reference, type 07, temp x100, humidity, pressure
          out[n++] = 0x10;
          n += put32(out + n, currentEntry);
          n += put32(out + n, r.time - h.refTime - EPOCH_OFFSET);
          out[n++] = 0x07;
          n += put16(out + n, (uint16_t)r.temp100);
          n += put16(out + n, 0);
          n += put16(out + n, 0);
        }
      }
      if (f) f.close();
      Serial.printf("[Eve] %s: sent entries up to %u (%u bytes)\n", path, (unsigned)(currentEntry - 1), (unsigned)n);
    } else {
      if (transfer) Serial.printf("[Eve] %s: transfer complete\n", path);
      transfer = false;
      out[n++] = 0x00;
    }
    entries->setData(out, n, false);
  }
};
