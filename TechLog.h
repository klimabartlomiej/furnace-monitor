// Technical log: the firmware's own messages (what it prints through techLog - also to Serial), the newest ~2 KB
// kept in RTC memory, which survives a crash, a watchdog or a software restart (not a power cut). The diagnostic
// report (/api/diag) shows it: what happened before a problem on a device far away. Every start is marked with its
// reason; lines get the day and time once the clock is set, seconds since boot before. Never log a secret here -
// the report is meant to be sent to someone.

#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include <time.h>

class TechLog : public Print {
public:
  static constexpr size_t SIZE = 2040;

  // At the very start of setup(): keeps what survived the restart and marks this start.
  void begin() {
    if (ring.magic != MAGIC || ring.head >= SIZE) ring = {MAGIC, 0, 0, {}};
    printf("--- start: %s\n", startReason());
  }

  static const char* startReason() {
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:  return "power on";
      case ESP_RST_SW:       return "restart";
      case ESP_RST_PANIC:    return "CRASH (panic)";
      case ESP_RST_INT_WDT:
      case ESP_RST_TASK_WDT:
      case ESP_RST_WDT:      return "CRASH (watchdog)";
      case ESP_RST_BROWNOUT: return "brownout";
      default:               return "other reset";
    }
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* p, size_t n) override {
    Serial.write(p, n);
    char stamp[16];
    time_t t = time(nullptr);
    if (t > 1600000000) {
      struct tm lt;
      localtime_r(&t, &lt);
      snprintf(stamp, sizeof(stamp), "%02d %02d:%02d ", lt.tm_mday, lt.tm_hour, lt.tm_min);
    } else {
      snprintf(stamp, sizeof(stamp), "+%lus ", (unsigned long)(millis() / 1000));
    }
    portENTER_CRITICAL(&mux);                            // the update and relay tasks log too
    for (size_t i = 0; i < n; i++) {
      if (lineStart)
        for (const char* s = stamp; *s; s++) put(*s);
      put(p[i]);
      lineStart = p[i] == '\n';
    }
    portEXIT_CRITICAL(&mux);
    return n;
  }

  // The ring, oldest first.
  String text() const {
    String out;
    out.reserve(SIZE);
    portENTER_CRITICAL(&mux);
    if (ring.full)
      for (size_t i = ring.head; i < SIZE; i++) out += ring.text[i];
    for (size_t i = 0; i < ring.head; i++) out += ring.text[i];
    portEXIT_CRITICAL(&mux);
    return out;
  }

private:
  static constexpr uint32_t MAGIC = 0x544C4F47;   // "TLOG"
  struct Ring {
    uint32_t magic;
    uint16_t head;      // where the next character goes
    uint16_t full;      // the ring has wrapped
    char     text[SIZE];
  };
  static Ring ring;
  mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  bool lineStart = true;

  void put(char c) {
    ring.text[ring.head++] = c;
    if (ring.head >= SIZE) {
      ring.head = 0;
      ring.full = 1;
    }
  }
};

RTC_NOINIT_ATTR TechLog::Ring TechLog::ring;
TechLog techLog;
