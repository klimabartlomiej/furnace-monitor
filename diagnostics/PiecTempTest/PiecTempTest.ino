// Furnace sensor diagnostics
// Full health check of the DS18B20 1-Wire bus on D22 (GPIO22):
//   1. bus line   - idle voltage (ADC pins only), rise time, presence pulses
//   2. search     - which probes answer and how reliably
//   3. each probe - power mode, scratchpad, 10 timed conversions on its own
//   4. monitor    - the real-firmware pattern (one broadcast conversion, then read all) every 3 s
// Serial Monitor 115200 baud. Send "r" to run the whole check again.

#include <algorithm>
#include <climits>
#include <OneWire.h>
#include <esp_system.h>
#include "soc/gpio_reg.h"

constexpr uint8_t  PIN                   = 22;     // D22 on the board
constexpr uint8_t  MAX_PROBES            = 8;
constexpr uint8_t  SEARCH_ROUNDS         = 20;
constexpr uint8_t  CONVERSIONS_PER_PROBE = 10;
constexpr uint32_t MONITOR_PERIOD_MS     = 3000;

OneWire ow(PIN);

enum ReadResult { RES_OK, RES_NO_PRESENCE, RES_ALL_ONES, RES_ALL_ZEROS, RES_CRC, RES_POR85, RES_COUNT };
const char* RESULT_NAME[RES_COUNT] = { "ok", "no-presence", "no-answer(FF)", "all-zeros", "crc-error", "85.0(power-on)" };

struct Probe {
  uint8_t  rom[8];
  uint8_t  seen;                 // search rounds that found it
  bool     parasite;
  uint32_t results[RES_COUNT];     // monitor statistics
  float    tMin, tMax;
};

Probe    probes[MAX_PROBES];
uint8_t  probeCount  = 0;
uint32_t cycle       = 0;
uint32_t lastCycleMs = 0;

// ------------------------------------------------------------------ helpers

String romString(const uint8_t* r) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
  return buf;
}

const char* familyName(uint8_t f) {
  switch (f) {
    case 0x28: return "DS18B20";
    case 0x10: return "DS18S20";
    case 0x22: return "DS1822";
    case 0x3B: return "MAX31850";
    default:   return "unknown family";
  }
}

// Genuine Maxim DS18B20 ROMs end with 00-00 before the CRC; most clones do not.
const char* romHint(const uint8_t* r) {
  return (r[5] == 0x00 && r[6] == 0x00) ? "Maxim-like ROM" : "clone-like ROM";
}

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply voltage dipped)";
    case ESP_RST_EXT:      return "external (EN pin / USB)";
    case ESP_RST_DEEPSLEEP:return "deep sleep wake";
    default:               return "other";
  }
}

int findProbe(const uint8_t* rom) {
  for (uint8_t i = 0; i < probeCount; i++)
    if (memcmp(probes[i].rom, rom, 8) == 0) return i;
  return -1;
}

// UINT32_MAX when the pin has no ADC (GPIO22 does not).
uint32_t idleMillivolts() {
  if (digitalPinToAnalogChannel(PIN) < 0) return UINT32_MAX;
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) sum += analogReadMilliVolts(PIN);
  pinMode(PIN, INPUT);           // hand the pin back to the digital 1-Wire driver
  return sum / 32;
}

// Time for the line to return high after a 10 us low pulse: pull-up strength vs cable capacitance.
uint32_t riseTimeNs() {
  REG_WRITE(GPIO_OUT_W1TC_REG, BIT(PIN));
  REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(PIN));
  delayMicroseconds(10);
  uint32_t mhz   = ESP.getCpuFreqMHz();
  uint32_t start = ESP.getCycleCount();
  REG_WRITE(GPIO_ENABLE_W1TC_REG, BIT(PIN));
  while (!(REG_READ(GPIO_IN_REG) & BIT(PIN)))
    if (ESP.getCycleCount() - start > mhz * 1000) return UINT32_MAX;   // still low after 1 ms
  return (ESP.getCycleCount() - start) * 1000 / mhz;
}

bool readScratchpad(const uint8_t* rom, uint8_t* sp) {
  if (!ow.reset()) return false;
  ow.select(rom);
  ow.write(0xBE);
  for (int i = 0; i < 9; i++) sp[i] = ow.read();
  return true;
}

ReadResult readTemp(const uint8_t* rom, uint8_t* sp, float& t) {
  if (!readScratchpad(rom, sp)) return RES_NO_PRESENCE;
  bool ones = true, zeros = true;
  for (int i = 0; i < 9; i++) {
    if (sp[i] != 0xFF) ones = false;
    if (sp[i] != 0x00) zeros = false;
  }
  if (ones)  return RES_ALL_ONES;       // nobody drove the line: probe did not answer
  if (zeros) return RES_ALL_ZEROS;      // line held low for the whole read: short / leak on the bus
  if (OneWire::crc8(sp, 8) != sp[8]) return RES_CRC;
  int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
  if (rom[0] == 0x10) raw = raw << 3;  // DS18S20 reports 9-bit
  t = raw / 16.0f;
  return raw == 0x0550 ? RES_POR85 : RES_OK;
}

String scratchpadString(const uint8_t* sp) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%02X %02X %02X %02X %02X %02X %02X %02X %02X",
           sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7], sp[8]);
  return buf;
}

// Starts a conversion on one probe and waits for it. Returns ms, -1 on timeout, -2 when nobody answered the reset.
int convertOne(const Probe& p) {
  if (!ow.reset()) return -2;
  ow.select(p.rom);
  if (p.parasite) {
    ow.write(0x44, 1);   // keep the line powered during the conversion
    delay(800);
    ow.depower();
    return 800;
  }
  ow.write(0x44);
  uint32_t start = millis();
  while (!ow.read_bit())
    if (millis() - start > 1500) return -1;
  return millis() - start;
}

// ------------------------------------------------------------------ 1. bus line

void lineTests() {
  Serial.printf("\n=== 1. Bus line (GPIO%u) ===\n", PIN);

  uint32_t mv = idleMillivolts();
  if (mv == UINT32_MAX) Serial.println("[LINE] idle voltage: n/a (no ADC on this pin)");
  else Serial.printf("[LINE] idle voltage: %u mV (healthy: 3100-3300)\n", (unsigned)mv);
  Serial.printf("[LINE] digital level at rest: %s\n", digitalRead(PIN) ? "HIGH" : "LOW");

  uint32_t samples[21];
  for (int i = 0; i < 21; i++) {
    samples[i] = riseTimeNs();
    delayMicroseconds(200);
  }
  std::sort(samples, samples + 21);
  if (samples[10] == UINT32_MAX)
    Serial.println("[LINE] rise time: line never returns HIGH - something holds it low");
  else
    Serial.printf("[LINE] rise time: min %u ns, median %u ns, max %u ns (short cable < 2000, long cable 2000-5000)\n",
                  (unsigned)samples[0], (unsigned)samples[10], samples[20] == UINT32_MAX ? 0u : (unsigned)samples[20]);

  uint8_t present = 0, lowBefore = 0;
  for (int i = 0; i < 100; i++) {
    if (!digitalRead(PIN)) lowBefore++;
    if (ow.reset()) present++;
    delay(2);
  }
  Serial.printf("[LINE] presence pulse: %u/100 resets, line LOW before reset: %u/100\n", present, lowBefore);
}

// ------------------------------------------------------------------ 2. search

void searchTests() {
  Serial.printf("\n=== 2. Search stability (%u rounds) ===\n", SEARCH_ROUNDS);
  probeCount = 0;
  uint16_t badCrc = 0, emptyRounds = 0;
  uint8_t  perRoundMin = 255, perRoundMax = 0;

  for (uint8_t round = 0; round < SEARCH_ROUNDS; round++) {
    uint8_t rom[8], found = 0;
    ow.reset_search();
    while (ow.search(rom)) {
      if (OneWire::crc8(rom, 7) != rom[7]) {
        badCrc++;
        continue;
      }
      found++;
      int idx = findProbe(rom);
      if (idx < 0 && probeCount < MAX_PROBES) {
        idx = probeCount++;
        memset(&probes[idx], 0, sizeof(Probe));
        memcpy(probes[idx].rom, rom, 8);
      }
      if (idx >= 0) probes[idx].seen++;
    }
    if (!found) emptyRounds++;
    perRoundMin = min(perRoundMin, found);
    perRoundMax = max(perRoundMax, found);
    delay(5);
  }

  Serial.printf("[SEARCH] unique probes: %u | probes per round: %u..%u | garbled ROMs (bad CRC): %u | empty rounds: %u/%u\n",
                probeCount, perRoundMin, perRoundMax, badCrc, emptyRounds, SEARCH_ROUNDS);
  for (uint8_t i = 0; i < probeCount; i++)
    Serial.printf("[SEARCH] probe %u: %s  %s, %s, found in %u/%u rounds\n", i + 1, romString(probes[i].rom).c_str(),
                  familyName(probes[i].rom[0]), romHint(probes[i].rom), probes[i].seen, SEARCH_ROUNDS);
}

// ------------------------------------------------------------------ 3. each probe on its own

void probeTests() {
  Serial.println("\n=== 3. Each probe on its own ===");
  for (uint8_t n = 0; n < probeCount; n++) {
    Probe& p = probes[n];

    bool answered = ow.reset();
    ow.select(p.rom);
    ow.write(0xB4);                      // Read Power Supply: parasite probes pull the slot low
    p.parasite = answered && !ow.read_bit();
    Serial.printf("\n[PROBE %u] %s  power: %s\n", n + 1, romString(p.rom).c_str(),
                  p.parasite ? "PARASITE (2-wire)" : "external VDD (3-wire)");

    uint8_t sp[9];
    float t = NAN;
    ReadResult r = readTemp(p.rom, sp, t);
    Serial.printf("[PROBE %u] scratchpad at rest: %s -> %s", n + 1, scratchpadString(sp).c_str(), RESULT_NAME[r]);
    if (r == RES_OK || r == RES_POR85)
      Serial.printf(", %.2f C, resolution %u bit, byte5 %s, byte7 %s",
                    t, 9 + ((sp[4] >> 5) & 3), sp[5] == 0xFF ? "FF ok" : "UNUSUAL", sp[7] == 0x10 ? "10 ok" : "UNUSUAL");
    Serial.println();

    uint8_t  counts[RES_COUNT] = {};
    int      msMin = INT_MAX, msMax = INT_MIN, timeouts = 0, noReset = 0;
    float    tMin = 999, tMax = -999;
    for (uint8_t k = 0; k < CONVERSIONS_PER_PROBE; k++) {
      int ms = convertOne(p);
      if (ms == -1) timeouts++;
      if (ms == -2) noReset++;
      if (ms >= 0) {
        msMin = min(msMin, ms);
        msMax = max(msMax, ms);
      }
      r = readTemp(p.rom, sp, t);
      counts[r]++;
      if (r == RES_OK) {
        tMin = min(tMin, t);
        tMax = max(tMax, t);
      }
      Serial.printf("[PROBE %u]   #%u  conversion %5d ms  %-15s", n + 1, k + 1, ms, RESULT_NAME[r]);
      if (r == RES_OK || r == RES_POR85) Serial.printf(" %.2f C", t);
      else Serial.printf(" raw %s", scratchpadString(sp).c_str());
      Serial.println();
    }

    Serial.printf("[PROBE %u] SUMMARY ok %u/%u", n + 1, counts[RES_OK], CONVERSIONS_PER_PROBE);
    for (int i = 1; i < RES_COUNT; i++)
      if (counts[i]) Serial.printf(", %s %u", RESULT_NAME[i], counts[i]);
    if (msMin != INT_MAX) Serial.printf(" | conversion %d..%d ms (genuine 12-bit ~600-750)", msMin, msMax);
    if (timeouts) Serial.printf(" | conversion TIMEOUT %d", timeouts);
    if (noReset) Serial.printf(" | no presence %d", noReset);
    if (counts[RES_OK]) Serial.printf(" | temp %.2f..%.2f C", tMin, tMax);
    Serial.println();
  }
}

// ------------------------------------------------------------------ 4. monitor

void monitorCycle() {
  bool anyParasite = false;
  for (uint8_t i = 0; i < probeCount; i++) anyParasite |= probes[i].parasite;

  // Same pattern as the real firmware: one broadcast conversion for every probe, then read each one.
  bool present = ow.reset();
  ow.skip();
  ow.write(0x44, anyParasite ? 1 : 0);
  delay(800);
  if (anyParasite) ow.depower();

  cycle++;
  Serial.printf("[CYCLE %3u]%s", (unsigned)cycle, present ? "" : " (no presence on broadcast)");
  for (uint8_t i = 0; i < probeCount; i++) {
    Probe& p = probes[i];
    uint8_t sp[9];
    float t = NAN;
    ReadResult r = readTemp(p.rom, sp, t);
    p.results[r]++;
    if (r == RES_OK) {
      p.tMin = min(p.tMin, t);
      p.tMax = max(p.tMax, t);
      Serial.printf("  #%u %6.2f", i + 1, t);
    } else {
      Serial.printf("  #%u %s", i + 1, RESULT_NAME[r]);
    }
  }
  Serial.println();

  if (cycle % 10 == 0) {
    Serial.printf("[MONITOR] after %u cycles\n", (unsigned)cycle);
    for (uint8_t i = 0; i < probeCount; i++) {
      Probe& p = probes[i];
      Serial.printf("[MONITOR] probe %u %s: ok %u/%u", i + 1, romString(p.rom).c_str(), (unsigned)p.results[RES_OK], (unsigned)cycle);
      for (int k = 1; k < RES_COUNT; k++)
        if (p.results[k]) Serial.printf(", %s %u", RESULT_NAME[k], (unsigned)p.results[k]);
      if (p.results[RES_OK]) Serial.printf(" | %.2f..%.2f C", p.tMin, p.tMax);
      Serial.println();
    }
  }
}

// ------------------------------------------------------------------ setup / loop

void runFullCheck() {
  lineTests();
  searchTests();
  if (probeCount == 0) {
    Serial.println("\n[RESULT] no probe answered - check wiring, pull-up resistor and the pin");
  } else {
    probeTests();
  }
  for (uint8_t i = 0; i < probeCount; i++) {
    memset(probes[i].results, 0, sizeof(probes[i].results));
    probes[i].tMin = 999;
    probes[i].tMax = -999;
  }
  cycle = 0;
  Serial.printf("\n=== 4. Continuous monitor (broadcast conversion every %u s) ===\n", (unsigned)(MONITOR_PERIOD_MS / 1000));
  lastCycleMs = millis();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n##### Furnace sensor diagnostics #####");
  Serial.printf("[BOARD] %s rev %u, %u cores, %u MHz, flash %u MB, reset reason: %s\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), (unsigned)ESP.getCpuFreqMHz(),
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)), resetReasonName(esp_reset_reason()));
  runFullCheck();
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') runFullCheck();
  if (probeCount && millis() - lastCycleMs >= MONITOR_PERIOD_MS) {
    lastCycleMs = millis();
    monitorCycle();
  }
}
