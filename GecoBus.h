// The Geco GH11TB controller's room-panel bus (terminals 21 A / 22 B), READ ONLY - docs/geco-gh11tb-bus.md.
// The controller announces its state for a GA01HA panel every ~57 ms, also when no panel is attached:
//   9600 8N1, every byte bit-inverted, every piece ends with 0x23 (inverted). Used frames (inverted):
//     40 T1 T2 T3 .. .. .. .. .. CK      temperatures x 2 (T1 boiler, T2 feeder, T3 hot water), 0xfa = no sensor
//     50 .. .. P3 .. .. OUT MODE .. CK   P3 0x40 piston away from home; OUT 0x40 fan, 0x20 feeder, 0x02 CO pump;
//                                        MODE 0x04 standby, 0x08 burning, 0x10 podtrzymanie
//   CK = sum(bytes 0..8) - 0x10.
// The UART has no TX pin and the RS485 module's driver is held off: nothing can reach the controller.
// A task of its own reads the line, so a busy loop() loses no frame; every change of the state is queued as an
// Event (to a tenth of a second) for the work log, and so is the bus going quiet (mode 0) and coming back.
// Until the first frame is heard the task also works out why not, for /api/now (the board is far from anyone with
// a meter): what the RX pin does, the line the other way round when it carries data but no frame comes (A and B
// swapped), and the bus on another free pin when the RX pin is silent (RO on the wrong pin) - listening there.

#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

class GecoBus {
public:
  enum Mode : uint8_t { NO_DATA = 0, STANDBY = 0x04, BURNING = 0x08, HOLDING = 0x10 };
  enum Output : uint8_t { FAN = 0x40, FEEDER = 0x20, CO_PUMP = 0x02 };
  static constexpr uint8_t PISTON_AWAY = 0x40;               // in flags
  static constexpr uint32_t QUIET_MS  = 5000;                 // no valid frame this long = the controller is gone

  struct __attribute__((packed)) Event {
    uint32_t time;       // Unix time
    uint8_t  tenths;     // + tenths of the second
    uint8_t  mode;       // Mode, NO_DATA = the bus went quiet
    uint8_t  outputs;    // the raw OUT byte (unknown bits kept)
    uint8_t  flags;      // the raw P3 byte
  };

  // scan: free pins (nothing else on them, no strapping pin) where the bus is looked for when the RX pin is silent.
  void begin(int8_t rxPin, const int8_t* scan = nullptr, uint8_t scanCount = 0) {
    if (rxPin < 0) return;
    scanPins = scan;
    scanPinCount = scanCount;
    events = xQueueCreate(256, sizeof(Event));
    listenOn(rxPin);
    xTaskCreatePinnedToCore(task, "geco", 4096, this, 1, nullptr, 0);
  }

  // A controller temperature (1 boiler, 2 feeder, 3 hot water), NAN without a recent frame or without the sensor.
  float temp(uint8_t channel) const {
    if (channel < 1 || channel > 3 || !fresh()) return NAN;
    portENTER_CRITICAL(&mux);
    uint8_t v = temps[channel - 1];
    portEXIT_CRITICAL(&mux);
    return v == 0xFA || v == 0 ? NAN : v / 2.0f;
  }

  bool fresh() const { return lastFrameMs && millis() - lastFrameMs < QUIET_MS; }
  uint32_t lastFrame() const { return lastFrameMs; }
  uint32_t goodFrames() const { return good; }
  uint32_t badFrames() const { return bad; }

  // The connection check: bytes received at all, the pin listened on, the line read the other way round, what the
  // RX pin did at the last check ('d' data, 'h' / 'l' held high / low, 'f' nothing drives it, 0 not checked yet)
  // and the last bytes as they came (hex).
  uint32_t rawBytes() const { return bytes; }
  int8_t   pin() const { return rxPin; }
  bool     inverted() const { return invert; }
  char     line() const { return lineState; }
  String rawHex() const {
    uint8_t copy[sizeof(raw)], at;
    portENTER_CRITICAL(&mux);
    memcpy(copy, raw, sizeof(raw));
    at = rawAt;
    portEXIT_CRITICAL(&mux);
    String s;
    uint32_t n = bytes < sizeof(raw) ? bytes : sizeof(raw);
    for (uint32_t k = 0; k < n; k++) {
      char h[3];
      snprintf(h, sizeof(h), "%02x", copy[(at + sizeof(raw) - n + k) % sizeof(raw)]);
      s += h;
    }
    return s;
  }

  Event state() const {
    portENTER_CRITICAL(&mux);
    Event e = current;
    portEXIT_CRITICAL(&mux);
    return e;
  }

  // The next queued change, false when none.
  bool nextEvent(Event& e) { return events && xQueueReceive(events, &e, 0) == pdTRUE; }

private:
  static constexpr uint32_t CHECK_MS      = 30 * 1000;       // a check while nothing has been heard ...
  static constexpr uint32_t CHECK_SLOW_MS = 5 * 60 * 1000;   // ... every 5 minutes after the first 10
  static constexpr uint32_t DATA_EDGES    = 20;              // level changes in a sample that mean data

  mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  QueueHandle_t events = nullptr;
  Event    current = {};
  uint8_t  temps[3] = {0xFA, 0xFA, 0xFA};
  volatile uint32_t lastFrameMs = 0;
  volatile uint32_t good = 0, bad = 0, bytes = 0;
  uint8_t  piece[24];
  uint8_t  have = 0;
  uint8_t  raw[16];
  uint8_t  rawAt = 0;
  volatile int8_t rxPin = -1;
  volatile bool invert = false;
  volatile char lineState = 0;
  const int8_t* scanPins = nullptr;
  uint8_t  scanPinCount = 0;
  uint32_t lastCheckMs = 0;

  struct Sample { uint32_t edges; bool high; };

  static void task(void* self) { static_cast<GecoBus*>(self)->run(); }

  void listenOn(int8_t p) {
    if (rxPin >= 0) Serial1.end();
    Serial1.setRxBufferSize(2048);
    Serial1.begin(9600, SERIAL_8N1, p, -1);
    Serial1.setRxInvert(invert);
    rxPin = p;
  }

  void run() {
    for (;;) {
      while (Serial1.available()) {
        uint8_t r = Serial1.read();
        portENTER_CRITICAL(&mux);
        raw[rawAt] = r;
        rawAt = (rawAt + 1) % sizeof(raw);
        portEXIT_CRITICAL(&mux);
        bytes++;
        uint8_t b = r ^ 0xFF;
        if (have < sizeof(piece)) piece[have++] = b;
        if (b == 0x23) {                                   // a piece ends
          take(piece, have);
          have = 0;
        }
      }
      if (current.mode != NO_DATA && !fresh()) changed(NO_DATA, 0, 0);   // the controller went quiet
      // Once a frame has been heard the wiring is right - silence then is the controller switched off.
      if (!good && millis() - lastCheckMs >= (millis() < 10 * 60 * 1000 ? CHECK_MS : CHECK_SLOW_MS)) diagnose();
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  void diagnose() {
    lastCheckMs = millis();
    lineState = probe(rxPin);
    if (lineState == 'd') {                                // data, yet no frame for a whole check: A and B swapped?
      invert = !invert;
      Serial1.setRxInvert(invert);
      return;
    }
    for (uint8_t k = 0; k < scanPinCount; k++) {          // the RX pin is silent: is the bus on another pin?
      gpio_num_t p = (gpio_num_t)scanPins[k];
      gpio_reset_pin(p);
      gpio_set_direction(p, GPIO_MODE_INPUT);
      gpio_set_pull_mode(p, GPIO_PULLDOWN_ONLY);
      delayMicroseconds(200);
      Sample s = sample(p, 3);
      gpio_reset_pin(p);
      if (s.edges >= DATA_EDGES) {
        listenOn(scanPins[k]);
        return;
      }
    }
  }

  // The RX pin with the pull-up and then the pull-down: toggling = data; the same level both times = something
  // drives it (the module's receiver output); following the pull = nothing does (no power, or RO not on this pin).
  char probe(int8_t p) {
    gpio_num_t g = (gpio_num_t)p;
    gpio_set_pull_mode(g, GPIO_PULLUP_ONLY);
    delayMicroseconds(200);
    Sample up = sample(g, 10);
    gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY);
    delayMicroseconds(200);
    Sample down = sample(g, 10);
    gpio_set_pull_mode(g, invert ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY);   // as the UART driver keeps it
    if (up.edges + down.edges >= DATA_EDGES) return 'd';
    if (up.high == down.high) return up.high ? 'h' : 'l';
    return up.high ? 'f' : '?';
  }

  // Slices of 10 ms (other tasks run in between): the level changes, and whether the pin was mostly high.
  static Sample sample(gpio_num_t p, uint8_t slices) {
    uint32_t edges = 0, highs = 0, n = 0;
    int last = gpio_get_level(p);
    for (uint8_t k = 0; k < slices; k++) {
      uint32_t t0 = micros();
      while (micros() - t0 < 10000) {
        int v = gpio_get_level(p);
        if (v != last) {
          edges++;
          last = v;
        }
        highs += v;
        n++;
        delayMicroseconds(10);
      }
      vTaskDelay(1);
    }
    return {edges, highs * 2 > n};
  }

  void take(const uint8_t* f, uint8_t n) {
    if (n < 10 || (f[0] != 0x40 && f[0] != 0x50)) return;
    uint8_t sum = 0;
    for (uint8_t i = 0; i < 9; i++) sum += f[i];
    if ((uint8_t)(sum - 0x10) != f[9]) {
      bad++;
      return;
    }
    good++;
    lastFrameMs = millis();
    if (f[0] == 0x40) {
      portENTER_CRITICAL(&mux);
      memcpy(temps, f + 1, 3);
      portEXIT_CRITICAL(&mux);
    } else if (f[7] != current.mode || f[6] != current.outputs || f[3] != current.flags) {
      changed(f[7], f[6], f[3]);
    }
  }

  void changed(uint8_t mode, uint8_t outputs, uint8_t flags) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    Event e = {(uint32_t)tv.tv_sec, (uint8_t)(tv.tv_usec / 100000), mode, outputs, flags};
    portENTER_CRITICAL(&mux);
    current = e;
    portEXIT_CRITICAL(&mux);
    if (events && xQueueSend(events, &e, 0) != pdTRUE) {   // the loop fell behind: the oldest change goes
      Event old;
      xQueueReceive(events, &old, 0);
      xQueueSend(events, &e, 0);
    }
  }
};
