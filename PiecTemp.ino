// Furnace temperature monitor
// ESP32 + DS18B20 probes on one 1-Wire bus, exposed straight to Apple Home through HomeSpan (no bridge, no Pi).
//
// Apple Home (all names in Polish - the household's language):
//   name       - temperature sensor per probe; gets a fault flag when the probe stops answering
//   alarmName  - contact sensor per probe that OPENS when the temperature leaves its limits or the probe dies
//   warnings   - six contact sensors (furnace dying out, freezing, embers creeping into the coal hopper, low coal,
//                furnace stalled after a power cut, ash pan due); summer mode quiets the ones about burning
//   hot water  - a contact sensor that opens when the boiler is warm enough (not a problem - "you can shower")
//   Turn on notifications for the contact sensors in the Home app to get a push on the phone; Home sends one
//   when a contact opens and one when it closes.
// Coal: the web app records refills in buckets; the coal burned is taken as proportional to the furnace's heat
// output and the factor is learnt from refills "to the top" (see recomputeCoal) - daily use, costs, hopper level,
// time left.
// Pairing: Home app -> Add Accessory -> More options -> "Czujniki pieca", setup code 466-37-726.
// Eve app: every temperature sensor carries an Eve history (EveHistory.h).
//
// Web app http://furnace.local:8080 on the home WiFi: live cards, charts from 6 hours to years, warnings and
// limits, event journal. Source in web/index.html, packed into WebApp.h by web/build.ps1.
// Backup (Backup.h): the whole device in one file - restored on a new board it keeps the history, settings,
// WiFi, HomeKit pairing and remote access (Ustawienia -> Kopia zapasowa).
// Remote access (Relay.h): the same app from GitHub Pages, anywhere - through a Cloudflare relay (relay/), end-to-end
// encrypted with a key paired in the home app (Ustawienia -> Dostep zdalny). Nothing to open on the router.
// History (HistoryLog.h): every minute for 31 days in its own flash partition; daily summaries for 2.5-5 years,
// the event journal and coal refills appended to segment files and 14 days of Eve entries on LittleFS. The
// newest data always replaces the oldest, nothing ever fills up.
//
// WiFi: no credentials in the code. Without stored WiFi the ESP opens the open hotspot AP_SSID; join it with
// the phone, pick the network and type its password - it is kept in the ESP's flash.
// To give it a new network:
//   - hold the BOOT button ~5 s and release it: the stored WiFi is forgotten and the hotspot opens,
//   - hold it ~15 s and release: FULL reset before moving to another home - WiFi, HomeKit pairing and all
//     history are erased (limits, warning settings and the OTA password stay),
//   - or Serial "X" (forget WiFi) / "A" (open the hotspot).
// A router that is down does not open the hotspot: the ESP keeps measuring and recording and reconnects by itself
// (a hotspot blocks everything for its whole timeout).
// Serial commands work for SERIAL_WINDOW_MS after a boot (opening the Serial Monitor resets the board); then the
// input is switched off - a stray byte would otherwise make HomeSpan wait for a newline forever.
// The blue LED (if the board has one on GPIO2) shows the HomeSpan status.
//
// Updates:
//   - over the internet (any home): bump FW_VERSION and its CHANGES note in web/index.html, run
//     release/publish.ps1; the ESP checks UPDATE_MANIFEST_URL every hour (or "@u" / the web app), the web app
//     offers the new version with its notes and installs it on a tap; it rolls back if it does not come online,
//   - on the same WiFi: web app -> Ustawienia -> Oprogramowanie -> install a .bin file, or Arduino IDE -> Tools ->
//     Port -> "furnace at ..." (both use the OTA password set with the serial command "O"; without a password
//     set both stay off - HomeSpan would otherwise accept its public default).
//
// Arduino IDE: board "ESP32 Dev Module"; the partition table comes from partitions.csv in this folder
// (2 x 1.69 MB app slots for OTA, 352 KB minute ring, 16 KB alive marks, 208 KB LittleFS). A partition change
// needs a USB upload.
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
#include <algorithm>
#include <vector>
#include "Backup.h"
#include "EveHistory.h"
#include "HistoryLog.h"
#include "RemoteUpdate.h"
#include "Relay.h"
#include "WebApp.h"

// ------------------------------------------------------------------ configuration

constexpr uint32_t FW_VERSION = 14;                   // bump for every release (release/publish.ps1)
const char* UPDATE_MANIFEST_URL =
  "https://raw.githubusercontent.com/klimabartlomiej/furnace-monitor/main/release/manifest.json";

const char* DEVICE_NAME  = "furnace";                       // host name: furnace.local
const char* RELAY_HOST   = "furnace-relay.klimsoft.workers.dev";   // remote access relay (relay/); the
                                                            // web app can set another one or "" = off
const char* AP_SSID      = "Furnace-Setup";                 // WiFi setup hotspot
const char* AP_PASSWORD  = "";                              // empty = open hotspot
const char* TIME_ZONE    = "CET-1CEST,M3.5.0,M10.5.0/3";    // Poland; decides where a day starts

constexpr uint16_t AP_TIMEOUT_S       = 15 * 60;         // hotspot gives up (and restarts) after this long
constexpr uint32_t WIFI_RESET_HOLD_MS = 5000;            // hold BOOT this long to forget the WiFi
constexpr uint32_t FULL_RESET_HOLD_MS = 15000;           // ... this long to also forget HomeKit and the history
constexpr uint32_t SERIAL_WINDOW_MS   = 5 * 60 * 1000;   // serial commands accepted this long after a boot
constexpr uint16_t WATCHDOG_S         = 60;              // a loop stuck this long restarts the ESP
constexpr uint32_t HISTORY_INTERVAL_S = 600;             // one Eve entry per 10 min, like Eve's own sensors
constexpr uint32_t DAILY_CAPACITY     = 5 * 366;         // daily summaries kept at most (5 years; at least half)
constexpr uint32_t EVENT_CAPACITY     = 1024;            // event journal entries (at most; at least half)
constexpr uint32_t REFILL_CAPACITY    = 512;             // coal refills kept (at most; at least half)
constexpr uint8_t  COAL_INTERVALS     = 10;              // latest refill-to-refill intervals the kg factor averages
constexpr float    COAL_MIN_HEAT      = 600;             // degree-minutes an interval needs to count (10 degrees, 1 h)
constexpr float    HOT_WATER_HYSTERESIS = 3;             // degrees below "ready" before the hot-water contact closes
constexpr uint16_t SEASON_AUTO_MIN    = 6 * 60;          // minutes of burning that end summer mode by themselves -
                                                         // longer than firing it up for hot water in summer
constexpr uint32_t STALL_WATCH_MS     = 3 * 3600 * 1000; // after a power cut, how long a cooling furnace is watched
constexpr float    STALL_DROP         = 10;              // ... it counts as stalled once this much cooler (an idling
constexpr float    STALL_SLOPE        = -4;              // ... controller drops a few degrees) and still falling this
                                                         // fast (degrees/hour)
constexpr uint8_t  FS_LAYOUT          = 6;               // bump when the history files change shape (wipes them once)
constexpr uint32_t ALIVE_INTERVAL_S   = 2;               // "still alive" mark - how exactly a power cut is timed
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
constexpr size_t FURNACE      = 0;   // probes the warnings and the coal estimate look at
constexpr size_t RETURN_WATER = 1;
constexpr size_t COAL_HOPPER  = 2;
constexpr size_t BOILER       = 3;

// Warnings - each one is a contact sensor in Apple Home and a banner in the web app; tuned in the web app.
enum WarningId : uint8_t { WARN_GOING_OUT, WARN_FREEZE, WARN_BURNBACK, WARN_LOW_COAL, WARN_STALLED, WARN_ASH, WARN_COUNT };
const char* WARNING_NAMES[WARN_COUNT] = { "Piec wygasa", "Ryzyko zamarznięcia", "Cofanie żaru", "Mało węgla",
                                          "Piec stoi po zaniku prądu", "Opróżnij popielnik" };
const char* HOT_WATER_NAME = "Ciepła woda gotowa";

struct Settings {
  bool     goOutOn      = true;   // furnace dying out: falling faster than goOutRate (degrees/hour), or the
  float    goOutRate    = 8;      // trend reaches its min limit within goOutHorizon minutes
  uint16_t goOutHorizon = 45;
  bool     freezeOn     = true;   // any water probe at or below freezeTemp
  float    freezeTemp   = 5;
  bool     burnbackOn   = true;   // coal hopper warming faster than burnbackRate (degrees/hour)
  float    burnbackRate = 10;
  float    hotWater     = 45;     // boiler temperature that counts as "hot water ready" (web app + Home contact)
  bool     coalOn       = true;   // coal tracking; off = no coal card, refills or low-coal warning
  float    hopperBuckets = 0;     // buckets the hopper holds, 0 = not set (no level estimate)
  float    bucketKg     = 10;     // one bucket of coal - only for kg and costs; the coal model counts buckets
  float    coalPrice    = 0;      // PLN per tonne, 0 = no costs
  bool     lowCoalOn    = true;   // the hopper estimate runs out within lowCoalHours
  float    lowCoalHours = 12;
  bool     heating      = true;   // heating season; off = summer: no "too cold" on the furnace and the return, no
                                  // burning warnings (comes back by itself after SEASON_AUTO_MIN of burning)
  bool     stalledOn    = true;   // after a power cut the furnace keeps cooling - its controller did not restart
  bool     ashOn        = true;   // ash pan reminder: after ashLimit buckets burned or ashDays since emptied
  float    ashLimit     = 15;
  float    ashDays      = 7;
  float    offset[SENSOR_COUNT] = {};   // per probe, added to every reading: a probe reading 2 degrees low gets +2
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
  EV_WARN_ON, EV_WARN_OFF, EV_HISTORY_CLEARED, EV_POWER_CUT,
  EV_SEASON,            // value 1 = heating season on, 0 = summer mode; sensor 1 = switched on by itself
  EV_ASH,               // ash pan emptied
};

// Why the ESP started - the `sensor` byte of EV_BOOT and EV_POWER_CUT.
enum BootReason : uint8_t { BOOT_OTHER, BOOT_POWER_ON, BOOT_RESTART, BOOT_BROWNOUT, BOOT_CRASH };

struct __attribute__((packed)) EventRecord {
  uint32_t time;
  uint8_t  type;        // EventType
  uint8_t  sensor;      // probe index, WarningId for EV_WARN_*, BootReason for EV_BOOT
  int16_t  value;       // degrees x 100, or the firmware version (EV_BOOT, EV_UPDATED)
  // EV_POWER_CUT: time = power back, seconds without power = (sensor & 0x7F) << 16 | (uint16_t)value,
  // sensor bit 7 = the start was a brownout (voltage dip) rather than a full power-on.
};

struct __attribute__((packed)) DayRecord {
  uint32_t time;                      // local midnight
  int16_t  min[SENSOR_COUNT];         // degrees x 100, NO_VALUE when the probe had no reading that day
  int16_t  avg[SENSOR_COUNT];
  int16_t  max[SENSOR_COUNT];
  uint16_t burnMinutes;               // minutes the furnace was at or above burnThreshold()
  uint32_t heat;                      // degree-minutes of furnace minus return (heatOf) - coal used = coalK x heat
};

struct __attribute__((packed)) RefillRecord {
  uint32_t time;                      // when the coal went in (may be entered later)
  uint16_t amount10;                  // buckets x 10 (coal is carried up loose in buckets)
  uint8_t  full;                      // 1 = filled the hopper to the top
  uint8_t  reserved;
};

using MinuteRecord = MinuteLog<SENSOR_COUNT>::Record;

OneWire                 oneWire(ONE_WIRE_PIN);
DallasTemperature       bus(&oneWire);
WebServer               server(WEB_PORT);
Preferences             prefs;
RemoteUpdate            updater(UPDATE_MANIFEST_URL, FW_VERSION);
Relay                   relay;
MinuteLog<SENSOR_COUNT> minutes("minutes");                 // partition label in partitions.csv (31 days)
AliveLog                alive("alive");
SegmentLog<DayRecord>   daily("/day", DAILY_CAPACITY / 2);
SegmentLog<EventRecord> events("/ev", EVENT_CAPACITY / 2);
SegmentLog<RefillRecord> refills("/fill", REFILL_CAPACITY / 2);
Backup<SENSOR_COUNT>    backup(minutes);
Settings                settings;
SensorState             state[SENSOR_COUNT];
WarningState            warnings[WARN_COUNT];
char                    firmwareRevision[12];

float    recent[TREND_MINUTES][SENSOR_COUNT];   // last minutes for the trends, NAN = no reading
uint8_t  recentNext        = 0;
uint8_t  recentCount       = 0;
int16_t  goOutForecast     = -1;                // minutes until the furnace reaches its min limit, -1 = not heading there
EventRecord pendingEvents[8];                   // events from before NTP set the clock, time = seconds since boot
uint8_t  pendingEventCount = 0;
BootReason bootReason      = BOOT_OTHER;
uint32_t powerLostAt       = 0;                 // estimated start of the power cut this boot ended, 0 = none to log
uint32_t lastAliveMark     = 0;
SpanCharacteristic* hotWaterContact = nullptr;
bool     hotWaterReady     = false;

// Coal is counted in buckets - what goes into the hopper; kilograms and costs are the web app's (x bucketKg).
float    coalK             = NAN;               // buckets of coal per degree-minute, learnt from refills (flash "coalK")
uint16_t coalSamples       = 0;                 // refill-to-refill intervals it was learnt from (flash "coalN")
uint32_t lastFullAt        = 0;                 // last refill to the top, 0 = none
float    addedSinceFull    = 0;                 // buckets put in since then without filling up
float    heatSinceFull     = NAN;               // degree-minutes since then, NAN = beyond the minute log
RefillRecord lastRefill    = {};                // the newest refill (time 0 = none)
float    coalLevel         = NAN;               // buckets in the hopper (estimate)
float    coalRate          = NAN;               // buckets per hour over the last day
float    coalHoursLeft     = NAN;               // at that pace; INFINITY when the furnace hardly burns
uint32_t ashAt             = 0;                 // ash pan last emptied (flash "ashAt"); counting starts with the clock
float    heatSinceAsh      = NAN;               // degree-minutes since then, NAN = beyond the minute log
float    ashBuckets        = NAN;               // coal burned since then, buckets (once coalK is known)
uint16_t burningMinutes    = 0;                 // in a row - ends summer mode at SEASON_AUTO_MIN
bool     stallWatch        = false;             // started from a power cut: watch whether the furnace comes back
float    stallFrom         = NAN;               // furnace temperature at the first reading after that cut

uint32_t lastRequestMs     = 0;
uint32_t lastRequestMinute = 0;
uint32_t lastHistorySlot   = 0;
uint32_t currentDay        = 0;   // local midnight (Unix time) of the day being summarized
bool     conversionPending = false;
bool     webStarted        = false;
bool     serialClosed      = false;   // serial input switched off after SERIAL_WINDOW_MS

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
    e.time = millis() / 1000;
    if (pendingEventCount < 8) pendingEvents[pendingEventCount++] = e;
    return;
  }
  events.add(e);
  pushEvent(e);
}

// A notification on the paired phones (Relay.h -> relay -> Web Push) for what matters away from home: alarms and
// their end, dead probes, warnings, power cuts of a minute or more, a finished update. Restored problems are not
// journaled again after a restart (SavedAlerts), so they are not announced twice. The phone words it (web/sw.js).
void pushEvent(const EventRecord& e) {
  switch (e.type) {
    case EV_UPDATED: case EV_ALARM_HIGH: case EV_ALARM_LOW: case EV_ALARM_CLEAR: case EV_FAULT: case EV_FAULT_CLEAR:
    case EV_WARN_ON:
      break;
    case EV_WARN_OFF:
      if (e.sensor == WARN_ASH) return;                     // the ash pan was emptied - nothing to tell
      break;
    case EV_POWER_CUT:
      if ((e.sensor & 0x7F) * 65536 + (uint16_t)e.value < 60) return;   // blips only in the journal
      break;
    default:
      return;
  }
  String j = String("{\"t\":") + e.time + ",\"e\":" + e.type + ",\"s\":" + e.sensor + ",\"v\":" + e.value;
  if (e.type >= EV_ALARM_HIGH && e.type <= EV_FAULT_CLEAR && e.sensor < SENSOR_COUNT) j += String(",\"n\":\"") + SENSORS[e.sensor].name + "\"";
  relay.alert(j + "}");
}

// The clock is set: events from before it get their real time (boot time + their seconds since boot).
void flushPendingEvents(time_t now) {
  uint32_t bootTime = now - millis() / 1000;
  for (uint8_t i = 0; i < pendingEventCount; i++) {
    pendingEvents[i].time += bootTime;
    events.add(pendingEvents[i]);
    pushEvent(pendingEvents[i]);
  }
  pendingEventCount = 0;
}

BootReason readBootReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return BOOT_POWER_ON;
    case ESP_RST_SW:       return BOOT_RESTART;
    case ESP_RST_BROWNOUT: return BOOT_BROWNOUT;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return BOOT_CRASH;
    default:               return BOOT_OTHER;
  }
}

// A start from a power cut puts the cut itself in the journal, from the last "still alive" mark (the ESP has
// no clock that runs without power) to this boot - every cut, a blip of a few seconds too.
void logPowerCut(time_t now) {
  uint32_t bootTime = now - millis() / 1000;
  uint32_t off = min(bootTime > powerLostAt ? bootTime - powerLostAt : 0, (uint32_t)0x7FFFFF);
  uint8_t  high = (off >> 16) | (bootReason == BOOT_BROWNOUT ? 0x80 : 0);
  EventRecord e = {bootTime, (uint8_t)EV_POWER_CUT, high, (int16_t)(uint16_t)(off & 0xFFFF)};
  events.add(e);
  pushEvent(e);
  powerLostAt = 0;
}

// ------------------------------------------------------------------ problem states across restarts

// Alarms, dead probes and warnings survive a restart (update, power cut): a problem still there afterwards
// keeps its contact open from the first second, so Apple Home does not announce it again and the journal
// does not get a second start. Written to flash only when one of them changes.
struct __attribute__((packed)) SavedAlerts {
  int8_t  alarm[SENSOR_COUNT];
  uint8_t faulty;       // bit per probe
  uint8_t warnings;     // bit per warning
  uint8_t hotWater;     // the hot-water contact is open
};

void saveAlerts() {
  SavedAlerts a = {};
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    a.alarm[i] = state[i].alarm;
    if (state[i].faulty) a.faulty |= 1 << i;
  }
  for (uint8_t w = 0; w < WARN_COUNT; w++)
    if (warnings[w].active) a.warnings |= 1 << w;
  a.hotWater = hotWaterReady;
  prefs.putBytes("alerts", &a, sizeof(a));
}

void restoreAlerts() {
  SavedAlerts a;
  if (!prefs.isKey("alerts") || prefs.getBytes("alerts", &a, sizeof(a)) != sizeof(a)) return;
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    state[i].alarm  = constrain(a.alarm[i], -1, 1);
    state[i].faulty = a.faulty >> i & 1;
    if (state[i].faulty) state[i].badReadings = FAULT_AFTER;
  }
  for (uint8_t w = 0; w < WARN_COUNT; w++) warnings[w].active = a.warnings >> w & 1;
  hotWaterReady = a.hotWater;
}

// ------------------------------------------------------------------ HomeKit

SpanCharacteristic* newContactAccessory(const char* name, bool open) {
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(name);
    new Service::ContactSensor();
      return new Characteristic::ContactSensorState(open ? 1 : 0);
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
        s.fault = new Characteristic::StatusFault(s.faulty ? 1 : 0);
        new Characteristic::TemperatureDisplayUnits(0);   // Celsius - Eve shows a weather history only with this set
      s.history = new EveHistory(s.historyName, s.historyPath);
  }

  // One alarm contact per probe, so a dead probe that keeps its alarm open never hides a new alarm elsewhere.
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    state[i].alarmContact = newContactAccessory(SENSORS[i].alarmName, state[i].alarm != 0 || state[i].faulty);
  for (uint8_t w = 0; w < WARN_COUNT; w++) warnings[w].contact = newContactAccessory(WARNING_NAMES[w], warnings[w].active);
  hotWaterContact = newContactAccessory(HOT_WATER_NAME, hotWaterReady);
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
  saveAlerts();
}

// Summer mode silences "too cold" on the furnace and the return water - they are cold on purpose then.
float lowLimit(size_t i) {
  return !settings.heating && (i == FURNACE || i == RETURN_WATER) ? NAN : state[i].low;
}

void updateAlarm(size_t i, float t) {
  SensorState& s = state[i];
  int8_t before = s.alarm;
  float low = lowLimit(i);
  // NAN limits never compare true, so a disabled limit simply never fires.
  if (t >= s.high)                                        s.alarm = +1;
  else if (t <= low)                                      s.alarm = -1;
  else if (s.alarm > 0 && t <= s.high - ALARM_HYSTERESIS) s.alarm = 0;
  else if (s.alarm < 0 && !(t < low + ALARM_HYSTERESIS))  s.alarm = 0;
  logAlarmChange(i, before, t);
}

// New limits or season - judge the current value from scratch, without the old hysteresis; a still-active alarm
// is not logged again, one that ends by the change gets its end in the journal.
// A new calibration offset moves what is already known about the probe by the same step - the trends (and the
// warnings built on them) must not see it as a sudden rise or fall. Home gets the corrected value at once.
void shiftProbe(size_t i, float delta) {
  SensorState& s = state[i];
  if (!isnan(s.lastGood)) s.lastGood += delta;
  if (!isnan(s.unconfirmed)) s.unconfirmed += delta;
  for (auto& row : recent)
    if (!isnan(row[i])) row[i] += delta;
  rejudgeAlarm(i);
}

void rejudgeAlarm(size_t i) {
  SensorState& s = state[i];
  int8_t before = s.alarm;
  s.alarm = isnan(s.lastGood) ? 0 : s.lastGood >= s.high ? 1 : s.lastGood <= lowLimit(i) ? -1 : 0;
  logAlarmChange(i, before, s.lastGood);
  reportSensor(i);
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
  if (s.faulty) {
    s.faulty = false;
    logEvent(EV_FAULT_CLEAR, i, toHundredths(t));
    saveAlerts();
  }
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
    saveAlerts();
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

// A warning switches on after its condition held for holdOn minutes and off after it was gone for holdOff minutes
// (counted only on the minute's reading, `tick`); a warning switched off in the settings clears at once. While the
// condition cannot be judged (`known` false - too few readings for a trend after a restart, no clock yet) the
// warning keeps its state.
void setWarning(WarningId w, bool condition, bool known, uint8_t holdOn, uint8_t holdOff, bool enabled, float value,
                bool tick) {
  WarningState& s = warnings[w];
  bool before = s.active;
  if (!enabled) {
    s.active = false;
    s.pending = s.calm = 0;
  } else if (!known || !tick) {
    return;
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
  saveAlerts();
}

float burnThreshold() {
  return isnan(state[FURNACE].low) ? 40 : state[FURNACE].low;
}

// A journal value is degrees x 100 in an int16 - bucket counts are capped to fit; NAN stays "no value".
float upTo320(float v) {
  return isnan(v) ? NAN : fminf(v, 320);
}

// tick = the minute's reading (advances the hold timers); other callers (settings, refills) only apply what was
// switched off.
void evaluateWarnings(bool tick = false) {
  // Summer mode: the warnings about burning stay quiet; freezing, embers in the hopper and hot water keep working.
  const bool heating = settings.heating;

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
  setWarning(WARN_GOING_OUT, dying, !burning || !isnan(slope), 5, 10, heating && settings.goOutOn, f.lastGood, tick);

  // Freezing: the coldest water probe, with one degree of hysteresis.
  float coldest = NAN;
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    if (SENSORS[i].water && !state[i].faulty && !isnan(state[i].lastGood))
      coldest = isnan(coldest) ? state[i].lastGood : fminf(coldest, state[i].lastGood);
  bool freezing = !isnan(coldest) && coldest < settings.freezeTemp + (warnings[WARN_FREEZE].active ? 1 : 0);
  setWarning(WARN_FREEZE, freezing, !isnan(coldest), 2, 2, settings.freezeOn, coldest, tick);

  // Embers creeping into the coal hopper: its temperature climbing fast. Also in summer mode (a fire for hot
  // water), and a probe that dies - embers may have destroyed it - keeps the warning as it was.
  float hopper = trendPerHour(COAL_HOPPER);
  bool burnback = !isnan(hopper) && hopper >= settings.burnbackRate;
  setWarning(WARN_BURNBACK, burnback, !isnan(hopper) && !state[COAL_HOPPER].faulty, 3, 10, settings.burnbackOn,
             state[COAL_HOPPER].lastGood, tick);

  // Low coal: at the current pace the hopper estimate runs out within lowCoalHours, or is nearly empty
  // (half again as long before it clears, so a wobbling pace does not flap it). Logged with the buckets left.
  float lowHours = settings.lowCoalHours * (warnings[WARN_LOW_COAL].active ? 1.5f : 1);
  // Unknown while there is a model but no estimate yet (after a power cut, before the clock).
  bool lowCoal = !isnan(coalLevel) && (coalHoursLeft < lowHours || coalLevel < settings.hopperBuckets * 0.05f);
  bool coalKnown = !isnan(coalLevel) || isnan(coalK) || settings.hopperBuckets <= 0;
  setWarning(WARN_LOW_COAL, lowCoal, coalKnown, 2, 5, heating && settings.coalOn && settings.lowCoalOn, upTo320(coalLevel),
             tick);

  // Stalled after a power cut: it burned when the power came back and keeps cooling - many furnace controllers
  // need a hand after a cut. Clears once the furnace warms up again; the watch ends after STALL_WATCH_MS.
  if (stallWatch && millis() >= STALL_WATCH_MS && !warnings[WARN_STALLED].active) stallWatch = false;
  bool stalled;
  if (warnings[WARN_STALLED].active) stalled = !(slope >= 2) && !(f.lastGood >= stallFrom - 1);
  else stalled = stallWatch && millis() < STALL_WATCH_MS && f.lastGood <= stallFrom - STALL_DROP && slope <= STALL_SLOPE;
  setWarning(WARN_STALLED, stalled, !isnan(slope) || !stallWatch, 3, 10, heating && settings.stalledOn, f.lastGood, tick);

  // Ash pan: due after ashLimit buckets of coal (once the coal use is learnt) or ashDays since it was emptied.
  // Unknown without a clock (right after a power cut), so it does not close and reopen.
  time_t now = time(nullptr);
  bool ashDue = ashAt && clockSet(now) && ((now - ashAt) / 86400.0f >= settings.ashDays || ashBuckets >= settings.ashLimit);
  setWarning(WARN_ASH, ashDue, clockSet(now), 1, 1, heating && settings.coalOn && settings.ashOn, upTo320(ashBuckets), tick);

  // Hot water ready: not a problem, just a contact for a push when the boiler is warm enough.
  const SensorState& b = state[BOILER];
  if (!b.faulty && !isnan(b.lastGood)) {
    bool ready = b.lastGood >= settings.hotWater - (hotWaterReady ? HOT_WATER_HYSTERESIS : 0);
    if (ready != hotWaterReady) {
      hotWaterReady = ready;
      hotWaterContact->setVal(ready ? 1 : 0);
      saveAlerts();
    }
  }
}

// ------------------------------------------------------------------ coal

// Heat the furnace gives off in one minute, in degrees: furnace minus return water (0 without both readings).
// Coal burned is taken as proportional to its sum, the degree-minutes; the factor coalK is learnt from refills
// to the top - between two of them the hopper burned exactly what was put in. The power cuts in the minute log
// cost nothing, rightly: the feeder stands still without power.
float heatOf(const MinuteRecord& r) {
  int16_t f = r.v[FURNACE], w = r.v[RETURN_WATER];
  return f == NO_VALUE || w == NO_VALUE || f <= w ? 0 : (f - w) / 100.0f;
}

// Degree-minutes in [from, to) as far as the minute log goes; `covered` gets the minutes with both readings.
float heatBetween(uint32_t from, uint32_t to, uint32_t* covered = nullptr) {
  float heat = 0;
  uint32_t n = 0;
  if (to > from) minutes.forEach(from / 60, (to - 1) / 60, [&](uint32_t, const MinuteRecord& r) {
    if (r.v[FURNACE] != NO_VALUE && r.v[RETURN_WATER] != NO_VALUE) n++;
    heat += heatOf(r);
  });
  if (covered) *covered = n;
  return heat;
}

// Degree-minutes from `from` to the newest minute: the minute log where it reaches (from logStart), the daily
// summaries before that - a share of a day by the hours it overlaps. A hopper last filled more than 31 days ago
// still has a level.
float heatSince(uint32_t from, uint32_t logStart) {
  float heat = 0;
  if (from < logStart) {
    daily.forEach(0, [&](const DayRecord& d) {
      uint32_t dayStart = d.time, dayEnd = nextMidnight(dayStart);   // copies: a packed field binds to no reference
      uint32_t a = max(dayStart, from), b = min(dayEnd, logStart);
      if (b > a) heat += d.heat * (float)(b - a) / (dayEnd - dayStart);
    });
    from = logStart;
  }
  if (minutes.newest()) heat += heatBetween(from, (minutes.newest() + 1) * 60);
  return heat;
}

// All refills, oldest first - one entered later for an earlier hour lands in its place.
std::vector<RefillRecord> sortedRefills() {
  std::vector<RefillRecord> v;
  refills.forEach(0, [&](const RefillRecord& r) { v.push_back(r); });
  std::stable_sort(v.begin(), v.end(), [](const RefillRecord& a, const RefillRecord& b) { return a.time < b.time; });
  return v;
}

// Learns coalK from the latest COAL_INTERVALS intervals between refills to the top that the minute log still
// covers (burned = everything put in after the first, the closing refill included), and finds where the
// hopper stands since the last refill to the top. Reads up to the whole minute log - at boot and per refill.
// A learnt coalK outlives its intervals (summer, a long gap) in flash; without two refills to the top it goes.
void recomputeCoal() {
  uint32_t logStart = minutes.newest() ? minutes.oldest() * 60 : UINT32_MAX;
  float put[COAL_INTERVALS], heat[COAL_INTERVALS];
  uint16_t found = 0, fulls = 0;
  uint32_t prevFull = 0;
  float added = 0;
  lastRefill = {};
  for (const RefillRecord& r : sortedRefills()) {
    lastRefill = r;
    added += r.amount10 / 10.0f;
    if (!r.full) continue;
    fulls++;
    if (prevFull >= logStart) {
      float h = heatBetween(prevFull, r.time);
      if (h >= COAL_MIN_HEAT) {
        put[found % COAL_INTERVALS] = added;
        heat[found % COAL_INTERVALS] = h;
        found++;
      }
    }
    prevFull = r.time;
    added = 0;
  }
  if (found) {
    float sumPut = 0, sumHeat = 0;
    for (uint16_t k = 0; k < min(found, (uint16_t)COAL_INTERVALS); k++) { sumPut += put[k]; sumHeat += heat[k]; }
    coalK = sumPut / sumHeat;
    coalSamples = found;
    prefs.putFloat("coalK", coalK);
    prefs.putUShort("coalN", coalSamples);
  } else if (fulls < 2) {
    coalK = NAN;
    coalSamples = 0;
    prefs.remove("coalK");
    prefs.remove("coalN");
  }
  lastFullAt = prevFull;
  addedSinceFull = added;
  heatSinceFull = prevFull ? heatSince(prevFull, logStart) : NAN;
  heatSinceAsh = ashAt ? heatSince(ashAt, logStart) : 0;
}

// Hopper level, burning pace (last 24 h of readings) and time left - every 10 minutes, after a refill and after
// a settings change.
void updateCoal(uint32_t now) {
  coalRate = coalLevel = coalHoursLeft = ashBuckets = NAN;
  if (!settings.coalOn || isnan(coalK)) return;
  if (!isnan(heatSinceAsh)) ashBuckets = coalK * heatSinceAsh;
  uint32_t covered;
  float heat = heatBetween(now - 86400, now, &covered);
  if (covered >= 60) coalRate = coalK * heat / (covered / 60.0f);
  if (settings.hopperBuckets <= 0 || !lastFullAt || isnan(heatSinceFull)) return;
  coalLevel = constrain(settings.hopperBuckets + addedSinceFull - coalK * heatSinceFull, 0.0f, settings.hopperBuckets);
  if (!isnan(coalRate)) coalHoursLeft = coalRate > 0.01f ? coalLevel / coalRate : INFINITY;
}

// Switches the heating season (from the web app, or on by itself when the furnace burns in summer mode): the
// furnace and return alarms are judged again under the new limits, and the journal gets the change.
void setHeating(bool on, bool automatic) {
  if (settings.heating == on) return;
  settings.heating = on;
  prefs.putBool("heating", on);
  logEvent(EV_SEASON, automatic, on);
  Serial.printf("Heating season %s%s\n", on ? "on" : "off (summer mode)", automatic ? " - furnace burning" : "");
  rejudgeAlarm(FURNACE);
  rejudgeAlarm(RETURN_WATER);
  evaluateWarnings();
}

// One reading cycle: probes -> HomeKit -> minute log -> trends -> warnings.
void readAll() {
  float values[SENSOR_COUNT];
  MinuteRecord rec;
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    float t = readSensor(i);
    if (!isnan(t)) t += settings.offset[i];          // calibration: everything downstream sees the corrected value
    bool ok = !isnan(t) && registerReading(i, t);
    if (isnan(t)) registerBadReading(i);
    values[i] = ok ? t : NAN;
    rec.v[i] = toHundredths(values[i]);
    reportSensor(i);
  }
  Serial.printf("Readings: %.2f / %.2f / %.2f / %.2f C\n", values[0], values[1], values[2], values[3]);

  time_t now = time(nullptr);
  if (clockSet(now)) {
    minutes.add(now / 60, rec);
    if (!isnan(heatSinceFull)) heatSinceFull += heatOf(rec);
    if (!isnan(heatSinceAsh)) heatSinceAsh += heatOf(rec);
  }

  float furnace = values[FURNACE];
  // The first reading after a power cut: was the furnace burning? Then watch whether it comes back (WARN_STALLED).
  if (stallWatch && isnan(stallFrom) && !isnan(furnace)) {
    stallFrom = furnace;
    if (furnace < burnThreshold()) stallWatch = false;
  }
  // Summer mode ends by itself once the furnace burns for SEASON_AUTO_MIN in a row - more alarms, never fewer.
  burningMinutes = !isnan(furnace) && furnace >= burnThreshold() ? min(burningMinutes + 1, 60000) : 0;
  if (!settings.heating && burningMinutes >= SEASON_AUTO_MIN) setHeating(true, true);
  pushRecent(values);
  evaluateWarnings(true);
}

// ------------------------------------------------------------------ history

uint32_t localMidnight(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
  lt.tm_isdst = -1;                       // midnight may be on the other side of a clock change than `t`
  return mktime(&lt);
}

uint32_t nextMidnight(uint32_t midnight) {
  return localMidnight(midnight + 36 * 3600);   // 23 or 25 hour days included
}

// Daily min / avg / max, the furnace's burning minutes and its heat output, from the minute log (survives reboots).
void summarizeDay(uint32_t dayStart, uint32_t dayEnd) {
  float mn[SENSOR_COUNT], mx[SENSOR_COUNT], sum[SENSOR_COUNT] = {};
  uint16_t count[SENSOR_COUNT] = {};
  uint16_t burn = 0;
  float heat = 0;
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
    heat += heatOf(r);
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
  d.heat = lroundf(heat);
  if (any) daily.add(d);
}

// Runs every loop once NTP has set the clock: pending events, Eve entries, daily summaries.
void logHistory() {
  time_t now = time(nullptr);
  if (!clockSet(now)) return;
  if (powerLostAt) logPowerCut(now);
  if (pendingEventCount) flushPendingEvents(now);
  if (!ashAt) {                                   // the ash pan counts from the first time the device has a clock
    ashAt = now;
    prefs.putUInt("ashAt", ashAt);
    heatSinceAsh = 0;
  }
  if (now - lastAliveMark >= ALIVE_INTERVAL_S) {
    alive.mark(now);
    lastAliveMark = now;
  }

  uint32_t slot = now / HISTORY_INTERVAL_S;
  if (slot != lastHistorySlot) {
    lastHistorySlot = slot;
    updateCoal(now);
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
    // Skipped after crash restarts in a row - this is the first heavy work after boot.
    uint32_t last = daily.newestTime();
    uint32_t d = last ? nextMidnight(last) : localMidnight((time_t)minutes.oldest() * 60);
    for (uint8_t guard = 0; minutes.newest() && d < today && guard < 32 && !updater.crashLoop(); guard++, d = nextMidnight(d)) {
      homeSpan.resetWatchdog();
      summarizeDay(d, nextMidnight(d));
    }
  } else {
    summarizeDay(currentDay, today);
  }
  currentDay = today;
}

// wipe: the first start after a full reset - everything goes.
void setupHistory(bool wipe) {
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed - no history");
  // Files of an older layout would eat the space of the new ones: a layout change wipes the history once.
  bool newLayout = prefs.getUChar("fsLayout", 0) != FS_LAYOUT;
  if (newLayout || wipe) {
    Serial.println(wipe ? "Full reset - wiping the history" : "History layout changed - formatting LittleFS");
    LittleFS.format();
    prefs.putUChar("fsLayout", FS_LAYOUT);
  }
  if (!minutes.begin()) Serial.println("No \"minutes\" partition - per-minute history disabled");
  if (wipe) minutes.clear();
  if (!alive.begin()) Serial.println("No \"alive\" partition - power cuts timed from the minute log");
  if (newLayout || wipe) alive.clear();                       // old LittleFS blocks / the last home's marks
  daily.begin();
  events.begin();
  refills.begin();
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
  settings.coalOn       = prefs.getBool("coalOn", d.coalOn);
  settings.hopperBuckets = prefs.getFloat("hopBk", d.hopperBuckets);
  settings.bucketKg     = prefs.getFloat("bucketKg", d.bucketKg);
  settings.coalPrice    = prefs.getFloat("coalPr", d.coalPrice);
  settings.lowCoalOn    = prefs.getBool("lcOn", d.lowCoalOn);
  settings.lowCoalHours = prefs.getFloat("lcHrs", d.lowCoalHours);
  settings.heating      = prefs.getBool("heating", d.heating);
  settings.stalledOn    = prefs.getBool("stallOn", d.stalledOn);
  settings.ashOn        = prefs.getBool("ashOn", d.ashOn);
  settings.ashLimit     = prefs.getFloat("ashLim", d.ashLimit);
  settings.ashDays      = prefs.getFloat("ashDays", d.ashDays);
  for (size_t i = 0; i < SENSOR_COUNT; i++) settings.offset[i] = prefs.getFloat(prefKey("off", i).c_str(), 0);
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
  prefs.putBool("coalOn", settings.coalOn);
  prefs.putFloat("hopBk", settings.hopperBuckets);
  prefs.putFloat("bucketKg", settings.bucketKg);
  prefs.putFloat("coalPr", settings.coalPrice);
  prefs.putBool("lcOn", settings.lowCoalOn);
  prefs.putFloat("lcHrs", settings.lowCoalHours);
  prefs.putBool("heating", settings.heating);
  prefs.putBool("stallOn", settings.stalledOn);
  prefs.putBool("ashOn", settings.ashOn);
  prefs.putFloat("ashLim", settings.ashLimit);
  prefs.putFloat("ashDays", settings.ashDays);
  for (size_t i = 0; i < SENSOR_COUNT; i++) prefs.putFloat(prefKey("off", i).c_str(), settings.offset[i]);
  prefs.putFloat("lat", settings.lat);
  prefs.putFloat("lon", settings.lon);
  prefs.putString("place", settings.place);
}

float parseNumber(String v) {
  v.trim();
  v.replace(',', '.');
  return v.length() ? v.toFloat() : NAN;
}

// A limit from the web app: empty = off (NAN). False for anything a probe cannot show - a typo must never switch
// an alarm off.
bool parseLimit(String v, float& out) {
  v.trim();
  if (!v.length()) {
    out = NAN;
    return true;
  }
  float f = parseNumber(v);
  if (isnan(f) || f < -55 || f > 125) return false;
  out = f;
  return true;
}

// ------------------------------------------------------------------ web app + API

String jsonNumber(float v, unsigned decimals = 2) {
  return isnan(v) ? String("null") : String(v, decimals);
}

// The page changes only with the firmware: a phone that already has it asks with its ETag and gets
// "304 Not Modified" instead of the 34 KB again.
void handleApp() {
  server.sendHeader("ETag", WEBAPP_ETAG);
  server.sendHeader("Cache-Control", "no-cache");
  if (server.header("If-None-Match") == WEBAPP_ETAG) {
    server.send(304);
    return;
  }
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8", (const char*)WEBAPP_HTML_GZ, WEBAPP_HTML_GZ_LEN);
}

void handleIcon() {
  server.sendHeader("Cache-Control", "max-age=604800");
  server.send_P(200, "image/png", (const char*)WEBAPP_ICON_PNG, WEBAPP_ICON_PNG_LEN);
}

void handleManifest() {
  server.send_P(200, "application/manifest+json", WEBAPP_MANIFEST);
}

// Current readings, trends, limits, warnings, settings and device health. GET t = the phone's Unix time: used
// only while NTP has not set the clock (WiFi up but no internet) - so the history keeps recording.
void handleNow() {
  if (!clockSet(time(nullptr)) && clockSet(argOr("t", 0))) {
    timeval tv = {(time_t)argOr("t", 0), 0};
    settimeofday(&tv, nullptr);
    Serial.println("Clock set from the web app - no NTP");
  }
  String j;
  j.reserve(2600);
  j += "{\"time\":";     j += (uint32_t)time(nullptr);
  j += ",\"uptime\":";   j += (uint32_t)(millis() / 1000);
  j += ",\"rssi\":";     j += (int)WiFi.RSSI();
  j += ",\"ip\":\"";     j += WiFi.localIP().toString();
  j += "\",\"heap\":";   j += (uint32_t)ESP.getFreeHeap();
  j += ",\"fsUsed\":";   j += (uint32_t)LittleFS.usedBytes();
  j += ",\"fsTotal\":";  j += (uint32_t)LittleFS.totalBytes();
  j += ",\"oldest\":";   j += minutes.oldest() * 60;
  j += ",\"fw\":";       j += FW_VERSION;
  j += ",\"ota\":";      j += otaHashStored().length() ? "true" : "false";
  j += ",\"update\":\""; j += updater.status();
  RemoteUpdate::Progress up = updater.progress();
  j += "\",\"upd\":{\"phase\":"; j += (int)up.phase;
  j += ",\"to\":";      j += up.version;
  j += ",\"done\":";    j += up.done;
  j += ",\"size\":";    j += up.size;
  j += "},\"avail\":";  j += updater.available();
  j += ",\"forced\":";    j += updater.forced() ? "true" : "false";
  j += ",\"sensors\":[";
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
    j += ",\"offset\":";  j += jsonNumber(settings.offset[i], 1);
    j += '}';
  }
  j += "],\"warnings\":[";
  for (uint8_t w = 0; w < WARN_COUNT; w++) {
    if (w) j += ',';
    j += warnings[w].active ? "true" : "false";
  }
  j += "],\"forecast\":"; j += goOutForecast < 0 ? String("null") : String(goOutForecast);
  j += ",\"hotWater\":";  j += hotWaterReady ? "true" : "false";
  j += ",\"stallFrom\":"; j += jsonNumber(stallFrom, 1);
  j += ",\"ash\":{\"at\":"; j += ashAt;
  j += ",\"buckets\":";   j += jsonNumber(ashBuckets, 1);
  j += '}';
  j += ",\"coal\":{\"k\":"; j += jsonNumber(coalK, 7);
  j += ",\"samples\":";   j += coalSamples;
  j += ",\"lastFull\":";  j += lastFullAt;
  j += ",\"level\":";     j += jsonNumber(coalLevel, 1);
  j += ",\"rate\":";      j += jsonNumber(coalRate, 2);
  j += ",\"hoursLeft\":"; j += isinf(coalHoursLeft) ? String("1e9") : jsonNumber(coalHoursLeft, 1);
  j += ",\"lastRefill\":";
  if (lastRefill.time) {
    j += '[';  j += lastRefill.time;  j += ',';  j += lastRefill.amount10;  j += ',';  j += (int)lastRefill.full;  j += ']';
  } else {
    j += "null";
  }
  j += "},\"settings\":{\"goOutOn\":"; j += settings.goOutOn ? "true" : "false";
  j += ",\"goOutRate\":";    j += jsonNumber(settings.goOutRate, 1);
  j += ",\"goOutHorizon\":"; j += settings.goOutHorizon;
  j += ",\"freezeOn\":";     j += settings.freezeOn ? "true" : "false";
  j += ",\"freezeTemp\":";   j += jsonNumber(settings.freezeTemp, 1);
  j += ",\"burnbackOn\":";   j += settings.burnbackOn ? "true" : "false";
  j += ",\"burnbackRate\":"; j += jsonNumber(settings.burnbackRate, 1);
  j += ",\"hotWater\":";     j += jsonNumber(settings.hotWater, 1);
  j += ",\"coalOn\":";       j += settings.coalOn ? "true" : "false";
  j += ",\"hopperBuckets\":"; j += jsonNumber(settings.hopperBuckets, 1);
  j += ",\"bucketKg\":";     j += jsonNumber(settings.bucketKg, 1);
  j += ",\"coalPrice\":";    j += jsonNumber(settings.coalPrice, 0);
  j += ",\"lowCoalOn\":";    j += settings.lowCoalOn ? "true" : "false";
  j += ",\"lowCoalHours\":"; j += jsonNumber(settings.lowCoalHours, 0);
  j += ",\"heating\":";      j += settings.heating ? "true" : "false";
  j += ",\"stalledOn\":";    j += settings.stalledOn ? "true" : "false";
  j += ",\"ashOn\":";        j += settings.ashOn ? "true" : "false";
  j += ",\"ashLimit\":";     j += jsonNumber(settings.ashLimit, 0);
  j += ",\"ashDays\":";      j += jsonNumber(settings.ashDays, 0);
  j += ",\"lat\":";          j += jsonNumber(settings.lat, 4);
  j += ",\"lon\":";          j += jsonNumber(settings.lon, 4);
  j += ",\"place\":\"";      j += settings.place;
  j += "\"}}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", j);
}

// Chunked binary responses: collects small writes into one buffer.
struct ChunkWriter {
  uint8_t buf[1436];    // one full TCP segment per write
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
    homeSpan.resetWatchdog();     // a long answer (the backup) holds the loop for seconds
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
  float low, high;
  if (!parseLimit(server.arg("low"), low) || !parseLimit(server.arg("high"), high) || low >= high) {
    server.send(400, "text/plain", "out of range");
    return;
  }
  s.low  = low;
  s.high = high;
  prefs.putFloat(prefKey("lo", i).c_str(), s.low);
  prefs.putFloat(prefKey("hi", i).c_str(), s.high);
  rejudgeAlarm(i);
  handleNow();
}

// POST any of the Settings fields (booleans as 1/0). Answers with the same JSON as /api/now, or 400 "out of range:
// <field>" with nothing changed when a number is outside what the field allows.
void handleSettings() {
  String bad;
  auto flag = [](const char* k, bool& v) { if (server.hasArg(k)) v = server.arg(k) == "1"; };
  auto number = [&bad](const char* k, float& v, float lo, float hi) {
    if (!server.hasArg(k)) return;
    float f = parseNumber(server.arg(k));
    if (!isnan(f) && f >= lo && f <= hi) v = f;
    else bad = k;
  };
  flag("goOutOn", settings.goOutOn);
  number("goOutRate", settings.goOutRate, 1, 60);
  float horizon = settings.goOutHorizon;
  number("goOutHorizon", horizon, 5, 240);
  settings.goOutHorizon = lroundf(horizon);
  flag("freezeOn", settings.freezeOn);
  number("freezeTemp", settings.freezeTemp, -20, 20);
  flag("burnbackOn", settings.burnbackOn);
  number("burnbackRate", settings.burnbackRate, 1, 60);
  number("hotWater", settings.hotWater, 20, 90);
  flag("coalOn", settings.coalOn);
  number("hopperBuckets", settings.hopperBuckets, 0, 500);
  number("bucketKg", settings.bucketKg, 1, 50);
  number("coalPrice", settings.coalPrice, 0, 20000);
  flag("lowCoalOn", settings.lowCoalOn);
  number("lowCoalHours", settings.lowCoalHours, 1, 96);
  flag("stalledOn", settings.stalledOn);
  flag("ashOn", settings.ashOn);
  number("ashLimit", settings.ashLimit, 1, 200);
  number("ashDays", settings.ashDays, 1, 60);
  float oldOffset[SENSOR_COUNT];
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    oldOffset[i] = settings.offset[i];
    number(prefKey("off", i).c_str(), settings.offset[i], -10, 10);
  }
  if (server.hasArg("lat") && server.hasArg("lon")) {
    float lat = parseNumber(server.arg("lat")), lon = parseNumber(server.arg("lon"));   // both empty = no location
    if (isnan(lat) != isnan(lon) || fabsf(lat) > 90 || fabsf(lon) > 180) bad = "lat";
    settings.lat = lat;
    settings.lon = lon;
    String place = server.arg("place");
    place.replace("\"", "");
    place.replace("\\", "");
    strlcpy(settings.place, place.c_str(), sizeof(settings.place));
  }
  if (bad.length()) {
    loadSettings();                                        // back to what is in flash - nothing half-applied
    server.send(400, "text/plain", "out of range: " + bad);
    return;
  }
  if (server.hasArg("heating")) setHeating(server.arg("heating") == "1", false);
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    if (settings.offset[i] != oldOffset[i]) shiftProbe(i, settings.offset[i] - oldOffset[i]);
  saveSettings();
  if (clockSet(time(nullptr))) updateCoal(time(nullptr));
  evaluateWarnings();
  handleNow();
}

// GET. JSON [[time, kg x 10, full], ...] in the order they were entered (the last one can be taken back).
void handleRefills() {
  ChunkWriter w;
  w.begin("application/json");
  w.put("[", 1);
  bool first = true;
  refills.forEach(0, [&](const RefillRecord& r) {
    char line[40];
    int n = snprintf(line, sizeof(line), "%s[%u,%u,%u]", first ? "" : ",", (unsigned)r.time, r.amount10, r.full);
    w.put(line, n);
    first = false;
  });
  w.put("]", 1);
  w.end();
}

// The coal model and the hopper recounted after the refills changed.
void recountCoal() {
  recomputeCoal();
  updateCoal(time(nullptr));
  evaluateWarnings();
}

void coalChanged() {
  recountCoal();
  handleNow();
}

void addRefill(float buckets, bool full, uint32_t t) {
  RefillRecord r = {t, (uint16_t)lroundf(buckets * 10), (uint8_t)full, 0};
  refills.add(r);
  recountCoal();
}

// POST buckets, full (1/0), time (Unix; optional - a refill entered later, up to 7 days back). Answers /api/now.
void handleRefill() {
  uint32_t now = time(nullptr), t = argOr("time", now);
  float buckets = parseNumber(server.arg("buckets"));
  if (!clockSet(now) || isnan(buckets) || buckets < 0.1f || buckets > 500 || t > now + 300 || t + 7 * 86400 < now) {
    server.send(400, "text/plain", "bad refill");
    return;
  }
  addRefill(buckets, server.arg("full") == "1", min(t, now));
  handleNow();
}

// POST time: takes back the refill entered last - only if it is the one with that time (a second tap must not
// remove an older one). Answers /api/now, or 409.
void handleRefillUndo() {
  if (!refills.removeLast(argOr("time", 0))) {
    server.send(409, "text/plain", "not the last refill");
    return;
  }
  coalChanged();
}

// POST: the ash pan was emptied - its reminder starts counting again. Answers /api/now.
void handleAsh() {
  time_t now = time(nullptr);
  if (!clockSet(now)) {
    server.send(503, "text/plain", "no clock yet");
    return;
  }
  emptyAsh(now);
  handleNow();
}

// The ash pan was emptied: its reminder starts counting again.
void emptyAsh(uint32_t now) {
  ashAt = now;
  prefs.putUInt("ashAt", ashAt);
  heatSinceAsh = 0;
  logEvent(EV_ASH, 0, 0);
  updateCoal(now);
  evaluateWarnings();
}

void handleBus() {
  server.send(200, "text/plain", busReport());
}

// The remote-access key (Relay.h): made once, renewed by "new key" in the web app and by the BOOT full reset.
void relaySecret(uint8_t secret[32], bool renew) {
  if (renew || prefs.getBytes("relayKey", secret, 32) != 32) {
    esp_fill_random(secret, 32);
    prefs.putBytes("relayKey", secret, 32);
  }
}

// Requests from the relay come from 127.0.0.1 (Relay.h replays them to this server).
bool viaRelay() {
  return server.client().remoteIP() == IPAddress(127, 0, 0, 1);
}

// GET: remote access - relay host, connection state, the pairing code. POST (home WiFi only - a phone outside would
// cut itself off): host = the relay ("" = off), renew = 1 a new key (every paired phone must pair again).
void handleRemote() {
  if (server.method() == HTTP_POST) {
    if (viaRelay()) {
      server.send(403, "text/plain", "only from the home WiFi");
      return;
    }
    String host = relay.host();
    bool renew = server.arg("renew") == "1";
    if (server.hasArg("host")) {
      host = server.arg("host");
      host.trim();
      host.toLowerCase();
      bool bad = host.length() > 63;
      for (char c : host) bad |= !isalnum((unsigned char)c) && c != '.' && c != '-';
      if (bad) {
        server.send(400, "text/plain", "bad host");
        return;
      }
      prefs.putString("relayHost", host);
    }
    uint8_t secret[32];
    relaySecret(secret, renew);
    relay.configure(host, secret);
  }
  String j = "{\"host\":\"" + relay.host() + "\",\"state\":\"" + relay.state() + "\",\"since\":" + relay.connectedAt() +
             ",\"served\":" + relay.served() + ",\"via\":" + (viaRelay() ? "true" : "false") + ",\"code\":\"" +
             relay.pairingCode() + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", j);
}

// GET: the whole device in one file (Backup.h). Home WiFi only - it holds the WiFi password and the pairing keys.
void handleBackup() {
  if (viaRelay()) {
    server.send(403, "text/plain", "only from the home WiFi");
    return;
  }
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  char disposition[64];
  strftime(disposition, sizeof(disposition), "attachment; filename=\"piec-kopia-%Y-%m-%d.bin\"", &lt);
  server.sendHeader("Content-Disposition", disposition);
  ChunkWriter w;
  w.begin("application/octet-stream");
  backup.write(w, FW_VERSION);
  w.end();
}

// POST /api/restore with a backup as a multipart file: everything here is replaced, then the ESP restarts. The
// whole upload is one handleClient() call, so the loop's own writing waits meanwhile.
String restoreError;
bool   restoreStarted = false;

void handleRestoreChunk() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    restoreError = viaRelay() ? "only from the home WiFi" : updater.busy() ? "an update is being downloaded" : "";
    restoreStarted = restoreError.isEmpty();
    if (restoreStarted) {
      Serial.println("Restoring a backup");
      backup.begin();
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    homeSpan.resetWatchdog();
    if (restoreStarted && restoreError.isEmpty()) backup.feed(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (restoreStarted && restoreError.isEmpty()) restoreError = backup.end();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    restoreError = "upload aborted";
  }
}

void handleRestoreDone() {
  if (!restoreStarted || restoreError.length()) {
    server.send(400, "text/plain", restoreError.length() ? restoreError : String("no file"));
    if (!restoreStarted || !backup.changed()) return;        // nothing written - carry on as before
  } else {
    server.send(200, "text/plain", "restored, restarting");
  }
  Serial.println("Backup restored - restarting");
  delay(500);
  ESP.restart();
}

// POST: look for a new firmware now instead of waiting for the hourly check; install = the version the user
// accepted in the web app - it is downloaded and installed (progress in /api/now "upd").
void handleUpdate() {
  if (server.hasArg("install")) updater.requestInstall(argOr("install", 0));
  else updater.requestCheck();
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

// The network-OTA password: HomeSpan keeps its hash in NVS "OTA"/"OTADATA" (serial command "O") and its factory
// reset erases it - after which HomeSpan would accept its public default. The hash is mirrored in the sketch's own
// namespace, which the factory reset keeps, and put back before HomeSpan starts.
String otaHashStored() {
  char stored[65] = "";
  size_t len = sizeof(stored);
  nvs_handle_t nvs;
  if (nvs_open("OTA", NVS_READONLY, &nvs) != ESP_OK) return "";
  if (nvs_get_str(nvs, "OTADATA", stored, &len) != ESP_OK) stored[0] = 0;
  nvs_close(nvs);
  return stored;
}

void keepOtaPassword() {
  String hash = otaHashStored();
  if (hash.length()) {
    if (prefs.getString("otaHash", "") != hash) prefs.putString("otaHash", hash);
    return;
  }
  hash = prefs.getString("otaHash", "");
  nvs_handle_t nvs;
  if (!hash.length() || nvs_open("OTA", NVS_READWRITE, &nvs) != ESP_OK) return;
  nvs_set_str(nvs, "OTADATA", hash.c_str());
  nvs_commit(nvs);
  nvs_close(nvs);
  Serial.println("OTA password restored after a factory reset");
}

// The web app's "install from file" uses the network-OTA password; without one set, nothing is accepted.
bool otaPasswordMatches(const String& password) {
  String stored = otaHashStored();
  if (!stored.length()) return false;
  uint8_t digest[32];
  mbedtls_sha256((const uint8_t*)password.c_str(), password.length(), digest, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
  return stored.equalsIgnoreCase(hex);
}

String firmwareUploadError;

// POST /api/firmware?pwd=... with the .bin as a multipart file. Goes to the spare app slot and boots on
// probation like a remote update (RemoteUpdate.h), so a bad file rolls back by itself.
void handleFirmwareChunk() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    firmwareUploadError = otaPasswordMatches(server.arg("pwd")) ? "" : "wrong password";
    if (firmwareUploadError.isEmpty() && updater.busy()) firmwareUploadError = "an update is being downloaded";
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
  // Mains-powered: WiFi modem sleep saves nothing here and makes every reply wait for the router's next beacon
  // (measured: the 34 KB page took 1.5 s, connects up to 1 s).
  WiFi.setSleep(false);
  updater.networkUp();
  if (webStarted) return;
  // From the first connection on, a loop stuck for WATCHDOG_S restarts the ESP. Not earlier: the setup hotspot
  // (no WiFi stored) legitimately blocks for its whole timeout.
  homeSpan.enableWatchdog(WATCHDOG_S);
  static const char* headers[] = {"If-None-Match"};
  server.collectHeaders(headers, 1);
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
  server.on("/api/refills", HTTP_GET, handleRefills);
  server.on("/api/refill", HTTP_POST, handleRefill);
  server.on("/api/refill-undo", HTTP_POST, handleRefillUndo);
  server.on("/api/ash", HTTP_POST, handleAsh);
  server.on("/api/update", HTTP_POST, handleUpdate);
  server.on("/api/remote", HTTP_GET, handleRemote);
  server.on("/api/backup", HTTP_GET, handleBackup);
  server.on("/api/restore", HTTP_POST, handleRestoreDone, handleRestoreChunk);
  server.on("/api/remote", HTTP_POST, handleRemote);
  server.on("/api/clear-history", HTTP_POST, handleClearHistory);
  server.on("/api/firmware", HTTP_POST, handleFirmwareDone, handleFirmwareChunk);
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
  server.begin();
  webStarted = true;
}

// ------------------------------------------------------------------ BOOT button

// Hold BOOT and release: after WIFI_RESET_HOLD_MS the WiFi is forgotten; after FULL_RESET_HOLD_MS also the HomeKit
// pairing, and the next start wipes the history. Either way the ESP restarts into the setup hotspot.
// The button has its own task: HomeSpan blocks the loop while it waits for WiFi (~20 s) and for the whole setup
// hotspot, and a hold measured in the loop came out short - 15 s at dad's became "forget the WiFi" and the old
// HomeKit pairing stayed. The task only erases NVS (HomeSpan's WIFI, HAP and CHAR, like its factory reset; SRP
// with the setup code and OTA with its password stay) and leaves the history to the next start, before anything
// opens it. It acts on release: restarting while BOOT is held would drop the chip into its flashing mode.
void eraseNamespace(const char* name) {
  nvs_handle_t h;
  if (nvs_open(name, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
}

void buttonTask(void*) {
  uint32_t pressedAt = 0;
  uint8_t level = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      if (!pressedAt) pressedAt = millis();
      uint32_t held = millis() - pressedAt;
      if (level < 2 && held >= FULL_RESET_HOLD_MS) {
        level = 2;
        Serial.println("BOOT held 15 s - release for a FULL reset (WiFi, HomeKit pairing, history)");
      } else if (level < 1 && held >= WIFI_RESET_HOLD_MS) {
        level = 1;
        Serial.println("BOOT held 5 s - release to forget the WiFi, keep holding for a full reset");
      }
      continue;
    }
    pressedAt = 0;
    if (!level) continue;
    eraseNamespace("WIFI");                        // HomeSpan's network: no WiFi -> setup hotspot
    if (level == 2) {
      eraseNamespace("HAP");                       // pairing + accessory ID: Home sees a new accessory
      eraseNamespace("CHAR");
      prefs.putBool("wipe", true);                 // the history goes at the next start (setup)
    }
    WiFi.disconnect(false, true);                  // also the network the WiFi driver kept
    Serial.println(level == 2 ? "FULL reset - restarting" : "WiFi forgotten - restarting");
    delay(300);
    esp_restart();
  }
}

// ------------------------------------------------------------------ setup / loop

void setup() {
  Serial.begin(115200);
  setenv("TZ", TIME_ZONE, 1);     // local days from the first second - after a soft restart the clock is already set
  tzset();
  snprintf(firmwareRevision, sizeof(firmwareRevision), "%u", (unsigned)FW_VERSION);
  updater.begin();

  bus.begin();
  bus.setWaitForConversion(false);
  assignAddresses();
  Serial.print(busReport());
  prefs.begin("furnace", false);
  // After a full reset with the BOOT button the new home starts empty: no history, no old problems, its own coal
  // use and ash count (limits and settings stay).
  bool wipe = prefs.getBool("wipe", false);
  if (wipe) {
    for (const char* key : {"alerts", "coalK", "coalN", "ashAt", "relayKey"}) prefs.remove(key);   // old phones out
    prefs.putBool("cleared", true);
  }
  loadLimits();
  loadSettings();
  restoreAlerts();
  setupHistory(wipe);
  if (wipe) prefs.remove("wipe");
  coalK = prefs.getFloat("coalK", NAN);
  coalSamples = prefs.getUShort("coalN", 0);
  ashAt = prefs.getUInt("ashAt", 0);
  if (!updater.crashLoop()) recomputeCoal();      // after crash restarts: no heavy work until a fix gets in
  keepOtaPassword();
  for (const char* key : {"tgToken", "tgPeople"}) prefs.remove(key);   // Telegram (v13) is gone
  uint8_t secret[32];
  relaySecret(secret, false);
  relay.begin(WEB_PORT, prefs.getString("relayHost", RELAY_HOST), secret);
  updater.onNeedMemory = [](bool on) { relay.pause(on); };
  bootReason = readBootReason();
  // The power went within ALIVE_INTERVAL_S after the last mark (without marks: within the minute after the last
  // stored one). A power cut is journaled as the cut itself, other starts as EV_BOOT.
  if (bootReason == BOOT_POWER_ON || bootReason == BOOT_BROWNOUT)
    powerLostAt = alive.newest() ? alive.newest() + ALIVE_INTERVAL_S / 2 : minutes.newest() ? minutes.newest() * 60 + 30 : 0;
  if (!powerLostAt) logEvent(EV_BOOT, bootReason, FW_VERSION);
  stallWatch = powerLostAt != 0;
  if (prefs.getBool("cleared", false)) {
    logEvent(EV_HISTORY_CLEARED, 0, 0);
    prefs.remove("cleared");
  }

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  xTaskCreatePinnedToCore(buttonTask, "bootButton", 4096, nullptr, 1, nullptr, 0);
  homeSpan.setStatusPin(STATUS_LED_PIN);
  homeSpan.setGetCharacteristicsCallback(EveHistory::onGetCharacteristics);
  homeSpan.setApSSID(AP_SSID);
  homeSpan.setApPassword(AP_PASSWORD);
  homeSpan.setApTimeout(AP_TIMEOUT_S);
  homeSpan.enableAutoStartAP();              // no stored WiFi -> open the setup hotspot
  homeSpan.setHostNameSuffix("");            // host name is exactly DEVICE_NAME
  if (otaHashStored().length()) homeSpan.enableOTA();   // password lives in flash (CLI "O"), not in this public code
  else Serial.println("No OTA password set (serial \"O\") - network OTA and the web upload stay off");
  homeSpan.setConnectionCallback(onNetworkUp);
  homeSpan.begin(Category::Bridges, "Czujniki pieca", DEVICE_NAME);
  createAccessories();

  new SpanUserCommand('a', "- list DS18B20 probes on the bus", [](const char*) { Serial.print(busReport()); });
  new SpanUserCommand('u', "- check for a firmware update now", [](const char*) { updater.requestCheck(); });

  lastRequestMs = millis() - READ_INTERVAL_MS;   // first reading right away
}

void loop() {
  homeSpan.resetWatchdog();
  homeSpan.poll();
  if (webStarted) server.handleClient();
  if (!serialClosed && millis() >= SERIAL_WINDOW_MS) {
    homeSpan.setSerialInputDisable(true);
    serialClosed = true;
  }
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
