// The Geco GH11TB controller's room-panel bus (terminals 21 A / 22 B), READ ONLY - docs/geco-gh11tb-bus.md.
// The controller announces its state for a GA01HA panel every ~57 ms, also when no panel is attached:
//   9600 8N1, every byte bit-inverted, every piece ends with 0x23 (inverted). Used frames (inverted):
//     40 T1 T2 T3 .. .. .. .. .. CK      temperatures x 2 (T1 boiler, T2 feeder, T3 hot water), 0xfa = no sensor
//     50 .. .. P3 .. .. OUT MODE .. CK   P3 0x40 piston away from home; OUT 0x40 fan, 0x20 feeder, 0x02 CO pump;
//                                        MODE 0x04 standby, 0x08 burning, 0x10 podtrzymanie
//   CK = sum(bytes 0..8) - 0x10.
// The UART has no TX pin and the RS485 module's transmit input is tied high: nothing can reach the controller.
// A task of its own reads the line, so a busy loop() loses no frame; every change of the state is queued as an
// Event (to a tenth of a second) for the work log, and so is the bus going quiet (mode 0) and coming back.

#pragma once

#include <Arduino.h>
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

  void begin(int8_t rxPin) {
    if (rxPin < 0) return;
    events = xQueueCreate(256, sizeof(Event));
    Serial1.setRxBufferSize(2048);
    Serial1.begin(9600, SERIAL_8N1, rxPin, -1);
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

  Event state() const {
    portENTER_CRITICAL(&mux);
    Event e = current;
    portEXIT_CRITICAL(&mux);
    return e;
  }

  // The next queued change, false when none.
  bool nextEvent(Event& e) { return events && xQueueReceive(events, &e, 0) == pdTRUE; }

private:
  mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  QueueHandle_t events = nullptr;
  Event    current = {};
  uint8_t  temps[3] = {0xFA, 0xFA, 0xFA};
  volatile uint32_t lastFrameMs = 0;
  volatile uint32_t good = 0, bad = 0;
  uint8_t  piece[24];
  uint8_t  have = 0;

  static void task(void* self) { static_cast<GecoBus*>(self)->run(); }

  void run() {
    for (;;) {
      while (Serial1.available()) {
        uint8_t b = Serial1.read() ^ 0xFF;
        if (have < sizeof(piece)) piece[have++] = b;
        if (b == 0x23) {                                   // a piece ends
          take(piece, have);
          have = 0;
        }
      }
      if (current.mode != NO_DATA && !fresh()) changed(NO_DATA, 0, 0);   // the controller went quiet
      vTaskDelay(pdMS_TO_TICKS(20));
    }
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
