// Furnace temperature monitor
// ESP32 + DS18B20 probes on one 1-Wire bus, exposed straight to Apple Home through HomeSpan (no bridge, no Pi).
//
// Every probe shows up in the Home app twice:
//   "<name>"        - temperature sensor; gets a fault flag when the probe stops answering
//   "<name> alarm"  - contact sensor that OPENS when the temperature leaves its limits or the probe dies;
//                     turn on notifications for it in the Home app to get a push on your phone
// Pairing: Home app -> Add Accessory -> More options -> "Furnace Sensors", setup code 466-37-726.
// Eve app: every temperature sensor carries an Eve history (EveHistory.h).
//
// Web app http://furnace.local:8080 (home WiFi only): live cards, charts from 24 h to a year, alarm limits,
// device status. Source in web/index.html, packed into WebApp.h by web/build.ps1.
// History on LittleFS: Eve entries (EveHistory.h) plus hourly and daily min / avg / max per probe (HistoryLog.h).
//
// WiFi: no credentials in the code. Without stored WiFi the ESP opens the open hotspot AP_SSID; join it with
// the phone, pick the network and type its password - it is kept in the ESP's flash.
// To give it a new network:
//   - hold the BOOT button ~5 s and release it: the stored WiFi is forgotten and the hotspot opens,
//   - hold it ~15 s and release: FULL reset before moving to another home - WiFi, HomeKit pairing and all
//     history are erased (alarm limits stay),
//   - or just wait: after WIFI_FALLBACK_MS without WiFi the hotspot opens by itself (old network kept),
//   - or Serial "X" (forget WiFi) / "A" (open the hotspot).
// The blue LED (if the board has one on GPIO2) shows the HomeSpan status.
//
// Updates:
//   - over the internet (any home): bump FW_VERSION, run release/publish.ps1; the ESP checks UPDATE_MANIFEST_URL
//     every hour (or "@u" / the web app), installs the new version and rolls back if it does not come online,
//   - on the same WiFi: Arduino IDE -> Tools -> Port -> "furnace at ..." (password set with the "O" command).
//
// Arduino IDE: board "ESP32 Dev Module"; the partition table comes from partitions.csv in this folder
// (2 x 1.69 MB app slots for OTA, 512 KB LittleFS for the history).
// Libraries:   HomeSpan, OneWire 2.3.8+, DallasTemperature.
// Serial Monitor (115200 baud): "@a" lists the probes on the bus, "?" shows the HomeSpan commands.

#include <HomeSpan.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SpanRollback.h>                                   // a new firmware has to prove itself (RemoteUpdate.h)
#include "EveHistory.h"
#include "HistoryLog.h"
#include "RemoteUpdate.h"
#include "WebApp.h"

// ------------------------------------------------------------------ configuration

constexpr uint32_t FW_VERSION = 2;                        // bump for every release (release/publish.ps1)
const char* UPDATE_MANIFEST_URL =
  "https://raw.githubusercontent.com/klimabartlomiej/furnace-monitor/main/release/manifest.json";

const char* DEVICE_NAME  = "furnace";                       // host name: furnace.local
const char* AP_SSID      = "Furnace-Setup";                 // WiFi setup hotspot
const char* AP_PASSWORD  = "";                              // empty = open hotspot
const char* TIME_ZONE    = "CET-1CEST,M3.5.0,M10.5.0/3";    // Poland; decides where a day starts in the daily history

constexpr uint16_t AP_TIMEOUT_S       = 15 * 60;         // hotspot gives up (and restarts) after this long
constexpr uint32_t WIFI_FALLBACK_MS   = 5 * 60 * 1000;   // no WiFi for this long -> open the hotspot
constexpr uint32_t WIFI_RESET_HOLD_MS = 5000;            // hold BOOT this long to forget the WiFi
constexpr uint32_t FULL_RESET_HOLD_MS = 15000;           // ... this long to also forget HomeKit and the history
constexpr uint32_t HISTORY_INTERVAL_S = 600;             // one Eve entry per 10 min, like Eve's own sensors
constexpr uint16_t HOURLY_CAPACITY    = 120 * 24;        // hourly records per probe (120 days)
constexpr uint16_t DAILY_CAPACITY     = 5 * 366;         // daily records per probe (5 years)
constexpr uint8_t  BOOT_BUTTON_PIN    = 0;               // the BOOT button on the board
constexpr uint8_t  STATUS_LED_PIN     = 2;               // blue on-board LED on most ESP32 dev boards
constexpr uint16_t WEB_PORT           = 8080;            // port 80 belongs to HomeKit (HomeSpan)
constexpr uint8_t  ONE_WIRE_PIN       = 22;              // D22 - GPIO of the DQ (data) wire, 4.7k pull-up to 3.3V
constexpr uint32_t READ_INTERVAL_MS   = 60 * 1000;       // one reading per minute
constexpr uint32_t CONVERSION_MS      = 800;             // 12-bit conversion takes 750 ms
constexpr float    ALARM_HYSTERESIS   = 2.0;             // degrees back inside the limit before an alarm clears
constexpr float    MAX_JUMP           = 10.0;            // a bigger change between readings needs confirmation
constexpr float    CONFIRM_TOLERANCE  = 2.0;             // the next reading must be this close to confirm a jump
constexpr uint8_t  FAULT_AFTER        = 3;               // bad readings in a row before a probe is reported dead

struct SensorConfig {
  const char*   name;          // name in the Home app, in the web app and in the log
  DeviceAddress address;       // {} = take the next unassigned probe found on the bus
  float         defaultLow;    // alarm limits until they are changed in the web app; NAN = off
  float         defaultHigh;
};

// Pinned addresses: a dead or removed probe never shifts the others.
// Identified on the bench 2026-10-22 (hand-warming test). The web app relies on this order.
SensorConfig SENSORS[] = {
  { "Furnace",      {0x28, 0x4D, 0x83, 0x77, 0x91, 0x0C, 0x02, 0xF8}, 40.0, 90.0 },   // counterfeit chip (11 ms conversion), failed at ~60 C in place
  { "Return water", {0x28, 0xAA, 0xEF, 0x8D, 0x4A, 0x14, 0x01, 0xAE}, NAN,  NAN  },
  { "Coal hopper",  {0x28, 0xAA, 0x61, 0x94, 0x4A, 0x14, 0x01, 0x6C}, NAN,  50.0 },   // high = embers creeping back into the hopper
  { "Boiler water", {0x28, 0xAA, 0xDB, 0x95, 0x4D, 0x14, 0x01, 0x6B}, 35.0, NAN  },
};

// ------------------------------------------------------------------ state

constexpr size_t SENSOR_COUNT = sizeof(SENSORS) / sizeof(SENSORS[0]);

struct SensorState {
  SpanCharacteristic* temperature  = nullptr;
  SpanCharacteristic* fault        = nullptr;
  SpanCharacteristic* alarmContact = nullptr;
  EveHistory*         history      = nullptr;
  RingLog*            hourly       = nullptr;
  RingLog*            daily        = nullptr;
  Aggregate           hour;               // readings of the current hour
  char     alarmName[32]   = "";
  char     serial[16]      = "";          // Eve keeps histories apart by serial number
  char     historyName[40] = "";
  char     historyPath[16] = "";
  char     hourlyPath[16]  = "";
  char     dailyPath[16]   = "";
  float    historySum      = 0;           // readings since the last Eve entry
  uint16_t historyCount    = 0;
  float    low         = NAN;             // active alarm limits (flash / web app)
  float    high        = NAN;
  float    lastGood    = NAN;
  float    unconfirmed = NAN;             // big jump waiting for the next reading to agree
  uint8_t  badReadings = 0;
  bool     faulty      = false;
  int8_t   alarm       = 0;               // -1 too cold, 0 ok, +1 too hot
};

OneWire           oneWire(ONE_WIRE_PIN);
DallasTemperature bus(&oneWire);
WebServer         server(WEB_PORT);
Preferences       prefs;
RemoteUpdate      updater(UPDATE_MANIFEST_URL, FW_VERSION);
SensorState       state[SENSOR_COUNT];
char              firmwareRevision[12];

uint32_t lastRequestMs     = 0;
uint32_t lastWifiOkMs      = 0;
uint32_t bootPressedMs     = 0;
uint8_t  resetLevel        = 0;   // 1 = forget WiFi, 2 = full reset; acted on when BOOT is released
uint32_t lastHistorySlot   = 0;
uint32_t currentHour       = 0;   // Unix time / 3600 of the hour being aggregated
uint32_t currentDay        = 0;   // local midnight (Unix time) of the day being aggregated
bool     conversionPending = false;
bool     webStarted        = false;

// ------------------------------------------------------------------ probes on the bus

bool isUnset(const DeviceAddress a) {
  for (uint8_t i = 0; i < 8; i++)
    if (a[i]) return false;
  return true;
}

String formatAddress(const DeviceAddress a) {
  char buf[64];
  snprintf(buf, sizeof(buf), "{0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}",
           a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  return buf;
}

const char* slotName(const DeviceAddress a) {
  for (auto& s : SENSORS)
    if (memcmp(s.address, a, 8) == 0) return s.name;
  return nullptr;
}

// Gives every slot without a pinned address the next probe on the bus that no slot has pinned.
void assignAddresses() {
  DeviceAddress found;
  oneWire.reset_search();
  while (oneWire.search(found)) {
    if (!bus.validAddress(found) || !bus.validFamily(found) || slotName(found)) continue;
    for (auto& s : SENSORS) {
      if (!isUnset(s.address)) continue;
      memcpy(s.address, found, 8);
      Serial.printf("'%s' auto-assigned %s - pin this address in SENSORS\n", s.name, formatAddress(found).c_str());
      break;
    }
  }
}

// Every DS18B20 on the bus with its address, temperature and assigned name. Blocks ~750 ms for a fresh conversion.
String busReport() {
  bus.setWaitForConversion(true);
  bus.requestTemperatures();
  bus.setWaitForConversion(false);

  String out;
  DeviceAddress a;
  uint8_t count = 0;
  oneWire.reset_search();
  while (oneWire.search(a)) {
    if (!bus.validAddress(a)) continue;
    const char* name = slotName(a);
    char line[128];
    snprintf(line, sizeof(line), "%u: %s  %7.2f C  %s\n",
             ++count, formatAddress(a).c_str(), bus.getTempC(a), name ? name : "(not used)");
    out += line;
  }
  if (count == 0) out = "No DS18B20 found - check the wiring and the pull-up resistor\n";
  return out;
}

// ------------------------------------------------------------------ HomeKit

void createAccessories() {
  new SpanAccessory();                       // the bridge itself
    new Service::AccessoryInformation();
      new Characteristic::Identify();

  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    SensorState& s = state[i];
    snprintf(s.serial, sizeof(s.serial), "FURNACE-%u", (unsigned)(i + 1));
    snprintf(s.historyName, sizeof(s.historyName), "%s History", SENSORS[i].name);
    new SpanAccessory();
      new Service::AccessoryInformation();
        new Characteristic::Identify();
        new Characteristic::Name(SENSORS[i].name);
        new Characteristic::Manufacturer("HomeSpan");
        new Characteristic::Model("DS18B20 probe");
        new Characteristic::SerialNumber(s.serial);
        new Characteristic::FirmwareRevision(firmwareRevision);
      new Service::TemperatureSensor();
        new Characteristic::Name(SENSORS[i].name);
        s.temperature = new Characteristic::CurrentTemperature(0);
        s.temperature->setRange(-55, 125);
        s.fault = new Characteristic::StatusFault(0);
        new Characteristic::TemperatureDisplayUnits(0);   // Celsius - Eve shows a weather history only with this set
      s.history = new EveHistory(s.historyName, s.historyPath);
  }

  // One alarm contact per probe, so a dead probe that keeps its alarm open never hides a new alarm elsewhere.
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    snprintf(state[i].alarmName, sizeof(state[i].alarmName), "%s alarm", SENSORS[i].name);
    new SpanAccessory();
      new Service::AccessoryInformation();
        new Characteristic::Identify();
        new Characteristic::Name(state[i].alarmName);
      new Service::ContactSensor();
        state[i].alarmContact = new Characteristic::ContactSensorState(0);
  }
}

// Pushes the state of one probe to HomeKit; only changed values are sent.
// (MQTT can hook in here later, next to HomeKit.)
void reportSensor(size_t i) {
  SensorState& s = state[i];
  // A dead probe sends no temperature at all - Home keeps the last real value and shows the fault flag
  // (the old firmware sent -127, which HomeKit clamped to the -100 you saw).
  if (!s.faulty && !isnan(s.lastGood)) {
    float value = roundf(s.lastGood * 10) / 10;
    if (s.temperature->getVal<float>() != value) s.temperature->setVal(value);
  }
  int fault = s.faulty ? 1 : 0;
  if (s.fault->getVal() != fault) s.fault->setVal(fault);
  int open = (s.alarm != 0 || s.faulty) ? 1 : 0;   // 1 = contact open = alarm
  if (s.alarmContact->getVal() != open) {
    s.alarmContact->setVal(open);
    Serial.printf("%s: %s\n", s.alarmName, open ? "OPEN" : "closed");
  }
}

// ------------------------------------------------------------------ measurement

// Up to three scratchpad reads. The library already rejects CRC errors and all-zero frames
// (a shorted bus reads as zeros, which is where the old "0 on every sensor" came from).
float readSensor(size_t i) {
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    float t = bus.getTempC(SENSORS[i].address);
    if (t == DEVICE_DISCONNECTED_C || t < -55 || t > 125) continue;
    // 85.0 is the power-on value of a probe that reset mid-cycle - believe it only when we were already near it.
    if (t == 85.0f && !(fabsf(state[i].lastGood - 85.0f) < 10.0f)) continue;
    return t;
  }
  return NAN;
}

void updateAlarm(size_t i, float t) {
  SensorState& s = state[i];
  int8_t before = s.alarm;
  // NAN limits never compare true, so a disabled limit simply never fires.
  if (t >= s.high)                                        s.alarm = +1;
  else if (t <= s.low)                                    s.alarm = -1;
  else if (s.alarm > 0 && t <= s.high - ALARM_HYSTERESIS) s.alarm = 0;
  else if (s.alarm < 0 && t >= s.low + ALARM_HYSTERESIS)  s.alarm = 0;
  if (s.alarm != before)
    Serial.printf("%s: alarm %s\n", SENSORS[i].name, s.alarm > 0 ? "TOO HOT" : s.alarm < 0 ? "TOO COLD" : "cleared");
}

void registerReading(size_t i, float t) {
  SensorState& s = state[i];

  // A sudden jump is accepted only once the next reading agrees with it - filters one-off glitches.
  if (!isnan(s.lastGood) && fabsf(t - s.lastGood) > MAX_JUMP && !(fabsf(t - s.unconfirmed) <= CONFIRM_TOLERANCE)) {
    s.unconfirmed = t;
    Serial.printf("%s: %.2f C looks like a glitch, waiting for confirmation\n", SENSORS[i].name, t);
    return;
  }

  s.unconfirmed = NAN;
  s.lastGood = t;
  s.badReadings = 0;
  s.faulty = false;
  s.historySum += t;
  s.historyCount++;
  s.hour.add(t);
  updateAlarm(i, t);
  Serial.printf("%s: %.2f C\n", SENSORS[i].name, t);
}

void registerBadReading(size_t i) {
  SensorState& s = state[i];
  Serial.printf("%s: no valid reading\n", SENSORS[i].name);
  if (s.badReadings < 255) s.badReadings++;
  if (s.badReadings >= FAULT_AFTER) s.faulty = true;
}

void readAll() {
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    float t = readSensor(i);
    if (isnan(t)) registerBadReading(i);
    else registerReading(i, t);
    reportSensor(i);
  }
}

// ------------------------------------------------------------------ history

uint32_t localMidnight(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
  return mktime(&lt);
}

// Feeds the histories once NTP has set the clock (a dead probe simply leaves a gap):
//   Eve    - every HISTORY_INTERVAL_S: average of the readings since the previous entry
//   hourly - at every full hour: min / avg / max of that hour
//   daily  - at local midnight: min / avg / max of the day, built from its hourly records so a reboot
//            during the day loses nothing but the unfinished hour
void logHistory() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;

  uint32_t slot = now / HISTORY_INTERVAL_S;
  if (slot != lastHistorySlot) {
    lastHistorySlot = slot;
    for (auto& s : state) {
      if (!s.historyCount) continue;
      s.history->add(now, s.historySum / s.historyCount);
      s.historySum = 0;
      s.historyCount = 0;
    }
  }

  uint32_t hour = now / 3600;
  if (hour != currentHour) {
    if (currentHour)
      for (auto& s : state) {
        if (s.hour.count) s.hourly->add(s.hour.record(currentHour * 3600));
        s.hour.reset();
      }
    currentHour = hour;
  }

  uint32_t day = localMidnight(now);
  if (day != currentDay) {
    if (currentDay)
      for (auto& s : state) {
        Aggregate a;
        s.hourly->forEach(currentDay, [&](const AggRecord& r) { if (r.time < day) a.add(r); });
        if (a.count) s.daily->add(a.record(currentDay));
      }
    currentDay = day;
  }
}

void setupHistory() {
  if (LittleFS.begin(true))
    Serial.printf("LittleFS: %u of %u KB used\n", (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  else
    Serial.println("LittleFS mount failed - no history");

  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    SensorState& s = state[i];
    snprintf(s.historyPath, sizeof(s.historyPath), "/eve%u.bin", (unsigned)i);
    snprintf(s.hourlyPath, sizeof(s.hourlyPath), "/hour%u.bin", (unsigned)i);
    snprintf(s.dailyPath, sizeof(s.dailyPath), "/day%u.bin", (unsigned)i);
    s.hourly = new RingLog(s.hourlyPath, HOURLY_CAPACITY);
    s.daily  = new RingLog(s.dailyPath, DAILY_CAPACITY);
    s.hourly->begin();
    s.daily->begin();
  }
}

// ------------------------------------------------------------------ alarm limits (flash)

String prefKey(const char* kind, size_t i) {
  return String(kind) + i;
}

void loadLimits() {
  prefs.begin("furnace", false);
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    String lo = prefKey("lo", i), hi = prefKey("hi", i);
    state[i].low  = prefs.isKey(lo.c_str()) ? prefs.getFloat(lo.c_str()) : SENSORS[i].defaultLow;
    state[i].high = prefs.isKey(hi.c_str()) ? prefs.getFloat(hi.c_str()) : SENSORS[i].defaultHigh;
  }
}

float parseLimit(String v) {
  v.trim();
  v.replace(',', '.');
  if (v.length() == 0) return NAN;
  float f = v.toFloat();
  return (f < -55 || f > 125) ? NAN : f;
}

// ------------------------------------------------------------------ web app + API

String jsonNumber(float v, unsigned decimals = 2) {
  return isnan(v) ? String("null") : String(v, decimals);
}

void handleApp() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", (const char*)WEBAPP_HTML_GZ, WEBAPP_HTML_GZ_LEN);
}

void handleIcon() {
  server.sendHeader("Cache-Control", "max-age=604800");
  server.send_P(200, "image/png", (const char*)WEBAPP_ICON_PNG, WEBAPP_ICON_PNG_LEN);
}

void handleManifest() {
  server.send_P(200, "application/manifest+json", WEBAPP_MANIFEST);
}

// Current readings, limits, alarms and device health.
void handleNow() {
  String j;
  j.reserve(1024);
  j += "{\"time\":";     j += (uint32_t)time(nullptr);
  j += ",\"uptime\":";   j += (uint32_t)(millis() / 1000);
  j += ",\"rssi\":";     j += (int)WiFi.RSSI();
  j += ",\"ip\":\"";     j += WiFi.localIP().toString();
  j += "\",\"heap\":";   j += (uint32_t)ESP.getFreeHeap();
  j += ",\"fsUsed\":";   j += (uint32_t)LittleFS.usedBytes();
  j += ",\"fsTotal\":";  j += (uint32_t)LittleFS.totalBytes();
  j += ",\"interval\":"; j += HISTORY_INTERVAL_S;
  j += ",\"fw\":";       j += FW_VERSION;
  j += ",\"update\":\""; j += updater.status();
  j += "\",\"sensors\":[";
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    const SensorState& s = state[i];
    if (i) j += ',';
    j += "{\"name\":\"";  j += SENSORS[i].name;
    j += "\",\"temp\":";  j += s.faulty ? String("null") : jsonNumber(s.lastGood);
    j += ",\"low\":";     j += jsonNumber(s.low, 1);
    j += ",\"high\":";    j += jsonNumber(s.high, 1);
    j += ",\"alarm\":";   j += (int)s.alarm;
    j += ",\"fault\":";   j += s.faulty ? "true" : "false";
    j += '}';
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// Binary history for the charts, little-endian records, oldest first:
//   r=f (Eve entries)       : uint32 time, int16 temp x100
//   r=h (hourly) / r=d (daily): uint32 time, int16 min, avg, max x100
void handleHistory() {
  size_t i = server.arg("s").toInt();
  if (i >= SENSOR_COUNT) {
    server.send(400, "text/plain", "bad sensor");
    return;
  }
  uint32_t since = strtoul(server.arg("since").c_str(), nullptr, 10);
  String range = server.arg("r");

  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/octet-stream", "");

  uint8_t buf[600];
  size_t n = 0;
  auto put = [&](const void* p, size_t len) {
    if (n + len > sizeof(buf)) {
      server.sendContent((const char*)buf, n);
      n = 0;
    }
    memcpy(buf + n, p, len);
    n += len;
  };
  if (range == "f") {
    state[i].history->forEach(since, [&](uint32_t t, int16_t v) { put(&t, 4); put(&v, 2); });
  } else {
    RingLog* log = range == "d" ? state[i].daily : state[i].hourly;
    log->forEach(since, [&](const AggRecord& r) { put(&r, sizeof(r)); });
  }
  if (n) server.sendContent((const char*)buf, n);
  server.sendContent("");
}

// POST s, low, high (empty = off). Answers with the same JSON as /api/now.
void handleLimits() {
  size_t i = server.arg("s").toInt();
  if (i >= SENSOR_COUNT) {
    server.send(400, "text/plain", "bad sensor");
    return;
  }
  SensorState& s = state[i];
  s.low  = parseLimit(server.arg("low"));
  s.high = parseLimit(server.arg("high"));
  prefs.putFloat(prefKey("lo", i).c_str(), s.low);
  prefs.putFloat(prefKey("hi", i).c_str(), s.high);
  // New limits - judge the current value from scratch, without the old hysteresis.
  s.alarm = 0;
  if (!isnan(s.lastGood)) updateAlarm(i, s.lastGood);
  reportSensor(i);
  handleNow();
}

void handleBus() {
  server.send(200, "text/plain", busReport());
}

// POST: look for a new firmware at the next loop() instead of waiting for the hourly check.
void handleUpdate() {
  updater.requestCheck();
  handleNow();
}

// Called by HomeSpan every time WiFi comes up; the web server is started once and survives reconnects.
void onNetworkUp(int count) {
  Serial.printf("Network up (#%d), IP %s - web app http://%s.local:%u\n",
                count, WiFi.localIP().toString().c_str(), DEVICE_NAME, WEB_PORT);
  configTzTime(TIME_ZONE, "pool.ntp.org", "time.google.com");   // the histories need real timestamps
  updater.networkUp();
  if (webStarted) return;
  server.on("/", HTTP_GET, handleApp);
  server.on("/icon.png", HTTP_GET, handleIcon);
  server.on("/manifest.webmanifest", HTTP_GET, handleManifest);
  server.on("/api/now", HTTP_GET, handleNow);
  server.on("/api/hist", HTTP_GET, handleHistory);
  server.on("/api/limits", HTTP_POST, handleLimits);
  server.on("/api/bus", HTTP_GET, handleBus);
  server.on("/api/update", HTTP_POST, handleUpdate);
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
  server.begin();
  webStarted = true;
}

// ------------------------------------------------------------------ WiFi recovery

// Hold BOOT and release: after WIFI_RESET_HOLD_MS the WiFi is forgotten, after FULL_RESET_HOLD_MS also the
// HomeKit pairing and all history; either way the ESP reboots into the setup hotspot.
// Acts on release - rebooting while BOOT is still held would drop the chip into its flashing mode.
void checkBootButton() {
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (!bootPressedMs) bootPressedMs = millis();
    uint32_t held = millis() - bootPressedMs;
    if (resetLevel < 2 && held >= FULL_RESET_HOLD_MS) {
      resetLevel = 2;
      Serial.println("BOOT held 15 s - release for a FULL reset (WiFi, HomeKit pairing, history)");
    } else if (resetLevel < 1 && held >= WIFI_RESET_HOLD_MS) {
      resetLevel = 1;
      Serial.println("BOOT held 5 s - release to forget the WiFi, keep holding for a full reset");
    }
    return;
  }
  bootPressedMs = 0;
  if (resetLevel == 1) homeSpan.processSerialCommand("X");   // erases the WiFi and reboots
  if (resetLevel == 2) {
    LittleFS.format();                                       // all history (Eve, hourly, daily)
    homeSpan.processSerialCommand("F");                      // erases WiFi + pairing and reboots
  }
}

// The stored network is gone (new router, new password): open the hotspot instead of retrying forever.
// If nobody uses it, HomeSpan reboots after AP_TIMEOUT_S and tries the old network again.
void checkWifiFallback() {
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOkMs = millis();
  } else if (millis() - lastWifiOkMs >= WIFI_FALLBACK_MS) {
    Serial.println("No WiFi for too long - opening the setup hotspot");
    lastWifiOkMs = millis();
    homeSpan.processSerialCommand("A");   // blocks until the hotspot is used or times out
  }
}

// ------------------------------------------------------------------ setup / loop

void setup() {
  Serial.begin(115200);
  snprintf(firmwareRevision, sizeof(firmwareRevision), "%u", (unsigned)FW_VERSION);
  updater.begin();

  bus.begin();
  bus.setWaitForConversion(false);
  assignAddresses();
  Serial.print(busReport());
  loadLimits();
  setupHistory();

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  homeSpan.setStatusPin(STATUS_LED_PIN);
  homeSpan.setGetCharacteristicsCallback(EveHistory::onGetCharacteristics);
  homeSpan.setApSSID(AP_SSID);
  homeSpan.setApPassword(AP_PASSWORD);
  homeSpan.setApTimeout(AP_TIMEOUT_S);
  homeSpan.enableAutoStartAP();              // no stored WiFi -> open the setup hotspot
  homeSpan.setHostNameSuffix("");            // host name is exactly DEVICE_NAME
  homeSpan.enableOTA();                      // password lives in flash (CLI "O"), not in this public code
  homeSpan.setConnectionCallback(onNetworkUp);
  homeSpan.begin(Category::Bridges, "Furnace Sensors", DEVICE_NAME);
  createAccessories();

  new SpanUserCommand('a', "- list DS18B20 probes on the bus", [](const char*) { Serial.print(busReport()); });
  new SpanUserCommand('u', "- check for a firmware update now", [](const char*) { updater.requestCheck(); });

  lastWifiOkMs  = millis();
  lastRequestMs = millis() - READ_INTERVAL_MS;   // first reading right away
}

void loop() {
  homeSpan.poll();
  if (webStarted) server.handleClient();
  checkBootButton();
  checkWifiFallback();
  logHistory();
  updater.loop();

  uint32_t now = millis();
  if (!conversionPending && now - lastRequestMs >= READ_INTERVAL_MS) {
    bus.requestTemperatures();   // returns at once, the probes convert on their own
    lastRequestMs = now;
    conversionPending = true;
  } else if (conversionPending && now - lastRequestMs >= CONVERSION_MS) {
    conversionPending = false;
    readAll();
  }
}
