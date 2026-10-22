// Furnace temperature monitor
// ESP32 + DS18B20 probes on one 1-Wire bus, exposed straight to Apple Home through HomeSpan (no bridge, no Pi).
//
// Apple Home (all names in Polish - the household's language):
//   name       - temperature sensor per probe; gets a fault flag when the probe stops answering
//   alarmName  - contact sensor per probe that OPENS when the temperature leaves its limits or the probe dies
//   warnings   - three contact sensors (furnace dying out, freezing, embers creeping into the coal hopper)
//   Turn on notifications for the contact sensors in the Home app to get a push on the phone.
// Pairing: Home app -> Add Accessory -> More options -> "Czujniki pieca", setup code 466-37-726.
// Eve app: every temperature sensor carries an Eve history (EveHistory.h).
//
// Web app http://furnace.local:8080 (home WiFi only): live cards, charts from 6 hours to years, warnings and
// limits, event journal. Source in web/index.html, packed into WebApp.h by web/build.ps1.
// History (HistoryLog.h): every minute for 31 days in its own flash partition; daily summaries for 5 years, the
// event journal and 14 days of Eve entries on LittleFS. All of them are rings - the newest overwrites the
// oldest, nothing ever fills up.
//
// WiFi: no credentials in the code. Without stored WiFi the ESP opens the open hotspot AP_SSID; join it with
// the phone, pick the network and type its password - it is kept in the ESP's flash.
// To give it a new network:
//   - hold the BOOT button ~5 s and release it: the stored WiFi is forgotten and the hotspot opens,
//   - hold it ~15 s and release: FULL reset before moving to another home - WiFi, HomeKit pairing and all
//     history are erased (limits and warning settings stay),
//   - or just wait: after WIFI_FALLBACK_MS without WiFi the hotspot opens by itself (old network kept),
//   - or Serial "X" (forget WiFi) / "A" (open the hotspot).
// The blue LED (if the board has one on GPIO2) shows the HomeSpan status.
//
// Updates:
//   - over the internet (any home): bump FW_VERSION, run release/publish.ps1; the ESP checks UPDATE_MANIFEST_URL
//     every hour (or "@u" / the web app), installs the new version and rolls back if it does not come online,
//   - on the same WiFi: web app -> Ustawienia -> Oprogramowanie -> install a .bin file, or Arduino IDE -> Tools ->
//     Port -> "furnace at ..." (both use the OTA password set with the serial command "O").
//
// Arduino IDE: board "ESP32 Dev Module"; the partition table comes from partitions.csv in this folder
// (2 x 1.69 MB app slots for OTA, 352 KB minute ring, 224 KB LittleFS). A partition change needs a USB upload.
// Libraries:   HomeSpan, OneWire 2.3.8+, DallasTemperature.
// Serial Monitor (115200 baud): "@a" lists the probes on the bus, "@u" checks for updates, "?" HomeSpan help.

#include <HomeSpan.h>
#include <SpanRollback.h>                                   // a new firmware has to prove itself (RemoteUpdate.h)
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <nvs.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "EveHistory.h"
#include "HistoryLog.h"
#include "RemoteUpdate.h"
#include "WebApp.h"

// ------------------------------------------------------------------ configuration

constexpr uint32_t FW_VERSION = 7;                      // bump for every release (release/publish.ps1)
const char* UPDATE_MANIFEST_URL =
  "https://raw.githubusercontent.com/klimabartlomiej/furnace-monitor/main/release/manifest.json";

const char* DEVICE_NAME  = "furnace";                       // host name: furnace.local
const char* AP_SSID      = "Furnace-Setup";                 // WiFi setup hotspot
const char* AP_PASSWORD  = "";                              // empty = open hotspot
const char* TIME_ZONE    = "CET-1CEST,M3.5.0,M10.5.0/3";    // Poland; decides where a day starts

constexpr uint16_t AP_TIMEOUT_S       = 15 * 60;         // hotspot gives up (and restarts) after this long
constexpr uint32_t WIFI_FALLBACK_MS   = 5 * 60 * 1000;   // no WiFi for this long -> open the hotspot
constexpr uint32_t WIFI_RESET_HOLD_MS = 5000;            // hold BOOT this long to forget the WiFi
constexpr uint32_t FULL_RESET_HOLD_MS = 15000;           // ... this long to also forget HomeKit and the history
constexpr uint32_t HISTORY_INTERVAL_S = 600;             // one Eve entry per 10 min, like Eve's own sensors
constexpr uint32_t DAILY_CAPACITY     = 5 * 366;         // daily summaries (5 years)
constexpr uint32_t EVENT_CAPACITY     = 1024;            // event journal entries
constexpr uint8_t  FS_LAYOUT          = 3;               // bump when the history files change shape (wipes them once)
constexpr uint8_t  TREND_MINUTES      = 30;              // window of the trend the warnings look at
constexpr uint8_t  BOOT_BUTTON_PIN    = 0;               // the BOOT button on the board
constexpr uint8_t  STATUS_LED_PIN     = 2;               // blue on-board LED on most ESP32 dev boards
constexpr uint16_t WEB_PORT           = 8080;            // port 80 belongs to HomeKit (HomeSpan)
constexpr uint8_t  ONE_WIRE_PIN       = 22;              // D22 - GPIO of the DQ (data) wire, 4.7k pull-up to 3.3V
constexpr uint32_t READ_INTERVAL_MS   = 60 * 1000;       // one reading per minute (on the full minute once NTP is in)
constexpr uint32_t CONVERSION_MS      = 800;             // 12-bit conversion takes 750 ms
constexpr float    ALARM_HYSTERESIS   = 2.0;             // degrees back inside the limit before an alarm clears
constexpr float    MAX_JUMP           = 10.0;            // a bigger change between readings needs confirmation
constexpr float    CONFIRM_TOLERANCE  = 2.0;             // the next reading must be this close to confirm a jump
constexpr uint8_t  FAULT_AFTER        = 3;               // bad readings in a row before a probe is reported dead

// Names are what the Home app and the web app show; HomeKit accepts only letters, digits, spaces and apostrophes.
struct SensorConfig {
  const char*   name;          // temperature sensor
  const char*   alarmName;     // the probe's alarm contact sensor
  DeviceAddress address;       // {} = take the next unassigned probe found on the bus
  bool          water;         // watched by the freeze warning
  float         defaultLow;    // alarm limits until they are changed in the web app; NAN = off
  float         defaultHigh;
};

// Pinned addresses: a dead or removed probe never shifts the others.
// Identified on the bench 2026-10-22 (hand-warming test). The web app relies on this order.
SensorConfig SENSORS[] = {
  { "Piec",           "Alarm pieca",     {0x28, 0x4D, 0x83, 0x77, 0x91, 0x0C, 0x02, 0xF8}, true,  40.0, 90.0 },   // furnace; counterfeit chip, failed at ~60 C in place
  { "Powrót",         "Alarm powrotu",   {0x28, 0xAA, 0xEF, 0x8D, 0x4A, 0x14, 0x01, 0xAE}, true,  NAN,  NAN  },   // return water
  { "Zasobnik węgla", "Alarm zasobnika", {0x28, 0xAA, 0x61, 0x94, 0x4A, 0x14, 0x01, 0x6C}, false, NAN,  50.0 },   // coal hopper; high = embers creeping back
  { "Bojler",         "Alarm bojlera",   {0x28, 0xAA, 0xDB, 0x95, 0x4D, 0x14, 0x01, 0x6B}, true,  35.0, NAN  },   // hot water tank
};
constexpr size_t SENSOR_COUNT = sizeof(SENSORS) / sizeof(SENSORS[0]);
constexpr size_t FURNACE      = 0;   // probes the warnings look at
constexpr size_t COAL_HOPPER  = 2;

// Warnings - each one is a contact sensor in Apple Home and a banner in the web app; tuned in the web app.
enum WarningId : uint8_t { WARN_GOING_OUT, WARN_FREEZE, WARN_BURNBACK, WARN_COUNT };
const char* WARNING_NAMES[WARN_COUNT] = { "Piec wygasa", "Ryzyko zamarznięcia", "Cofanie żaru" };

struct Settings {
  bool     goOutOn      = true;   // furnace dying out: falling faster than goOutRate (degrees/hour), or the
  float    goOutRate    = 8;      // trend reaches its min limit within goOutHorizon minutes
  uint16_t goOutHorizon = 45;
  bool     freezeOn     = true;   // any water probe at or below freezeTemp
  float    freezeTemp   = 5;
  bool     burnbackOn   = true;   // coal hopper warming faster than burnbackRate (degrees/hour)
  float    burnbackRate = 10;
  float    hotWater     = 45;     // boiler temperature the web app calls "hot water ready"
  float    lat          = NAN;    // location of the house; the web app fetches the outdoor temperature for it
  float    lon          = NAN;
  char     place[48]    = "";
};

// ------------------------------------------------------------------ state

struct SensorState {
  SpanCharacteristic* temperature  = nullptr;
  SpanCharacteristic* fault        = nullptr;
  SpanCharacteristic* alarmContact = nullptr;
  EveHistory*         history      = nullptr;
  char     serial[16]      = "";          // Eve keeps histories apart by serial number
  char     historyName[40] = "";
  char     historyPath[16] = "";
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

struct WarningState {
  SpanCharacteristic* contact = nullptr;
  bool    active  = false;
  uint8_t pending = 0;    // minutes the condition has held
  uint8_t calm    = 0;    // minutes it has been gone
};

enum EventType : uint8_t {
  EV_BOOT = 1, EV_UPDATED, EV_ALARM_HIGH, EV_ALARM_LOW, EV_ALARM_CLEAR, EV_FAULT, EV_FAULT_CLEAR,
  EV_WARN_ON, EV_WARN_OFF, EV_HISTORY_CLEARED,
};

struct __attribute__((packed)) EventRecord {
  uint32_t time;
  uint8_t  type;        // EventType
  uint8_t  sensor;      // probe index, or WarningId for EV_WARN_*
  int16_t  value;       // degrees x 100, or the firmware version
};

struct __attribute__((packed)) DayRecord {
  uint32_t time;                      // local midnight
  int16_t  min[SENSOR_COUNT];         // degrees x 100, NO_VALUE when the probe had no reading that day
  int16_t  avg[SENSOR_COUNT];
  int16_t  max[SENSOR_COUNT];
  uint16_t burnMinutes;               // minutes the furnace was at or above burnThreshold()
};

using MinuteRecord = MinuteLog<SENSOR_COUNT>::Record;

OneWire                 oneWire(ONE_WIRE_PIN);
DallasTemperature       bus(&oneWire);
WebServer               server(WEB_PORT);
Preferences             prefs;
RemoteUpdate            updater(UPDATE_MANIFEST_URL, FW_VERSION);
MinuteLog<SENSOR_COUNT> minutes("minutes");                 // partition label in partitions.csv (31 days)
RingFile<DayRecord>     daily("/daily.bin", DAILY_CAPACITY);
RingFile<EventRecord>   events("/events.bin", EVENT_CAPACITY);
Settings                settings;
SensorState             state[SENSOR_COUNT];
WarningState            warnings[WARN_COUNT];
char                    firmwareRevision[12];

float    recent[TREND_MINUTES][SENSOR_COUNT];   // last minutes for the trends, NAN = no reading
uint8_t  recentNext        = 0;
uint8_t  recentCount       = 0;
int16_t  goOutForecast     = -1;                // minutes until the furnace reaches its min limit, -1 = not heading there
EventRecord pendingEvents[8];                   // events from before NTP set the clock
uint8_t  pendingEventCount = 0;

uint32_t lastRequestMs     = 0;
uint32_t lastRequestMinute = 0;
uint32_t lastWifiOkMs      = 0;
uint32_t bootPressedMs     = 0;
uint8_t  resetLevel        = 0;   // 1 = forget WiFi, 2 = full reset; acted on when BOOT is released
uint32_t lastHistorySlot   = 0;
uint32_t currentDay        = 0;   // local midnight (Unix time) of the day being summarized
bool     conversionPending = false;
bool     webStarted        = false;

bool clockSet(time_t t) { return t >= 1700000000; }

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

// ------------------------------------------------------------------ event journal

void logEvent(EventType type, uint8_t sensor, int16_t value) {
  time_t now = time(nullptr);
  EventRecord e = {(uint32_t)now, (uint8_t)type, sensor, value};
  if (!clockSet(now)) {
    if (pendingEventCount < 8) pendingEvents[pendingEventCount++] = e;
    return;
  }
  events.add(e);
}

void flushPendingEvents(time_t now) {
  for (uint8_t i = 0; i < pendingEventCount; i++) {
    pendingEvents[i].time = now;
    events.add(pendingEvents[i]);
  }
  pendingEventCount = 0;
}

// ------------------------------------------------------------------ HomeKit

SpanCharacteristic* newContactAccessory(const char* name) {
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(name);
    new Service::ContactSensor();
      return new Characteristic::ContactSensorState(0);
}

void createAccessories() {
  new SpanAccessory();                       // the bridge itself
    new Service::AccessoryInformation();
      new Characteristic::Identify();

  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    SensorState& s = state[i];
    snprintf(s.serial, sizeof(s.serial), "FURNACE-%u", (unsigned)(i + 1));
    snprintf(s.historyName, sizeof(s.historyName), "%s historia", SENSORS[i].name);
    snprintf(s.historyPath, sizeof(s.historyPath), "/eve%u.bin", (unsigned)i);
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
  for (size_t i = 0; i < SENSOR_COUNT; i++) state[i].alarmContact = newContactAccessory(SENSORS[i].alarmName);
  for (uint8_t w = 0; w < WARN_COUNT; w++) warnings[w].contact = newContactAccessory(WARNING_NAMES[w]);
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
    Serial.printf("%s: %s\n", SENSORS[i].alarmName, open ? "OPEN" : "closed");
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

// Every alarm change goes to the journal - the web app pairs starts with ends from it.
void logAlarmChange(size_t i, int8_t before, float t) {
  int8_t now = state[i].alarm;
  if (now == before) return;
  Serial.printf("%s: alarm %s\n", SENSORS[i].name, now > 0 ? "TOO HOT" : now < 0 ? "TOO COLD" : "cleared");
  logEvent(now > 0 ? EV_ALARM_HIGH : now < 0 ? EV_ALARM_LOW : EV_ALARM_CLEAR, i, toHundredths(t));
}

void updateAlarm(size_t i, float t) {
  SensorState& s = state[i];
  int8_t before = s.alarm;
  // NAN limits never compare true, so a disabled limit simply never fires.
  if (t >= s.high)                                        s.alarm = +1;
  else if (t <= s.low)                                    s.alarm = -1;
  else if (s.alarm > 0 && t <= s.high - ALARM_HYSTERESIS) s.alarm = 0;
  else if (s.alarm < 0 && t >= s.low + ALARM_HYSTERESIS)  s.alarm = 0;
  logAlarmChange(i, before, t);
}

// Returns true when the reading was accepted.
bool registerReading(size_t i, float t) {
  SensorState& s = state[i];

  // A sudden jump is accepted only once the next reading agrees with it - filters one-off glitches.
  if (!isnan(s.lastGood) && fabsf(t - s.lastGood) > MAX_JUMP && !(fabsf(t - s.unconfirmed) <= CONFIRM_TOLERANCE)) {
    s.unconfirmed = t;
    Serial.printf("%s: %.2f C looks like a glitch, waiting for confirmation\n", SENSORS[i].name, t);
    return false;
  }

  s.unconfirmed = NAN;
  s.lastGood = t;
  s.badReadings = 0;
  if (s.faulty) logEvent(EV_FAULT_CLEAR, i, toHundredths(t));
  s.faulty = false;
  s.historySum += t;
  s.historyCount++;
  updateAlarm(i, t);
  return true;
}

void registerBadReading(size_t i) {
  SensorState& s = state[i];
  Serial.printf("%s: no valid reading\n", SENSORS[i].name);
  if (s.badReadings < 255) s.badReadings++;
  if (s.badReadings >= FAULT_AFTER && !s.faulty) {
    s.faulty = true;
    logEvent(EV_FAULT, i, NO_VALUE);
  }
}

// ------------------------------------------------------------------ trends and warnings

void pushRecent(const float* values) {
  memcpy(recent[recentNext], values, sizeof(recent[0]));
  recentNext = (recentNext + 1) % TREND_MINUTES;
  if (recentCount < TREND_MINUTES) recentCount++;
}

// Least-squares slope of the last TREND_MINUTES minutes in degrees per hour; NAN with fewer than 10 readings.
float trendPerHour(size_t i) {
  float sx = 0, sy = 0, sxx = 0, sxy = 0;
  uint8_t n = 0;
  for (uint8_t k = 0; k < recentCount; k++) {
    float v = recent[(recentNext + TREND_MINUTES - recentCount + k) % TREND_MINUTES][i];
    if (isnan(v)) continue;
    sx += k;
    sy += v;
    sxx += (float)k * k;
    sxy += k * v;
    n++;
  }
  float d = n * sxx - sx * sx;
  return (n < 10 || d == 0) ? NAN : (n * sxy - sx * sy) / d * 60;
}

// A warning switches on after its condition held for holdOn minutes and off after it was gone for holdOff minutes;
// a warning switched off in the settings clears at once.
void setWarning(WarningId w, bool condition, uint8_t holdOn, uint8_t holdOff, bool enabled, float value) {
  WarningState& s = warnings[w];
  bool before = s.active;
  if (!enabled) {
    s.active = false;
    s.pending = s.calm = 0;
  } else if (condition) {
    s.calm = 0;
    if (!s.active && ++s.pending >= holdOn) s.active = true;
  } else {
    s.pending = 0;
    if (s.active && ++s.calm >= holdOff) s.active = false;
  }
  if (s.active == before) return;
  s.contact->setVal(s.active ? 1 : 0);
  Serial.printf("Warning %s: %s\n", WARNING_NAMES[w], s.active ? "ON" : "off");
  logEvent(s.active ? EV_WARN_ON : EV_WARN_OFF, w, toHundredths(value));
}

float burnThreshold() {
  return isnan(state[FURNACE].low) ? 40 : state[FURNACE].low;
}

void evaluateWarnings() {
  // Furnace dying out: only while it still burns (above its min limit, or 30 degrees without one).
  const SensorState& f = state[FURNACE];
  float slope = trendPerHour(FURNACE);
  bool burning = !f.faulty && !isnan(f.lastGood) && f.lastGood > (isnan(f.low) ? 30 : f.low);
  bool dying = false;
  goOutForecast = -1;
  if (burning && !isnan(slope)) {
    if (slope <= -settings.goOutRate) dying = true;
    if (!isnan(f.low) && slope < -0.5f) {
      float toLimit = (f.lastGood - f.low) / -slope * 60;
      if (toLimit <= settings.goOutHorizon) {
        dying = true;
        goOutForecast = lroundf(toLimit);
      }
    }
  }
  setWarning(WARN_GOING_OUT, dying, 5, 10, settings.goOutOn, f.lastGood);

  // Freezing: the coldest water probe, with one degree of hysteresis.
  float coldest = NAN;
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    if (SENSORS[i].water && !state[i].faulty && !isnan(state[i].lastGood))
      coldest = isnan(coldest) ? state[i].lastGood : fminf(coldest, state[i].lastGood);
  bool freezing = !isnan(coldest) && coldest < settings.freezeTemp + (warnings[WARN_FREEZE].active ? 1 : 0);
  setWarning(WARN_FREEZE, freezing, 2, 2, settings.freezeOn, coldest);

  // Embers creeping into the coal hopper: its temperature climbing fast.
  float hopper = trendPerHour(COAL_HOPPER);
  bool burnback = !isnan(hopper) && hopper >= settings.burnbackRate;
  setWarning(WARN_BURNBACK, burnback, 3, 10, settings.burnbackOn, state[COAL_HOPPER].lastGood);
}

// One reading cycle: probes -> HomeKit -> minute log -> trends -> warnings.
void readAll() {
  float values[SENSOR_COUNT];
  MinuteRecord rec;
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    float t = readSensor(i);
    bool ok = !isnan(t) && registerReading(i, t);
    if (isnan(t)) registerBadReading(i);
    values[i] = ok ? t : NAN;
    rec.v[i] = toHundredths(values[i]);
    reportSensor(i);
  }
  Serial.printf("Readings: %.2f / %.2f / %.2f / %.2f C\n", values[0], values[1], values[2], values[3]);

  time_t now = time(nullptr);
  if (clockSet(now)) minutes.add(now / 60, rec);
  pushRecent(values);
  evaluateWarnings();
}

// ------------------------------------------------------------------ history

uint32_t localMidnight(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
  return mktime(&lt);
}

uint32_t nextMidnight(uint32_t midnight) {
  return localMidnight(midnight + 36 * 3600);   // 23 or 25 hour days included
}

// Daily min / avg / max and the furnace's burning minutes, from the minute log (survives reboots).
void summarizeDay(uint32_t dayStart, uint32_t dayEnd) {
  float mn[SENSOR_COUNT], mx[SENSOR_COUNT], sum[SENSOR_COUNT] = {};
  uint16_t count[SENSOR_COUNT] = {};
  uint16_t burn = 0;
  float threshold = burnThreshold();
  for (size_t i = 0; i < SENSOR_COUNT; i++) { mn[i] = INFINITY; mx[i] = -INFINITY; }
  minutes.forEach(dayStart / 60, dayEnd / 60 - 1, [&](uint32_t, const MinuteRecord& r) {
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
      if (r.v[i] == NO_VALUE) continue;
      float t = r.v[i] / 100.0f;
      mn[i] = fminf(mn[i], t);
      mx[i] = fmaxf(mx[i], t);
      sum[i] += t;
      count[i]++;
    }
    if (r.v[FURNACE] != NO_VALUE && r.v[FURNACE] / 100.0f >= threshold) burn++;
  });

  DayRecord d = {};
  d.time = dayStart;
  bool any = false;
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    d.min[i] = count[i] ? toHundredths(mn[i]) : NO_VALUE;
    d.avg[i] = count[i] ? toHundredths(sum[i] / count[i]) : NO_VALUE;
    d.max[i] = count[i] ? toHundredths(mx[i]) : NO_VALUE;
    any |= count[i] > 0;
  }
  d.burnMinutes = burn;
  if (any) daily.add(d);
}

// Runs every loop once NTP has set the clock: pending events, Eve entries, daily summaries.
void logHistory() {
  time_t now = time(nullptr);
  if (!clockSet(now)) return;
  if (pendingEventCount) flushPendingEvents(now);

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

  uint32_t today = localMidnight(now);
  if (today == currentDay) return;
  if (!currentDay) {
    // First time after boot: summarize the days missed while the device was off (as far as the minute log goes).
    uint32_t last = daily.newestTime();
    uint32_t d = last ? nextMidnight(last) : localMidnight((time_t)minutes.oldest() * 60);
    for (uint8_t guard = 0; minutes.newest() && d < today && guard < 32; guard++, d = nextMidnight(d))
      summarizeDay(d, nextMidnight(d));
  } else {
    summarizeDay(currentDay, today);
  }
  currentDay = today;
}

void setupHistory() {
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed - no history");
  // Files of an older layout would eat the space of the new ones: a layout change wipes the history once.
  if (prefs.getUChar("fsLayout", 0) != FS_LAYOUT) {
    Serial.println("History layout changed - formatting LittleFS");
    LittleFS.format();
    prefs.putUChar("fsLayout", FS_LAYOUT);
  }
  if (!minutes.begin()) Serial.println("No \"minutes\" partition - per-minute history disabled");
  daily.begin();
  events.begin();
  Serial.printf("LittleFS: %u of %u KB used\n", (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  for (auto& row : recent)
    for (auto& v : row) v = NAN;
}

// ------------------------------------------------------------------ settings and limits (flash)

String prefKey(const char* kind, size_t i) {
  return String(kind) + i;
}

void loadLimits() {
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    String lo = prefKey("lo", i), hi = prefKey("hi", i);
    state[i].low  = prefs.isKey(lo.c_str()) ? prefs.getFloat(lo.c_str()) : SENSORS[i].defaultLow;
    state[i].high = prefs.isKey(hi.c_str()) ? prefs.getFloat(hi.c_str()) : SENSORS[i].defaultHigh;
  }
}

void loadSettings() {
  Settings d;
  settings.goOutOn      = prefs.getBool("goOn", d.goOutOn);
  settings.goOutRate    = prefs.getFloat("goRate", d.goOutRate);
  settings.goOutHorizon = prefs.getUShort("goHor", d.goOutHorizon);
  settings.freezeOn     = prefs.getBool("frOn", d.freezeOn);
  settings.freezeTemp   = prefs.getFloat("frTemp", d.freezeTemp);
  settings.burnbackOn   = prefs.getBool("bbOn", d.burnbackOn);
  settings.burnbackRate = prefs.getFloat("bbRate", d.burnbackRate);
  settings.hotWater     = prefs.getFloat("hotWater", d.hotWater);
  settings.lat          = prefs.getFloat("lat", d.lat);
  settings.lon          = prefs.getFloat("lon", d.lon);
  prefs.getString("place", settings.place, sizeof(settings.place));
}

void saveSettings() {
  prefs.putBool("goOn", settings.goOutOn);
  prefs.putFloat("goRate", settings.goOutRate);
  prefs.putUShort("goHor", settings.goOutHorizon);
  prefs.putBool("frOn", settings.freezeOn);
  prefs.putFloat("frTemp", settings.freezeTemp);
  prefs.putBool("bbOn", settings.burnbackOn);
  prefs.putFloat("bbRate", settings.burnbackRate);
  prefs.putFloat("hotWater", settings.hotWater);
  prefs.putFloat("lat", settings.lat);
  prefs.putFloat("lon", settings.lon);
  prefs.putString("place", settings.place);
}

float parseNumber(String v) {
  v.trim();
  v.replace(',', '.');
  return v.length() ? v.toFloat() : NAN;
}

float parseLimit(const String& v) {
  float f = parseNumber(v);
  return (f < -55 || f > 125) ? NAN : f;
}

// ------------------------------------------------------------------ web app + API

String jsonNumber(float v, unsigned decimals = 2) {
  return isnan(v) ? String("null") : String(v, decimals);
}

void handleApp() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", (const char*)WEBAPP_HTML_GZ, WEBAPP_HTML_GZ_LEN);
}

void handleIcon() {
  server.sendHeader("Cache-Control", "max-age=604800");
  server.send_P(200, "image/png", (const char*)WEBAPP_ICON_PNG, WEBAPP_ICON_PNG_LEN);
}

void handleManifest() {
  server.send_P(200, "application/manifest+json", WEBAPP_MANIFEST);
}

// Current readings, trends, limits, warnings, settings and device health.
void handleNow() {
  String j;
  j.reserve(1800);
  j += "{\"time\":";     j += (uint32_t)time(nullptr);
  j += ",\"uptime\":";   j += (uint32_t)(millis() / 1000);
  j += ",\"rssi\":";     j += (int)WiFi.RSSI();
  j += ",\"ip\":\"";     j += WiFi.localIP().toString();
  j += "\",\"heap\":";   j += (uint32_t)ESP.getFreeHeap();
  j += ",\"fsUsed\":";   j += (uint32_t)LittleFS.usedBytes();
  j += ",\"fsTotal\":";  j += (uint32_t)LittleFS.totalBytes();
  j += ",\"oldest\":";   j += minutes.oldest() * 60;
  j += ",\"fw\":";       j += FW_VERSION;
  j += ",\"update\":\""; j += updater.status();
  j += "\",\"sensors\":[";
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    const SensorState& s = state[i];
    if (i) j += ',';
    j += "{\"name\":\"";  j += SENSORS[i].name;
    j += "\",\"temp\":";  j += s.faulty ? String("null") : jsonNumber(s.lastGood);
    j += ",\"trend\":";   j += jsonNumber(trendPerHour(i), 1);
    j += ",\"low\":";     j += jsonNumber(s.low, 1);
    j += ",\"high\":";    j += jsonNumber(s.high, 1);
    j += ",\"alarm\":";   j += (int)s.alarm;
    j += ",\"fault\":";   j += s.faulty ? "true" : "false";
    j += ",\"water\":";   j += SENSORS[i].water ? "true" : "false";
    j += '}';
  }
  j += "],\"warnings\":[";
  for (uint8_t w = 0; w < WARN_COUNT; w++) {
    if (w) j += ',';
    j += warnings[w].active ? "true" : "false";
  }
  j += "],\"forecast\":"; j += goOutForecast < 0 ? String("null") : String(goOutForecast);
  j += ",\"settings\":{\"goOutOn\":"; j += settings.goOutOn ? "true" : "false";
  j += ",\"goOutRate\":";    j += jsonNumber(settings.goOutRate, 1);
  j += ",\"goOutHorizon\":"; j += settings.goOutHorizon;
  j += ",\"freezeOn\":";     j += settings.freezeOn ? "true" : "false";
  j += ",\"freezeTemp\":";   j += jsonNumber(settings.freezeTemp, 1);
  j += ",\"burnbackOn\":";   j += settings.burnbackOn ? "true" : "false";
  j += ",\"burnbackRate\":"; j += jsonNumber(settings.burnbackRate, 1);
  j += ",\"hotWater\":";     j += jsonNumber(settings.hotWater, 1);
  j += ",\"lat\":";          j += jsonNumber(settings.lat, 4);
  j += ",\"lon\":";          j += jsonNumber(settings.lon, 4);
  j += ",\"place\":\"";      j += settings.place;
  j += "\"}}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", j);
}

// Chunked binary responses: collects small writes into one buffer.
struct ChunkWriter {
  uint8_t buf[600];
  size_t  n = 0;
  void begin(const char* type) {
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, type, "");
  }
  void put(const void* p, size_t len) {
    if (n + len > sizeof(buf)) flush();
    memcpy(buf + n, p, len);
    n += len;
  }
  void put(const String& s) {
    for (size_t k = 0; k < s.length(); k += sizeof(buf)) put(s.c_str() + k, min(s.length() - k, sizeof(buf)));
  }
  void flush() {
    if (n) server.sendContent((const char*)buf, n);
    n = 0;
  }
  void end() {
    flush();
    server.sendContent("");
  }
};

// Walks the minute log in buckets of `step` seconds and calls f(bucketStart, count, min, sum, max) per bucket
// that has any reading; values are degrees x 100.
template <typename F> void forEachBucket(uint32_t from, uint32_t to, uint32_t step, F f) {
  uint32_t bucket = UINT32_MAX;
  int32_t sum[SENSOR_COUNT];
  int16_t mn[SENSOR_COUNT], mx[SENSOR_COUNT];
  uint16_t count[SENSOR_COUNT];
  auto reset = [&] {
    for (size_t i = 0; i < SENSOR_COUNT; i++) { sum[i] = 0; mn[i] = INT16_MAX; mx[i] = INT16_MIN + 1; count[i] = 0; }
  };
  auto emit = [&] {
    for (size_t i = 0; i < SENSOR_COUNT; i++)
      if (count[i]) { f(bucket * step, count, mn, sum, mx); return; }
  };
  reset();
  minutes.forEach(from / 60, to / 60, [&](uint32_t m, const MinuteRecord& r) {
    uint32_t b = m * 60 / step;
    if (b != bucket) {
      if (bucket != UINT32_MAX) emit();
      reset();
      bucket = b;
    }
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
      int16_t v = r.v[i];
      if (v == NO_VALUE) continue;
      sum[i] += v;
      mn[i] = min(mn[i], v);
      mx[i] = max(mx[i], v);
      count[i]++;
    }
  });
  if (bucket != UINT32_MAX) emit();
}

uint32_t argOr(const char* name, uint32_t fallback) {
  return server.hasArg(name) ? strtoul(server.arg(name).c_str(), nullptr, 10) : fallback;
}

// GET from, to, step (seconds, multiple of 60). Binary, oldest first: uint32 time, then per probe
// int16 min, avg, max (degrees x 100, NO_VALUE when the probe had no reading in the bucket).
void handleSeries() {
  uint32_t to = argOr("to", time(nullptr)), from = argOr("from", to - 6 * 3600);
  uint32_t step = max((uint32_t)60, argOr("step", 60) / 60 * 60);
  ChunkWriter w;
  w.begin("application/octet-stream");
  forEachBucket(from, to, step, [&](uint32_t t, const uint16_t* count, const int16_t* mn, const int32_t* sum, const int16_t* mx) {
    w.put(&t, 4);
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
      int16_t v[3] = {NO_VALUE, NO_VALUE, NO_VALUE};
      if (count[i]) { v[0] = mn[i]; v[1] = (int16_t)(sum[i] / count[i]); v[2] = mx[i]; }
      w.put(v, sizeof(v));
    }
  });
  w.end();
}

// GET since. Binary DayRecords, oldest first.
void handleDaily() {
  ChunkWriter w;
  w.begin("application/octet-stream");
  daily.forEach(argOr("since", 0), [&](const DayRecord& d) { w.put(&d, sizeof(d)); });
  w.end();
}

// GET since. JSON [[time, type, sensor, value], ...], oldest first.
void handleEvents() {
  ChunkWriter w;
  w.begin("application/json");
  w.put("[", 1);
  bool first = true;
  events.forEach(argOr("since", 1), [&](const EventRecord& e) {
    char line[48];
    int n = snprintf(line, sizeof(line), "%s[%u,%u,%u,%d]", first ? "" : ",", (unsigned)e.time, e.type, e.sensor, e.value);
    w.put(line, n);
    first = false;
  });
  w.put("]", 1);
  w.end();
}

// GET from, to, step. Semicolon CSV with decimal commas and local times - opens as-is in a Polish Excel.
void handleCsv() {
  uint32_t to = argOr("to", time(nullptr)), from = argOr("from", to - 86400);
  uint32_t step = max((uint32_t)60, argOr("step", 60) / 60 * 60);
  server.sendHeader("Content-Disposition", "attachment; filename=\"piec.csv\"");
  ChunkWriter w;
  w.begin("text/csv; charset=utf-8");
  String head = "\xEF\xBB\xBF" "Czas";
  for (auto& s : SENSORS) head += String(";") + s.name;
  w.put(head + "\r\n");
  forEachBucket(from, to, step, [&](uint32_t t, const uint16_t* count, const int16_t*, const int32_t* sum, const int16_t*) {
    time_t tt = t;
    struct tm lt;
    localtime_r(&tt, &lt);
    char line[96];
    size_t n = strftime(line, sizeof(line), "%Y-%m-%d %H:%M", &lt);
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
      if (!count[i]) { n += snprintf(line + n, sizeof(line) - n, ";"); continue; }
      int32_t v = sum[i] / count[i];
      n += snprintf(line + n, sizeof(line) - n, ";%s%d,%02d", v < 0 ? "-" : "", (int)(abs(v) / 100), (int)(abs(v) % 100));
    }
    n += snprintf(line + n, sizeof(line) - n, "\r\n");
    w.put(line, n);
  });
  w.end();
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
  // New limits - judge the current value from scratch, without the old hysteresis; a still-active alarm
  // is not logged again, one that ends by the change gets its end in the journal.
  int8_t before = s.alarm;
  s.alarm = isnan(s.lastGood) ? 0 : s.lastGood >= s.high ? 1 : s.lastGood <= s.low ? -1 : 0;
  logAlarmChange(i, before, s.lastGood);
  reportSensor(i);
  handleNow();
}

// POST any of the Settings fields (booleans as 1/0). Answers with the same JSON as /api/now.
void handleSettings() {
  auto flag = [](const char* k, bool& v) { if (server.hasArg(k)) v = server.arg(k) == "1"; };
  auto number = [](const char* k, float& v, float lo, float hi) {
    if (!server.hasArg(k)) return;
    float f = parseNumber(server.arg(k));
    if (!isnan(f) && f >= lo && f <= hi) v = f;
  };
  flag("goOutOn", settings.goOutOn);
  number("goOutRate", settings.goOutRate, 1, 60);
  if (server.hasArg("goOutHorizon")) settings.goOutHorizon = constrain(server.arg("goOutHorizon").toInt(), 5, 240);
  flag("freezeOn", settings.freezeOn);
  number("freezeTemp", settings.freezeTemp, -20, 20);
  flag("burnbackOn", settings.burnbackOn);
  number("burnbackRate", settings.burnbackRate, 1, 60);
  number("hotWater", settings.hotWater, 20, 90);
  if (server.hasArg("lat") && server.hasArg("lon")) {
    settings.lat = parseNumber(server.arg("lat"));
    settings.lon = parseNumber(server.arg("lon"));
    String place = server.arg("place");
    place.replace("\"", "");
    place.replace("\\", "");
    strlcpy(settings.place, place.c_str(), sizeof(settings.place));
  }
  saveSettings();
  evaluateWarnings();
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

// POST: wipes every history (minutes, days, Eve, journal) and restarts; the journal then starts with a note.
void handleClearHistory() {
  server.send(200, "text/plain", "history cleared, restarting");
  prefs.putBool("cleared", true);
  Serial.println("History cleared from the web app - restarting");
  delay(300);
  minutes.clear();
  LittleFS.format();
  ESP.restart();
}

// The web app's "install from file" uses the network-OTA password: HomeSpan keeps its SHA-256 in NVS OTA/OTADATA.
bool otaPasswordMatches(const String& password) {
  char stored[65] = "";
  size_t len = sizeof(stored);
  nvs_handle_t nvs;
  bool found = false;
  if (nvs_open("OTA", NVS_READONLY, &nvs) == ESP_OK) {
    found = nvs_get_str(nvs, "OTADATA", stored, &len) == ESP_OK;
    nvs_close(nvs);
  }
  if (!found) return password == DEFAULT_OTA_PASSWORD;

  uint8_t digest[32];
  mbedtls_sha256((const uint8_t*)password.c_str(), password.length(), digest, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
  return String(stored).equalsIgnoreCase(hex);
}

String firmwareUploadError;

// POST /api/firmware?pwd=... with the .bin as a multipart file. Goes to the spare app slot and boots on
// probation like a remote update (RemoteUpdate.h), so a bad file rolls back by itself.
void handleFirmwareChunk() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    firmwareUploadError = otaPasswordMatches(server.arg("pwd")) ? "" : "wrong password";
    if (firmwareUploadError.isEmpty() && !Update.begin(UPDATE_SIZE_UNKNOWN)) firmwareUploadError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (firmwareUploadError.isEmpty() && Update.write(up.buf, up.currentSize) != up.currentSize) firmwareUploadError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (firmwareUploadError.isEmpty() && !Update.end(true)) firmwareUploadError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    firmwareUploadError = "upload aborted";
  }
}

void handleFirmwareDone() {
  if (!firmwareUploadError.isEmpty()) {
    if (Update.isRunning()) Update.abort();
    server.send(firmwareUploadError == "wrong password" ? 403 : 500, "text/plain", firmwareUploadError);
    return;
  }
  server.send(200, "text/plain", "installed, restarting");
  Serial.println("Firmware uploaded from the web app - restarting");
  delay(500);
  ESP.restart();
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
  server.on("/api/series", HTTP_GET, handleSeries);
  server.on("/api/daily", HTTP_GET, handleDaily);
  server.on("/api/events", HTTP_GET, handleEvents);
  server.on("/api/csv", HTTP_GET, handleCsv);
  server.on("/api/limits", HTTP_POST, handleLimits);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/bus", HTTP_GET, handleBus);
  server.on("/api/update", HTTP_POST, handleUpdate);
  server.on("/api/clear-history", HTTP_POST, handleClearHistory);
  server.on("/api/firmware", HTTP_POST, handleFirmwareDone, handleFirmwareChunk);
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
    minutes.clear();                                         // all history
    LittleFS.format();
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
  prefs.begin("furnace", false);
  loadLimits();
  loadSettings();
  setupHistory();
  logEvent(EV_BOOT, 0, FW_VERSION);
  if (prefs.getBool("cleared", false)) {
    logEvent(EV_HISTORY_CLEARED, 0, 0);
    prefs.remove("cleared");
  }

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
  homeSpan.begin(Category::Bridges, "Czujniki pieca", DEVICE_NAME);
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
  if (updater.takeJustConfirmed()) logEvent(EV_UPDATED, 0, FW_VERSION);

  // Readings on the full minute once NTP has set the clock, every READ_INTERVAL_MS before that.
  time_t wall = time(nullptr);
  bool due = clockSet(wall) ? (uint32_t)(wall / 60) != lastRequestMinute : millis() - lastRequestMs >= READ_INTERVAL_MS;
  if (!conversionPending && due) {
    bus.requestTemperatures();   // returns at once, the probes convert on their own
    lastRequestMs = millis();
    lastRequestMinute = wall / 60;
    conversionPending = true;
  } else if (conversionPending && millis() - lastRequestMs >= CONVERSION_MS) {
    conversionPending = false;
    readAll();
  }
}
