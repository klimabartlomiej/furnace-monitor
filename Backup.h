// Backup: the whole device in one file, for moving to a new board. Restored on the new one it keeps the history,
// the settings, the WiFi, the HomeKit pairing (the Home app sees the same accessory, nothing to add again) and the
// remote-access key (the paired phones keep working). The old board must be off by then - two boards with one
// HomeKit identity confuse the Home app.
//
//   GET  /api/backup   the file (home WiFi only: it holds the WiFi password and the pairing keys)
//   POST /api/restore  that file, multipart; applied as it arrives, then the ESP restarts
//
// Format (little-endian): "PDBK" | version u8 | probes u8 | firmware u32 | created u32, then sections
//   type u8 | name length u8 | name | data length u32 | data
//   1 NVS entry   name "namespace/key", data = NVS type u8 + the value (a string without its 0)
//   2 file        name = LittleFS path, data = the file
//   3 minutes     name "minutes", data = (minute u32 + probes x int16) per stored minute, oldest first
//   0 end
// The minutes go as records, not as the raw partition: the new board's ring may be of another size.
// A backup of fewer probes (the classic board's 4 into the S3's 8 slots) is taken: its minutes get the missing
// probes as "no value", and NVS "furnace/fromProbes" tells the sketch to widen the files it knows (daily summaries)
// at the next start.

#pragma once

#include <LittleFS.h>
#include <nvs.h>
#include <vector>
#include "HistoryLog.h"

template <size_t N> class Backup {
public:
  explicit Backup(MinuteLog<N>& minutes) : minutes(minutes) {}

  // NVS namespaces that travel: the sketch's own and HomeSpan's (WiFi, pairing, setup code, OTA password, saved
  // characteristics). Not "update" - the update state belongs to the old board.
  static constexpr const char* NAMESPACES[] = {"furnace", "HAP", "SRP", "WIFI", "OTA", "CHAR"};

  // The file into w (anything with put(const void*, size_t)).
  template <typename W> void write(W& w, uint32_t firmware) {
    uint8_t head[14] = {'P', 'D', 'B', 'K', VERSION, (uint8_t)N};
    put32(head + 6, firmware);
    put32(head + 10, time(nullptr));
    w.put(head, sizeof(head));
    for (const char* ns : NAMESPACES) writeNamespace(w, ns);
    writeFiles(w, "/");
    size_t count = 0;
    minutes.forEach(minutes.oldest(), minutes.newest(), [&](uint32_t, const typename MinuteLog<N>::Record& r) {
      if (stored(r)) count++;
    });
    section(w, 3, "minutes", count * (4 + 2 * N));
    minutes.forEach(minutes.oldest(), minutes.newest(), [&](uint32_t m, const typename MinuteLog<N>::Record& r) {
      if (!stored(r)) return;
      uint8_t b[4 + 2 * N];
      put32(b, m);
      memcpy(b + 4, r.v, 2 * N);
      w.put(b, sizeof(b));
    });
    uint8_t end = 0;
    w.put(&end, 1);
  }

  // ---- restore: begin(), feed() with every piece of the upload, end() -> "" or what went wrong

  void begin() {
    stage = MAGIC;
    need = 14;
    have = 0;
    error = "";
    erased.clear();
    formatted = cleared = false;
    probes = N;
  }

  void feed(const uint8_t* p, size_t n) {
    while (n && error.isEmpty() && stage != DONE) {
      if (stage == DATA) {
        size_t k = min(n, left);
        data(p, k);
        p += k;
        n -= k;
        left -= k;
        if (!left) finishSection();
        continue;
      }
      size_t k = min(n, need - have);
      memcpy(buf + have, p, k);
      have += k;
      p += k;
      n -= k;
      if (have == need) header();
    }
  }

  String end() {
    if (error.isEmpty() && stage != DONE) error = "the file ends too early";
    if (file) file.close();
    nvs_handle_t h;
    if (error.isEmpty() && probes < N && nvs_open("furnace", NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_u8(h, "fromProbes", probes);
      nvs_commit(h);
      nvs_close(h);
    }
    return error;
  }

  // Anything written yet - a restore that failed after this leaves a mix, so the ESP restarts anyway.
  bool changed() const { return formatted || cleared || !erased.empty(); }

private:
  static constexpr uint8_t VERSION = 1;
  enum Stage : uint8_t { MAGIC, TYPE, NAME, DATA, DONE };

  MinuteLog<N>& minutes;
  Stage    stage = MAGIC;
  uint8_t  buf[300];          // a header being read (name up to 255 + data length), or an NVS value
  size_t   need = 0, have = 0;
  uint8_t  type = 0;
  char     name[256];
  size_t   left = 0;          // data bytes of the current section still to come
  std::vector<uint8_t> value; // the NVS entry being read
  uint8_t  probes = N;        // in the backup being restored
  uint8_t  record[4 + 2 * N];
  size_t   recordHave = 0;
  File     file;
  String   error;
  std::vector<String> erased; // namespaces emptied before their first entry
  bool     formatted = false;
  bool     cleared   = false;

  static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
  static uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

  static bool stored(const typename MinuteLog<N>::Record& r) {
    for (size_t i = 0; i < N; i++)
      if (r.v[i] != NO_VALUE) return true;
    return false;
  }

  template <typename W> static void section(W& w, uint8_t type, const char* name, uint32_t length) {
    uint8_t h[2] = {type, (uint8_t)strlen(name)};
    uint8_t len[4];
    put32(len, length);
    w.put(h, 2);
    w.put(name, h[1]);
    w.put(len, 4);
  }

  template <typename W> void writeNamespace(W& w, const char* ns) {
    nvs_iterator_t it = nullptr;
    for (esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it); e == ESP_OK; e = nvs_entry_next(&it)) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      if (!strcmp(ns, "furnace") && (!strcmp(info.key, "wipe") || !strcmp(info.key, "cleared"))) continue;   // one-off flags
      std::vector<uint8_t> v;
      if (!readValue(ns, info.key, info.type, v)) continue;
      String key = String(ns) + "/" + info.key;
      section(w, 1, key.c_str(), 1 + v.size());
      uint8_t t = info.type;
      w.put(&t, 1);
      w.put(v.data(), v.size());
    }
    nvs_release_iterator(it);
  }

  static bool readValue(const char* ns, const char* key, nvs_type_t type, std::vector<uint8_t>& v) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = ESP_FAIL;
    union { uint8_t u8; int8_t i8; uint16_t u16; int16_t i16; uint32_t u32; int32_t i32; uint64_t u64; int64_t i64; } x;
    size_t size = 0;
    switch (type) {
      case NVS_TYPE_U8:  e = nvs_get_u8(h, key, &x.u8);   size = 1; break;
      case NVS_TYPE_I8:  e = nvs_get_i8(h, key, &x.i8);   size = 1; break;
      case NVS_TYPE_U16: e = nvs_get_u16(h, key, &x.u16); size = 2; break;
      case NVS_TYPE_I16: e = nvs_get_i16(h, key, &x.i16); size = 2; break;
      case NVS_TYPE_U32: e = nvs_get_u32(h, key, &x.u32); size = 4; break;
      case NVS_TYPE_I32: e = nvs_get_i32(h, key, &x.i32); size = 4; break;
      case NVS_TYPE_U64: e = nvs_get_u64(h, key, &x.u64); size = 8; break;
      case NVS_TYPE_I64: e = nvs_get_i64(h, key, &x.i64); size = 8; break;
      case NVS_TYPE_STR:
        if ((e = nvs_get_str(h, key, nullptr, &size)) == ESP_OK && size) {
          v.resize(size);
          e = nvs_get_str(h, key, (char*)v.data(), &size);
          v.pop_back();                                      // the terminating 0
        }
        break;
      case NVS_TYPE_BLOB:
        if ((e = nvs_get_blob(h, key, nullptr, &size)) == ESP_OK) {
          v.resize(size);
          e = nvs_get_blob(h, key, v.data(), &size);
        }
        break;
      default: break;
    }
    nvs_close(h);
    if (e == ESP_OK && type != NVS_TYPE_STR && type != NVS_TYPE_BLOB) v.assign((uint8_t*)&x, (uint8_t*)&x + size);
    return e == ESP_OK;
  }

  template <typename W> void writeFiles(W& w, const char* dir) {
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      String path = f.path();
      if (f.isDirectory()) {
        f.close();
        writeFiles(w, path.c_str());
        continue;
      }
      section(w, 2, path.c_str(), f.size());
      uint8_t b[512];
      for (int n; (n = f.read(b, sizeof(b))) > 0;) w.put(b, n);
      f.close();
    }
  }

  void header() {
    if (stage == MAGIC) {
      if (memcmp(buf, "PDBK", 4) || buf[4] != VERSION) error = "not a backup of this device";
      else if (buf[5] > N || !buf[5]) error = "a backup of more probes than this board has";
      probes = buf[5];
      stage = TYPE;
      need = 2;
    } else if (stage == TYPE) {
      type = buf[0];
      if (!type) {
        stage = DONE;
        return;
      }
      stage = NAME;
      need = buf[1] + 4;
    } else {                                                 // NAME: the name and the data length
      memcpy(name, buf, need - 4);
      name[need - 4] = 0;
      left = get32(buf + need - 4);
      startSection();
      stage = DATA;
      if (!left) finishSection();
    }
    have = 0;
  }

  void startSection() {
    value.clear();
    recordHave = 0;
    if (type == 1 && left > 4097) error = "NVS entry too big";
    if (type == 2) {
      if (!formatted) {                                      // the old board's files replace whatever is here
        LittleFS.format();
        formatted = true;
      }
      file = LittleFS.open(name, "w", true);
      if (!file) error = String("cannot write ") + name;
    }
    if (type == 3) {
      minutes.clear();
      cleared = true;
    }
  }

  void data(const uint8_t* p, size_t n) {
    if (type == 1) {
      value.insert(value.end(), p, p + n);
    } else if (type == 2) {
      if (file.write(p, n) != n) error = String("disk full at ") + name;
    } else if (type == 3) {
      for (size_t i = 0; i < n; i++) {
        record[recordHave++] = p[i];
        if (recordHave < 4 + 2 * probes) continue;
        typename MinuteLog<N>::Record r;
        for (size_t k = 0; k < N; k++) r.v[k] = NO_VALUE;
        memcpy(r.v, record + 4, 2 * probes);
        minutes.add(get32(record), r);
        recordHave = 0;
      }
    }
  }

  void finishSection() {
    if (type == 1) restoreEntry();
    if (type == 2) file.close();
    stage = TYPE;
    need = 2;
    have = 0;
  }

  void restoreEntry() {
    char* slash = strchr(name, '/');
    if (!slash || value.empty()) return;
    *slash = 0;
    const char *ns = name, *key = slash + 1;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
      error = String("cannot open ") + ns;
      return;
    }
    bool first = true;
    for (const String& e : erased) first &= e != ns;
    if (first) {                                             // what the new board had in it goes
      nvs_erase_all(h);
      erased.push_back(ns);
    }
    const uint8_t* v = value.data() + 1;
    size_t n = value.size() - 1;
    uint64_t x = 0;
    memcpy(&x, v, min(n, sizeof(x)));
    switch (value[0]) {
      case NVS_TYPE_U8:  nvs_set_u8(h, key, (uint8_t)x);   break;
      case NVS_TYPE_I8:  nvs_set_i8(h, key, (int8_t)x);    break;
      case NVS_TYPE_U16: nvs_set_u16(h, key, (uint16_t)x); break;
      case NVS_TYPE_I16: nvs_set_i16(h, key, (int16_t)x);  break;
      case NVS_TYPE_U32: nvs_set_u32(h, key, (uint32_t)x); break;
      case NVS_TYPE_I32: nvs_set_i32(h, key, (int32_t)x);  break;
      case NVS_TYPE_U64: nvs_set_u64(h, key, x);           break;
      case NVS_TYPE_I64: nvs_set_i64(h, key, (int64_t)x);  break;
      case NVS_TYPE_STR: nvs_set_str(h, key, String((const char*)v, n).c_str()); break;
      case NVS_TYPE_BLOB: nvs_set_blob(h, key, v, n);     break;
      default: break;
    }
    nvs_commit(h);
    nvs_close(h);
  }
};
