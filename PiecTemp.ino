// Furnace temperature monitor
// ESP32 + DS18B20 probes on one 1-Wire bus, exposed straight to Apple Home through HomeSpan (no bridge, no Pi).
//
// Apple Home (names in Polish - the household's language): one temperature sensor per probe, with a fault flag
// when the probe stops answering and an Eve history. Alarms, warnings and "hot water ready" are notifications of
// the app (Relay.h -> relay -> Web Push), not Home accessories - the phone words them (web/sw.js).
// Coal: the web app records refills in kilograms (shovels and buckets are its shortcuts) and how full the hopper was;
// the coal burned is counted in the feeder piston's strokes (Geco bus), each worth the kg learnt from refills "to the
// top" (see recomputeCoal) - daily use, costs, hopper level, time left. The water's heat is not used: far too unsure.
// Pairing: Home app -> Add Accessory -> More options -> "Czujniki pieca", setup code 466-37-726.
// Eve app: every temperature sensor carries an Eve history (EveHistory.h).
//
// Web app http://furnace.local:8080 on the home WiFi: live cards, charts from 6 hours to years, warnings and
// limits, event journal. Source in web/index.html, packed into WebApp.h by web/build.ps1.
// Backup (Backup.h): the whole device in one file - restored on a new board it keeps the history, settings,
// WiFi, HomeKit pairing and remote access (Ustawienia -> Kopia zapasowa).
// Remote access (Relay.h): the same app from GitHub Pages, anywhere - through a Cloudflare relay (relay/), end-to-end
// encrypted with a key paired in the home app (Ustawienia -> Dostep zdalny). Nothing to open on the router.
// History (HistoryLog.h): every minute for 31 days (ESP32-S3: ~90) in its own flash partition; daily summaries for 2.5-5 years,
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
// The blue LED (if the board has one on GPIO2) - on the ESP32-S3 the RGB LED - shows the HomeSpan status.
//
// Updates:
//   - over the internet (any home): bump FW_VERSION and its CHANGES note in web/index.html, run
//     release/publish.ps1; the ESP checks UPDATE_MANIFEST_URL every hour (or "@u" / the web app), the web app
//     offers the new version with its notes and installs it on a tap; it rolls back if it does not come online,
//   - on the same WiFi: web app -> Ustawienia -> Oprogramowanie -> install a .bin file, or Arduino IDE -> Tools ->
//     Port -> "furnace at ..." (both use the OTA password set with the serial command "O"; without a password
//     set both stay off - HomeSpan would otherwise accept its public default).
//
// Two boards, one code (CONFIG_IDF_TARGET_ESP32S3 picks the pins, the LED and the update channel):
//   - ESP32 Dev Module (4 MB): partitions.csv in this folder (2 x 1.69 MB app slots for OTA, 352 KB minute ring,
//     16 KB alive marks, 208 KB LittleFS) - full, kept as it is,
//   - ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM): partitions_s3.csv (2 x 4 MB app slots, 1 MB minute ring, core
//     dump, ~7 MB LittleFS); release/publish.ps1 -Target s3 builds it with PSRAM=opi, FlashSize=16M.
// A partition change needs a USB upload.
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
#include <functional>
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include "TechLog.h"
#include "Backup.h"
#include "EveHistory.h"
#include "HistoryLog.h"
#include "RemoteUpdate.h"
#include "Relay.h"
#include "GecoBus.h"
#include "WebApp.h"

// ------------------------------------------------------------------ configuration

constexpr uint32_t FW_VERSION = 36;                  // bump for every release (release/publish.ps1)
// Each board has its own release channel - a firmware for the other chip must never be offered.
#if CONFIG_IDF_TARGET_ESP32S3
const char* UPDATE_MANIFEST_URL =
  "https://raw.githubusercontent.com/klimabartlomiej/furnace-monitor/main/release/s3/manifest.json";
#else
const char* UPDATE_MANIFEST_URL =
  "https://raw.githubusercontent.com/klimabartlomiej/furnace-monitor/main/release/manifest.json";
#endif

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
constexpr uint32_t HEALTH_CAPACITY    = 120;             // days of health counters (at most; at least half)
constexpr uint32_t GECO_CAPACITY      = 390000;          // controller work changes kept (3 MB: ~2 months of winter
                                                         // burning, ~5 of autumn; at least half)
constexpr uint32_t WORK_CAPACITY      = 5 * 8784;        // hours of the controller's work summary (5 years; at least half)
constexpr uint8_t  COAL_INTERVALS     = 10;              // latest refill-to-refill intervals the kg per stroke averages
constexpr float    COAL_MIN_STROKES   = 120;             // piston strokes an interval needs to teach the kg per stroke
                                                         // (~30 kg: a top-up is no measure, "to the top" varies by kilos)
constexpr float    COAL_MIN_HEARD     = 0.9;             // ... and the share of it the controller has to be heard in
constexpr uint32_t STARVE_WINDOW_S    = 1800;            // coal fed heats the flue to its usual peak within minutes: the
constexpr float    STARVE_SHARE       = 0.7;             // peak is judged per window this long, and a window burning and
                                                         // fed but peaking below this share of the usual got no coal
constexpr uint32_t STARVE_USUAL_S     = 24 * 3600;       // the usual peak: the median of the judged windows this far back
constexpr float    STARVE_BURN_S      = 120;             // a window is judged with this much burning in it (holding's
                                                         // short feeds hardly heat the flue)
constexpr uint32_t STARVE_RELIT_S     = 90 * 60;         // ... and not this soon after the controller came back from
                                                         // standby or silence (a fire from cold heats slowly)
constexpr uint32_t STARVE_LOOKBACK_S  = 24 * 3600;       // strokes after the hopper ran empty: looked for this far back
constexpr uint32_t FLUE_MARK_S        = 240;             // the flue is compared this long into a burn: every burn gets
                                                         // this far, and it hardly rises any more (+-16 between burns)
constexpr uint32_t WORK_DELAY_S       = 360;             // an hour is stored this long after its end - the flue mark of
                                                         // a burn begun in its last minute is on the minute log by then
constexpr float    HOT_WATER_HYSTERESIS = 3;             // degrees below "ready" before the hot-water contact closes
constexpr float    BATH_DROP          = 2;               // boiler degrees lost within BATH_WINDOW = hot water drawn
constexpr uint8_t  BATH_WINDOW        = 10;              // minutes (standing, the boiler loses well under 1 degree/h)
constexpr uint8_t  BATH_MIN_MINUTES   = 5;               // a shorter fall is a sink or a quick rinse, not a bath
constexpr uint16_t SEASON_AUTO_MIN    = 6 * 60;          // minutes of burning that end summer mode by themselves -
                                                         // longer than firing it up for hot water in summer
constexpr uint32_t STALL_WATCH_MS     = 3 * 3600 * 1000; // after a power cut, how long a cooling furnace is watched
constexpr float    STALL_DROP         = 10;              // ... it counts as stalled once this much cooler (an idling
constexpr float    STALL_SLOPE        = -4;              // ... controller drops a few degrees) and still falling this
                                                         // fast (degrees/hour)
constexpr uint8_t  FS_LAYOUT          = 6;               // bump when the history files change shape (wipes them once)
constexpr uint32_t ALIVE_INTERVAL_S   = 2;               // "still alive" mark - how exactly a power cut is timed
constexpr uint8_t  TREND_MINUTES      = 30;              // window of the trend the warnings look at
constexpr uint8_t  MORNING_HOUR       = 7;               // the summary of the day before goes out from this hour
constexpr uint8_t  BOOT_BUTTON_PIN    = 0;               // the BOOT button on the board
constexpr uint16_t WEB_PORT           = 8080;            // port 80 belongs to HomeKit (HomeSpan)
#if CONFIG_IDF_TARGET_ESP32S3
constexpr uint8_t  STATUS_PIXEL_PIN   = PIN_RGB_LED;     // the RGB LED of the ESP32-S3-DevKitC-1 (GPIO48)
constexpr uint8_t  ONE_WIRE_PIN       = 4;               // GPIO4 (next to 3V3 on the header) - the DQ (data) wire,
                                                         // 4.7k pull-up to 3.3V
constexpr int8_t   TC_CS_PIN          = 10;              // MAX6675 thermocouple board (the flue): CS, SCK and SO -
constexpr uint8_t  TC_SCK_PIN         = 12;              // the S3's default SPI pins, driven by hand (16 bits a minute)
constexpr uint8_t  TC_SO_PIN          = 13;
constexpr int8_t   GECO_RX_PIN        = 18;              // TXD of the RS485 module on the Geco controller's panel bus -
                                                         // receive only (GecoBus.h)
// Free header pins where the bus is looked for while GPIO18 hears nothing (RO on a neighbour): no strapping pin
// (0, 3, 45, 46), no flash/PSRAM (26-37), no USB (19, 20), no console (43, 44), none of the pins above.
constexpr int8_t   GECO_SCAN_PINS[]   = {1, 2, 5, 6, 7, 8, 9, 11, 14, 15, 16, 17, 21, 38, 39, 40, 41, 42, 47};
#else
constexpr uint8_t  STATUS_LED_PIN     = 2;               // blue on-board LED on most ESP32 dev boards
constexpr uint8_t  ONE_WIRE_PIN       = 22;              // D22 - GPIO of the DQ (data) wire, 4.7k pull-up to 3.3V
constexpr int8_t   TC_CS_PIN          = -1;              // no thermocouple on the classic board (no flash left)
constexpr uint8_t  TC_SCK_PIN         = 0;
constexpr uint8_t  TC_SO_PIN          = 0;
constexpr int8_t   GECO_RX_PIN        = -1;              // no controller bus on the classic board
constexpr int8_t   GECO_SCAN_PINS[]   = {-1};
#endif
constexpr uint32_t READ_INTERVAL_MS   = 60 * 1000;       // one reading per minute (on the full minute once NTP is in)
constexpr uint32_t CONVERSION_MS      = 800;             // 12-bit conversion takes 750 ms
constexpr float    ALARM_HYSTERESIS   = 2.0;             // degrees back inside the limit before an alarm clears
constexpr float    MAX_JUMP           = 10.0;            // a bigger change between readings needs confirmation
constexpr float    MAX_JUMP_FLUE      = 150.0;           // ... on a thermocouple: the flue gains 50 degrees a minute
                                                         // when the fan starts
constexpr float    CONFIRM_TOLERANCE  = 2.0;             // the next reading must be this close to confirm a jump
constexpr uint8_t  FAULT_AFTER        = 3;               // bad readings in a row before a probe is reported dead
constexpr uint8_t  GECO_FAULT_AFTER   = 5;               // ... the controller's: a short silence plus the 2 minutes
                                                         // its temperatures settle after it (GecoBus SETTLE_MS)

// Probe slots. A slot is one place in the history - the minute log, the daily summaries, Eve, the limits and the
// offset are kept per slot, so a probe swapped into a slot carries its history on. Each slot has a role that tells
// the furnace logic what it measures; the logic looks probes up by role (FURNACE, RETURN_WATER, ... below), never by
// number, and whatever needs a missing role simply stays off. Set in the web app (Ustawienia -> Czujniki, flash
// "slots"); a change restarts the ESP, as Home needs its accessories anew. A probe found at a start that no slot
// knows takes a free slot by itself.
// Roles do not care what kind of probe it is: the address says that - a 1-Wire ROM code (DS18B20, family 0x28) or
// one of ours below for a probe off the 1-Wire bus.
// SUPPLY (v36): the water going out to the radiators, on the supply pipe - dad's old furnace probe since 12.11, as the
// furnace itself is read from the controller. Roles are stored by number: a new one goes last.
enum Role : uint8_t { ROLE_OTHER, ROLE_FURNACE, ROLE_RETURN, ROLE_HOPPER, ROLE_BOILER, ROLE_FLUE, ROLE_SUPPLY, ROLE_COUNT };
const char* ROLE_KEYS[ROLE_COUNT] = { "other", "furnace", "return", "hopper", "boiler", "flue", "supply" };
constexpr uint8_t KIND_MAX6675 = 0xF6;   // address {0xF6, CS pin, 0...}: a K thermocouple on a MAX6675 board
constexpr uint8_t KIND_GECO    = 0xF7;   // address {0xF7, 1..3, 0...}: a temperature the Geco controller reports

// Names are what the Home app and the web app show; HomeKit accepts only letters, digits, spaces and apostrophes.
struct __attribute__((packed)) SlotConfig {
  DeviceAddress address;       // all zero = an empty slot
  char          name[32];      // UTF-8
  uint8_t       role;          // Role
  uint8_t       on;            // read, shown and in Home
};

#if CONFIG_IDF_TARGET_ESP32S3
constexpr size_t SENSOR_COUNT = 8;   // slots
#else
constexpr size_t SENSOR_COUNT = 4;   // the classic board keeps its 4
#endif

// The probes at dad's, identified on the bench 2026-10-22 (hand-warming test) - the slots until the web app saves
// others.
const SlotConfig DEFAULT_SLOTS[] = {
  {{0x28, 0x4D, 0x83, 0x77, 0x91, 0x0C, 0x02, 0xF8}, "Piec",           ROLE_FURNACE, 1},   // counterfeit chip, failed at ~60 C in place
  {{0x28, 0xAA, 0xEF, 0x8D, 0x4A, 0x14, 0x01, 0xAE}, "Powrót",         ROLE_RETURN,  1},
  {{0x28, 0xAA, 0x61, 0x94, 0x4A, 0x14, 0x01, 0x6C}, "Zasobnik węgla", ROLE_HOPPER,  1},
  {{0x28, 0xAA, 0xDB, 0x95, 0x4D, 0x14, 0x01, 0x6B}, "Bojler",         ROLE_BOILER,  1},   // hot water tank
};
// Alarm limits a slot has until the web app sets its own - by role, so a replacement probe or a new home gets them
// too (NAN = off): the furnace 40..90, embers creeping into the hopper above 50, a boiler below 35.
const float ROLE_LOW[ROLE_COUNT]  = {NAN, 40, NAN, NAN, 35, NAN, NAN};
const float ROLE_HIGH[ROLE_COUNT] = {NAN, 90, NAN, 50, NAN, NAN, NAN};
constexpr size_t DEFAULT_COUNT = sizeof(DEFAULT_SLOTS) / sizeof(DEFAULT_SLOTS[0]);
SlotConfig slots[SENSOR_COUNT];
// The slots by role, -1 = no probe in that role (mapRoles). FLUE is shown and charted; no warning uses it yet.
int8_t FURNACE = -1, RETURN_WATER = -1, COAL_HOPPER = -1, BOILER = -1, FLUE = -1, SUPPLY = -1;

// Warnings - each one is a contact sensor in Apple Home and a banner in the web app; tuned in the web app.
enum WarningId : uint8_t { WARN_GOING_OUT, WARN_FREEZE, WARN_BURNBACK, WARN_LOW_COAL, WARN_STALLED, WARN_ASH, WARN_STANDBY,
                          WARN_COUNT };
const char* WARNING_NAMES[WARN_COUNT] = { "Piec wygasa", "Ryzyko zamarznięcia", "Cofanie żaru", "Mało węgla",
                                          "Piec stoi po zaniku prądu", "Opróżnij popielnik",
                                          "Sterownik nie w automacie" };

struct Settings {
  bool     goOutOn      = true;   // furnace dying out: falling and closer than goOutMargin degrees to its min limit
  float    goOutMargin  = 3;
  bool     freezeOn     = true;   // any water probe at or below freezeTemp
  float    freezeTemp   = 5;
  bool     burnbackOn   = true;   // coal hopper warming faster than burnbackRate (degrees/hour)
  float    burnbackRate = 10;
  float    hotWater     = 45;     // boiler temperature that counts as "hot water ready" (web app + Home contact)
  bool     coalOn       = true;   // coal tracking; off = no coal card, refills or low-coal warning
  float    hopperKg     = 100;    // kg the hopper holds (Ustawienia -> Dane kotla), 0 = not set (no level estimate)
  float    coalPrice    = 0;      // PLN per tonne, 0 = no costs
  bool     lowCoalOn    = true;   // the hopper estimate runs out within lowCoalHours
  float    lowCoalHours = 12;
  bool     heating      = true;   // heating season; off = summer: no "too cold" on the furnace and the return, no
                                  // burning warnings (comes back by itself after SEASON_AUTO_MIN of burning)
  bool     stalledOn    = true;   // after a power cut the furnace keeps cooling - its controller did not restart
  bool     standbyOn    = true;   // the boiler controller (Geco bus) in standby instead of automat - it needs the A key
  bool     ashOn        = true;   // ash pan reminder: after ashLimit kg burned or ashDays since emptied
  float    ashLimit     = 150;
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
  uint16_t errors      = 0;               // invalid readings since the start (the diagnostic report)
  bool     faulty      = false;
  int8_t   alarm       = 0;               // -1 too cold, 0 ok, +1 too hot
};

struct WarningState {
  bool    active  = false;
  uint8_t pending = 0;    // minutes the condition has held
  uint8_t calm    = 0;    // minutes it has been gone
};

enum EventType : uint8_t {
  EV_BOOT = 1, EV_UPDATED, EV_ALARM_HIGH, EV_ALARM_LOW, EV_ALARM_CLEAR, EV_FAULT, EV_FAULT_CLEAR,
  EV_WARN_ON, EV_WARN_OFF, EV_HISTORY_CLEARED, EV_POWER_CUT,
  EV_SEASON,            // value 1 = heating season on, 0 = summer mode; sensor 1 = switched on by itself
  EV_ASH,               // ash pan emptied
  EV_HOT_WATER,         // the boiler reached its "ready" temperature (value = degrees x 100)
  EV_BATH,              // hot water drawn - a bath, a shower: time = when it began, value = how far the boiler fell
                        // (degrees x 100), sensor = minutes it took (see BathWatch)
  EV_SETTING,           // a setting of the boiler controller moved, read off its work (SettingWatch): sensor = the setting,
                        // value = old x 256 + new; sensor MENU = a visit to its service menu, value = seconds
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
  uint32_t heat;                      // degree-minutes of furnace minus return (heatOf) - the charts' heat (v32's coal)
};

struct __attribute__((packed)) RefillRecord {
  uint32_t time;                      // when the coal went in (may be entered later)
  uint16_t kg10;                      // kilograms x 10 (up to v21 buckets x 10 - see coalToKilograms)
  uint8_t  full;                      // 1 = filled the hopper to the top
  uint8_t  reserved;                  // v33: bits 0-2 how full it was before - 0 not said, 1 empty, 2 a quarter, 3 half,
                                      // 4 three quarters; bits 3-7 the app's tag of the entry (a retry carries the same)

  bool isFull() const { return full != 0; }
  uint8_t before() const { return reserved & 7; }
};

// A slot showing another one's history before a change (handOn, v35).
struct __attribute__((packed)) Takeover {
  uint8_t  to, from;      // slots; to == from = an unused entry
  uint32_t at;            // Unix time of the change
};
constexpr size_t  TAKEOVER_MAX      = 4;
constexpr int16_t TAKEOVER_DISAGREE = 1000;   // degrees x 100
Takeover takeovers[TAKEOVER_MAX] = {};

// An hour of the boiler controller's work, worked out from its log (countWork) and kept for years: the charts
// beyond the log's reach, the day summaries and the coal counted from the feeder.
struct __attribute__((packed)) WorkHour {
  uint32_t time;                      // the hour's start (Unix, a multiple of 3600 - a whole hour in Polish time too)
  uint16_t heard;                     // seconds the controller was heard
  uint16_t burning;                   // ... of them in each mode
  uint16_t holding;
  uint16_t standby;
  uint16_t fan;                       // seconds each output ran
  uint16_t pump;
  uint16_t dhw;                       // ... the hot water pump too
  uint16_t feed10;                    // the feeder, in tenths of a second
  uint16_t feedBurn10;                // ... of it while burning, a burn's end included (the stroke finishing)
  uint16_t heatHeard;                 // the heat model's degree-minutes while the controller was heard (by the hour's
                                      // heard share) - what the feeder counts instead (heatHeardOf)
  uint16_t flueSum;                   // the flue FLUE_MARK_S into the burns begun this hour without the hot water pump, degrees
  uint8_t  flueN;                     // ... how many burns that adds up
  uint8_t  burns;                     // burns begun
  uint8_t  strokes;                   // piston cycles completed (away from home, then back)
  uint8_t  blows;                     // przedmuchy begun (in podtrzymanie the fan or the feeder comes on; the run that
                                      // finishes the stroke after a burn is no przedmuch)
};

// What a stretch of time took (useBetween): the feeder piston's strokes, and the seconds the controller was heard.
struct CoalUse {
  float strokes = 0, heard = 0, span = 0;
};

// One window of STARVE_WINDOW_S (flueWindows): the flue's peak and the feeding in it.
struct FlueWindow {
  float    peak    = NAN;             // the flue's highest minute reading, NAN without one
  float    feed    = 0;               // feeder seconds
  float    burn    = 0;               // seconds the controller was burning
  uint16_t strokes = 0;               // piston strokes finished
};

using MinuteRecord = MinuteLog<SENSOR_COUNT>::Record;

OneWire                 oneWire(ONE_WIRE_PIN);
DallasTemperature       bus(&oneWire);
WebServer               server(WEB_PORT);
Preferences             prefs;
RemoteUpdate            updater(UPDATE_MANIFEST_URL, FW_VERSION);
Relay                   relay;
GecoBus                 geco;                               // the boiler controller's panel bus, read only
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
bool     hotWaterReady     = false;
int      networkUps        = 0;                 // WiFi connections since the start (the diagnostic report)

// Coal is counted in kilograms - what goes into the hopper; costs are the web app's (x price per tonne).
float    strokeKg          = NAN;               // kg of coal one piston stroke pushes, learnt from refills (flash
uint16_t strokeSamples     = 0;                 // "strkKg"), and the refill-to-refill intervals it came from ("strkN")
uint32_t lastFullAt        = 0;                 // last refill to the top, 0 = none
float    coalCap           = NAN;               // kg the hopper holds: the setting (Dane kotla) - the owner's word for
                                                // "full" (v33 took refills into an empty hopper; v34 no more)
uint32_t anchorAt          = 0;                 // the hopper's content was last known then (0 = never): a refill to the
float    anchorKg          = NAN;               // top or one saying how full it was - what it held right after
float    addedSince        = 0;                 // ... and the kg put in later by refills of unknown fill
RefillRecord lastRefill    = {};                // the newest refill (time 0 = none)
float    coalLevel         = NAN;               // kg in the hopper (estimate)
float    coalPace          = NAN;               // piston strokes per hour heard: the busier of the last day and three
float    coalRate          = NAN;               // ... in kg per hour
float    coalHoursLeft     = NAN;               // at that pace; INFINITY when the furnace hardly burns
uint32_t starvedSince      = 0;                 // the burns stopped heating the flue then - no coal comes (0 = it does)
float    starvedUsual      = NAN;               // the usual flue peak when that began (the starved windows would drag
                                                // a fresh median down until they clear themselves)
uint32_t ashAt             = 0;                 // ash pan last emptied (flash "ashAt"); counting starts with the clock
float    ashBurned         = NAN;               // coal burned since then, kg (once the coal use is known)
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
uint32_t offerTold         = 0;       // the newest version a notification has offered (flash "offerTold")
uint32_t summaryDay        = 0;       // local midnight of the day whose morning summary went out (flash "sumDay")

bool clockSet(time_t t) { return t >= 1700000000; }

// ------------------------------------------------------------------ health (the diagnostic report)

// A day of what tells how the device copes: reconnects, probe errors, failed update checks, the least free RAM,
// the slowest loop pass, the weakest WiFi, the hottest chip and the starts. The day being counted lives in RTC
// memory, so a crash or an update restart keeps it - those are the days it matters; a power cut loses its part.
struct __attribute__((packed)) HealthRecord {
  uint32_t time;          // local midnight of the day, 0 = counted before the clock was set
  uint16_t wifiUps;       // WiFi connections
  uint16_t relayUps;      // relay connections
  uint16_t probeErrors;   // invalid readings, all probes
  uint16_t updateFails;   // update checks and downloads that failed
  uint16_t heapMinKB;     // least free internal RAM
  uint16_t loopMaxMs;     // longest loop pass (a WiFi connect in HomeSpan included)
  int8_t   rssiMin;       // weakest WiFi signal (dBm), 0 = none measured
  int8_t   chipMax;       // hottest chip (C), -128 = not measured
  uint16_t starts;        // starts of the ESP
};

constexpr uint32_t HEALTH_MAGIC = 0x484C5431;   // "HLT1" - change with HealthRecord
RTC_NOINIT_ATTR HealthRecord healthToday;
RTC_NOINIT_ATTR uint32_t     healthMagic;
SegmentLog<HealthRecord> health("/hl", HEALTH_CAPACITY / 2);
SegmentLog<GecoBus::Event> gecoLog("/gw", GECO_CAPACITY / 2);   // the controller's work: every change (GecoBus.h)
std::vector<GecoBus::Event> gecoPending;                        // changes not on flash yet (logGecoChanges)
SegmentLog<WorkHour> workLog("/wk2", WORK_CAPACITY / 2);        // ... and hour by hour (countWork)
uint32_t workDone = 0;                          // the newest hour in workLog (its start), 0 = none yet
uint16_t relayUpsSeen = 0, updateFailsSeen = 0;

void healthFresh(uint32_t day) {
  memset(&healthToday, 0, sizeof(healthToday));
  healthToday.time = day;
  healthToday.heapMinKB = 0xFFFF;
  healthToday.chipMax = -128;
}

void healthBegin() {
  if (healthMagic != HEALTH_MAGIC) {             // a power-on: RTC memory holds noise
    healthFresh(0);
    healthMagic = HEALTH_MAGIC;
  }
  healthToday.starts++;
}

// A counter that stops at its top instead of wrapping (packed fields bind to no reference - hence by value).
uint16_t add16(uint16_t v, uint32_t by) { return min<uint32_t>((uint32_t)v + by, 65535); }

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

// "28AAEF8D4A1401AE" - how the web app shows and sends an address.
String hexAddress(const DeviceAddress a) {
  char buf[17];
  for (uint8_t i = 0; i < 8; i++) sprintf(buf + 2 * i, "%02X", a[i]);
  return buf;
}

bool isThermocouple(const DeviceAddress a) {
  return TC_CS_PIN >= 0 && a[0] == KIND_MAX6675 && a[1] == TC_CS_PIN;
}

// A temperature the Geco controller reports on its panel bus: 1 boiler, 2 feeder, 3 hot water.
bool isGeco(const DeviceAddress a) {
  return GECO_RX_PIN >= 0 && a[0] == KIND_GECO && a[1] >= 1 && a[1] <= 3;
}

// What kind of probe an address is, for the web app.
const char* probeKind(const DeviceAddress a) {
  return isUnset(a) ? "" : a[0] == KIND_MAX6675 ? "Termopara K" : a[0] == KIND_GECO ? "Sterownik Geco" : "DS18B20";
}

// Well-formed UTF-8: /api/now and Home's accessory list carry the names as they are, and a broken byte (a shell in
// another code page) makes the whole JSON unreadable.
bool validUtf8(const String& s) {
  for (size_t i = 0, n = s.length(); i < n;) {
    uint8_t c = s[i];
    size_t more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 && c >= 0xC2 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 && c <= 0xF4 ? 3 : 9;
    if (more > 3 || n - i <= more) return false;           // not a lead byte, or the sequence is cut short
    for (size_t k = 1; k <= more; k++)
      if (((uint8_t)s[i + k] & 0xC0) != 0x80) return false;
    i += more + 1;
  }
  return true;
}

// An address a slot may take: empty, a 1-Wire temperature probe (valid CRC) or our thermocouple.
bool parseAddress(const String& hex, DeviceAddress out) {
  memset(out, 0, 8);
  if (hex.isEmpty()) return true;
  if (hex.length() != 16) return false;
  for (uint8_t i = 0; i < 8; i++) {
    char* end;
    out[i] = strtoul(hex.substring(2 * i, 2 * i + 2).c_str(), &end, 16);
    if (*end) return false;
  }
  return isThermocouple(out) || isGeco(out) || (bus.validAddress(out) && bus.validFamily(out));
}

// One 16-bit frame from the MAX6675, SCK idling low and SO read before each falling edge: D15 is always 0, D14..D3
// the temperature in quarter degrees, D2 = no thermocouple on the screws, D1 always 0. With no board on the pins SO
// is pulled up and reads 0xFFFF. Pulling CS up starts the next conversion (~220 ms).
uint16_t max6675Frame() {
  digitalWrite(TC_CS_PIN, LOW);
  delayMicroseconds(10);
  uint16_t v = 0;
  for (uint8_t b = 0; b < 16; b++) {
    digitalWrite(TC_SCK_PIN, LOW);
    delayMicroseconds(10);
    v = v << 1 | digitalRead(TC_SO_PIN);
    digitalWrite(TC_SCK_PIN, HIGH);
    delayMicroseconds(10);
  }
  digitalWrite(TC_SCK_PIN, LOW);
  digitalWrite(TC_CS_PIN, HIGH);
  return v;
}

bool max6675Present() {
  if (TC_CS_PIN < 0) return false;
  uint16_t f = max6675Frame();
  if (f == 0) {                              // right after power-up the board has no conversion yet
    delay(250);
    f = max6675Frame();
  }
  return f != 0xFFFF && f != 0 && !(f & 0x8002);
}

// The thermocouple board, when one answers; its address goes to `a`.
bool findThermocouple(DeviceAddress a) {
  memset(a, 0, 8);
  a[0] = KIND_MAX6675;
  a[1] = (uint8_t)TC_CS_PIN;
  return max6675Present();
}

float readThermocouple() {
  uint16_t f = max6675Frame();
  return f == 0xFFFF || f == 0 || (f & 0x8006) ? NAN : (f >> 3) * 0.25f;
}

int slotOf(const DeviceAddress a) {
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    if (!isUnset(slots[i].address) && memcmp(slots[i].address, a, 8) == 0) return i;
  return -1;
}

bool slotUsed(size_t i) { return slots[i].on && !isUnset(slots[i].address); }

// The freeze warning watches the water: the furnace, the supply, the return and the boiler.
bool isWater(size_t i) {
  uint8_t r = slots[i].role;
  return r == ROLE_FURNACE || r == ROLE_RETURN || r == ROLE_BOILER || r == ROLE_SUPPLY;
}

// FURNACE, RETURN_WATER, ... from the roles: the first slot in use with that role.
void mapRoles() {
  int8_t* byRole[ROLE_COUNT] = {nullptr, &FURNACE, &RETURN_WATER, &COAL_HOPPER, &BOILER, &FLUE, &SUPPLY};
  FURNACE = RETURN_WATER = COAL_HOPPER = BOILER = FLUE = SUPPLY = -1;
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    uint8_t r = slots[i].role;
    if (slotUsed(i) && r < ROLE_COUNT && byRole[r] && *byRole[r] < 0) *byRole[r] = i;
  }
}

// A slot that gets another probe (the web app, or found at a start): the old probe's offset goes - a calibration is
// the probe's own - and its alarm and fault are not brought back (flash "freshSlots", read by restoreAlerts); a slot
// emptied or newly taken loses its limits too.
void forgetSlot(size_t i, bool limitsToo) {
  prefs.remove(prefKey("off", i).c_str());
  if (limitsToo) {
    prefs.remove(prefKey("lo", i).c_str());
    prefs.remove(prefKey("hi", i).c_str());
  }
  prefs.putUChar("freshSlots", prefs.getUChar("freshSlots", 0) | 1 << i);
}

// A probe no slot knows goes into the first free slot. False when all are taken.
bool placeProbe(const DeviceAddress found, const char* name, uint8_t role) {
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    if (!isUnset(slots[i].address)) continue;
    forgetSlot(i, true);
    memcpy(slots[i].address, found, 8);
    strlcpy(slots[i].name, name, sizeof(slots[i].name));
    slots[i].role = role;
    slots[i].on = 1;
    techLog.printf("New probe %s - slot %u\n", hexAddress(found).c_str(), (unsigned)(i + 1));
    return true;
  }
  techLog.printf("New probe %s - no free slot\n", hexAddress(found).c_str());
  return false;
}

// The slots from flash (the defaults until the web app saves them), every probe found that no slot knows put into
// a free slot - a thermocouple straight into the flue role while no slot has it - and the roles looked up.
void loadSlots() {
  if (TC_CS_PIN >= 0) {
    pinMode(TC_CS_PIN, OUTPUT);
    digitalWrite(TC_CS_PIN, HIGH);
    pinMode(TC_SCK_PIN, OUTPUT);
    digitalWrite(TC_SCK_PIN, LOW);
    pinMode(TC_SO_PIN, INPUT_PULLUP);
  }
  if (prefs.getBytes("slots", slots, sizeof(slots)) != sizeof(slots)) {
    memset(slots, 0, sizeof(slots));
    for (size_t i = 0; i < DEFAULT_COUNT && i < SENSOR_COUNT; i++) slots[i] = DEFAULT_SLOTS[i];
  }
  for (auto& slot : slots) slot.name[sizeof(slot.name) - 1] = 0;
  mapRoles();
  bool added = false;
  DeviceAddress found;
  oneWire.reset_search();
  while (oneWire.search(found)) {
    if (!bus.validAddress(found) || !bus.validFamily(found) || slotOf(found) >= 0) continue;
    char name[16];
    snprintf(name, sizeof(name), "Czujnik %02X", found[7]);   // the CRC byte - tells two new probes apart
    added |= placeProbe(found, name, ROLE_OTHER);
  }
  DeviceAddress tc;
  if (findThermocouple(tc) && slotOf(tc) < 0) added |= placeProbe(tc, "Spaliny", FLUE < 0 ? ROLE_FLUE : ROLE_OTHER);
  // The controller's own temperatures, once its bus is heard (it speaks every ~57 ms): three more probes.
  static const char* GECO_NAMES[] = {"", "Sterownik kocioł", "Sterownik podajnik", "Sterownik CWU"};
  for (uint32_t waited = 0; GECO_RX_PIN >= 0 && !geco.fresh() && waited < 600; waited += 50) delay(50);
  for (uint8_t ch = 1; ch <= 3 && geco.fresh(); ch++) {
    DeviceAddress g = {KIND_GECO, ch};
    if (slotOf(g) < 0) added |= placeProbe(g, GECO_NAMES[ch], ROLE_OTHER);
  }
  if (added) prefs.putBytes("slots", slots, sizeof(slots));
  mapRoles();
}

// Every probe found with its address, temperature and slot. Blocks ~750 ms for a fresh conversion.
String busReport() {
  bus.setWaitForConversion(true);
  bus.requestTemperatures();
  bus.setWaitForConversion(false);

  String out;
  uint8_t count = 0;
  auto line = [&](const DeviceAddress a, float t) {
    int slot = slotOf(a);
    char buf[160];
    snprintf(buf, sizeof(buf), "%u: %s %s  %7.2f C  %s\n", ++count, probeKind(a), formatAddress(a).c_str(), t,
             slot >= 0 ? slots[slot].name : "(no slot)");
    out += buf;
  };
  DeviceAddress a;
  oneWire.reset_search();
  while (oneWire.search(a))
    if (bus.validAddress(a)) line(a, bus.getTempC(a));
  DeviceAddress tc;
  if (findThermocouple(tc)) line(tc, readThermocouple());
  for (uint8_t ch = 1; ch <= 3 && geco.fresh(); ch++) {
    DeviceAddress g = {KIND_GECO, ch};
    line(g, geco.temp(ch));
  }
  if (count == 0) out = "No probe found - check the wiring and the pull-up resistor\n";
  return out;
}

// ------------------------------------------------------------------ event journal

// at: when it happened, 0 = now.
void logEvent(EventType type, uint8_t sensor, int16_t value, time_t at = 0) {
  time_t now = time(nullptr);
  EventRecord e = {(uint32_t)(at ? at : now), (uint8_t)type, sensor, value};
  if (!clockSet(now)) {
    e.time = millis() / 1000;
    if (pendingEventCount < 8) pendingEvents[pendingEventCount++] = e;
    return;
  }
  events.add(e);
  pushEvent(e);
}

// A notification on the paired phones (Relay.h -> relay -> Web Push) for what matters away from home: alarms and
// their end, dead probes, warnings, power cuts of a minute or more, hot water ready, a finished update. Restored problems are not
// journaled again after a restart (SavedAlerts), so they are not announced twice. The phone words it (web/sw.js).
void pushEvent(const EventRecord& e) {
  switch (e.type) {
    case EV_UPDATED: case EV_ALARM_HIGH: case EV_ALARM_LOW: case EV_ALARM_CLEAR: case EV_FAULT: case EV_FAULT_CLEAR:
    case EV_WARN_ON: case EV_HOT_WATER:
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
  if (((e.type >= EV_ALARM_HIGH && e.type <= EV_FAULT_CLEAR) || e.type == EV_HOT_WATER) && e.sensor < SENSOR_COUNT)
    j += String(",\"n\":\"") + slots[e.sensor].name + "\"";
  // Which kind it is - each phone chooses what it gets (the relay filters; it cannot read the alert itself).
  const char* kind = e.type == EV_UPDATED ? "update" : e.type == EV_POWER_CUT ? "power" : e.type == EV_HOT_WATER ? "water"
                     : e.type == EV_WARN_ON || e.type == EV_WARN_OFF ? "warn" : "alarm";
  relay.alert(j + "}", kind);
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
  uint8_t fresh = prefs.getUChar("freshSlots", 0);          // slots with another probe since (forgetSlot)
  SavedAlerts a;
  if (prefs.isKey("alerts") && prefs.getBytes("alerts", &a, sizeof(a)) == sizeof(a)) {
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
      if (!slotUsed(i) || fresh >> i & 1) continue;          // a slot switched off or with a new probe starts clean
      state[i].alarm  = constrain(a.alarm[i], -1, 1);
      state[i].faulty = a.faulty >> i & 1;
      if (state[i].faulty) state[i].badReadings = FAULT_AFTER;
    }
    for (uint8_t w = 0; w < WARN_COUNT; w++) warnings[w].active = a.warnings >> w & 1;
    hotWaterReady = a.hotWater && BOILER >= 0;
  }
  if (fresh) {
    saveAlerts();
    prefs.remove("freshSlots");
  }
}

// ------------------------------------------------------------------ HomeKit


void createAccessories() {
  new SpanAccessory();                       // the bridge itself
    new Service::AccessoryInformation();
      new Characteristic::Identify();

  // A slot's accessory keeps its number (slot + 2) whatever else changes, so Home keeps its rooms and history.
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    if (!slotUsed(i)) continue;
    SensorState& s = state[i];
    bool tc = isThermocouple(slots[i].address);
    snprintf(s.serial, sizeof(s.serial), "FURNACE-%u", (unsigned)(i + 1));
    snprintf(s.historyName, sizeof(s.historyName), "%s historia", slots[i].name);
    snprintf(s.historyPath, sizeof(s.historyPath), "/eve%u.bin", (unsigned)i);
    new SpanAccessory(i + 2);
      new Service::AccessoryInformation();
        new Characteristic::Identify();
        new Characteristic::Name(slots[i].name);
        new Characteristic::Manufacturer("HomeSpan");
        new Characteristic::Model(tc ? "MAX6675 K thermocouple" : isGeco(slots[i].address) ? "Geco controller" : "DS18B20 probe");
        new Characteristic::SerialNumber(s.serial);
        new Characteristic::FirmwareRevision(firmwareRevision);
      new Service::TemperatureSensor();
        new Characteristic::Name(slots[i].name);
        s.temperature = new Characteristic::CurrentTemperature(0);
        if (tc) s.temperature->setRange(0, 1000);
        else s.temperature->setRange(-55, 125);
        s.fault = new Characteristic::StatusFault(s.faulty ? 1 : 0);
        new Characteristic::TemperatureDisplayUnits(0);   // Celsius - Eve shows a weather history only with this set
      s.history = new EveHistory(s.historyName, s.historyPath);
  }

}

// Pushes the state of one probe to HomeKit; only changed values are sent.
// (MQTT can hook in here later, next to HomeKit.)
void reportSensor(size_t i) {
  SensorState& s = state[i];
  if (!s.temperature) return;                // a slot not in Home
  // A dead probe sends no temperature at all - Home keeps the last real value and shows the fault flag
  // (the old firmware sent -127, which HomeKit clamped to the -100 you saw).
  if (!s.faulty && !isnan(s.lastGood)) {
    float value = roundf(s.lastGood * 10) / 10;
    if (s.temperature->getVal<float>() != value) s.temperature->setVal(value);
  }
  int fault = s.faulty ? 1 : 0;
  if (s.fault->getVal() != fault) s.fault->setVal(fault);
}

// ------------------------------------------------------------------ measurement

// Up to three scratchpad reads. The library already rejects CRC errors and all-zero frames
// (a shorted bus reads as zeros, which is where the old "0 on every sensor" came from).
// A thermocouple is read once (its board converts every ~220 ms on its own).
float readSensor(size_t i) {
  if (isThermocouple(slots[i].address)) return readThermocouple();
  if (isGeco(slots[i].address)) return geco.temp(slots[i].address[1]);   // NAN while the controller is not heard
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    float t = bus.getTempC(slots[i].address);
    if (t == DEVICE_DISCONNECTED_C || t < -55 || t > 125) continue;
    // 85.0 is the power-on value of a probe that reset mid-cycle - believe it only when we were already near it.
    if (t == 85.0f && !(fabsf(state[i].lastGood - 85.0f) < 10.0f)) continue;
    return t;
  }
  return NAN;
}

// Once a minute: RAM, WiFi, chip, the reconnect counters - and at a new day the finished one goes to "/hl".
void healthTick(time_t now) {
  HealthRecord& h = healthToday;
  h.heapMinKB = min<uint32_t>(h.heapMinKB, ESP.getFreeHeap() / 1024);
  if (WiFi.status() == WL_CONNECTED) {
    int8_t r = WiFi.RSSI();
    if (r < 0 && (!h.rssiMin || r < h.rssiMin)) h.rssiMin = r;
  }
#if CONFIG_IDF_TARGET_ESP32S3
  float chip = temperatureRead();
  if (!isnan(chip) && chip > h.chipMax) h.chipMax = (int8_t)constrain(lroundf(chip), -127, 127);
#endif
  uint16_t ups = relay.connections(), fails = updater.failures();
  h.relayUps = add16(h.relayUps, (uint16_t)(ups - relayUpsSeen));
  h.updateFails = add16(h.updateFails, (uint16_t)(fails - updateFailsSeen));
  relayUpsSeen = ups;
  updateFailsSeen = fails;
  if (!clockSet(now)) return;
  uint32_t today = localMidnight(now);
  if (!h.time) h.time = today;
  if (h.time == today) return;
  if (h.time < today) health.add(h);
  healthFresh(today);
}

// Every alarm change goes to the journal - the web app pairs starts with ends from it.
void logAlarmChange(size_t i, int8_t before, float t) {
  int8_t now = state[i].alarm;
  if (now == before) return;
  techLog.printf("%s: alarm %s\n", slots[i].name, now > 0 ? "TOO HOT" : now < 0 ? "TOO COLD" : "cleared");
  logEvent(now > 0 ? EV_ALARM_HIGH : now < 0 ? EV_ALARM_LOW : EV_ALARM_CLEAR, i, toHundredths(t));
  saveAlerts();
}

// Summer mode silences "too cold" on the furnace and the return water - they are cold on purpose then.
float lowLimit(size_t i) {
  return !settings.heating && ((int)i == FURNACE || (int)i == RETURN_WATER) ? NAN : state[i].low;
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
  float maxJump = isThermocouple(slots[i].address) ? MAX_JUMP_FLUE : MAX_JUMP;
  if (!isnan(s.lastGood) && fabsf(t - s.lastGood) > maxJump && !(fabsf(t - s.unconfirmed) <= CONFIRM_TOLERANCE)) {
    s.unconfirmed = t;
    techLog.printf("%s: %.2f C looks like a glitch, waiting for confirmation\n", slots[i].name, t);
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
  healthToday.probeErrors = add16(healthToday.probeErrors, 1);
  techLog.printf("%s: no valid reading\n", slots[i].name);
  if (s.badReadings < 255) s.badReadings++;
  if (s.errors < 65535) s.errors++;
  if (s.badReadings >= (isGeco(slots[i].address) ? GECO_FAULT_AFTER : FAULT_AFTER) && !s.faulty) {
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
  techLog.printf("Warning %s: %s\n", WARNING_NAMES[w], s.active ? "ON" : "off");
  logEvent(s.active ? EV_WARN_ON : EV_WARN_OFF, w, toHundredths(value));
  saveAlerts();
}

float burnThreshold() {
  return FURNACE < 0 || isnan(state[FURNACE].low) ? 40 : state[FURNACE].low;
}

// A journal value is degrees x 100 in an int16 - kilograms are capped to fit; NAN stays "no value".
float upTo320(float v) {
  return isnan(v) ? NAN : fminf(v, 320);
}

// tick = the minute's reading (advances the hold timers); other callers (settings, refills) only apply what was
// switched off.
void evaluateWarnings(bool tick = false) {
  // Summer mode: the warnings about burning stay quiet; freezing, embers in the hopper and hot water keep working.
  const bool heating = settings.heating;

  // Furnace dying out: falling and already within goOutMargin of its min limit - the step before the "too cold"
  // alarm. A fast fall alone says nothing: hot water drawn for a bath pulls the furnace down fast and it comes back,
  // and a furnace going out need not fall fast at all. Needs the min limit; only while still above it, with one
  // degree more to clear.
  static const SensorState noProbe = [] { SensorState n; n.faulty = true; return n; }();   // a role nobody has
  const SensorState& f = FURNACE >= 0 ? state[FURNACE] : noProbe;
  float slope = FURNACE >= 0 ? trendPerHour(FURNACE) : NAN;
  bool burning = !f.faulty && !isnan(f.lastGood) && !isnan(f.low) && f.lastGood > f.low;
  float margin = settings.goOutMargin + (warnings[WARN_GOING_OUT].active ? 1 : 0);
  bool dying = burning && !isnan(slope) && slope < -0.5f && f.lastGood <= f.low + margin;
  goOutForecast = dying ? lroundf((f.lastGood - f.low) / -slope * 60) : -1;
  setWarning(WARN_GOING_OUT, dying, !burning || !isnan(slope), 5, 10, heating && settings.goOutOn && FURNACE >= 0, f.lastGood, tick);

  // Freezing: the coldest water probe, with one degree of hysteresis.
  float coldest = NAN;
  bool anyWater = false;
  for (size_t i = 0; i < SENSOR_COUNT; i++) anyWater |= slotUsed(i) && isWater(i);
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    if (slotUsed(i) && isWater(i) && !state[i].faulty && !isnan(state[i].lastGood))
      coldest = isnan(coldest) ? state[i].lastGood : fminf(coldest, state[i].lastGood);
  bool freezing = !isnan(coldest) && coldest < settings.freezeTemp + (warnings[WARN_FREEZE].active ? 1 : 0);
  setWarning(WARN_FREEZE, freezing, !isnan(coldest), 2, 2, settings.freezeOn && anyWater, coldest, tick);

  // Embers creeping into the coal hopper: its temperature climbing fast. Also in summer mode (a fire for hot
  // water), and a probe that dies - embers may have destroyed it - keeps the warning as it was.
  const SensorState& h = COAL_HOPPER >= 0 ? state[COAL_HOPPER] : noProbe;
  float hopper = COAL_HOPPER >= 0 ? trendPerHour(COAL_HOPPER) : NAN;
  bool burnback = !isnan(hopper) && hopper >= settings.burnbackRate;
  setWarning(WARN_BURNBACK, burnback, !isnan(hopper) && !h.faulty, 3, 10, settings.burnbackOn && COAL_HOPPER >= 0,
             h.lastGood, tick);

  // Low coal: at the current pace the hopper estimate runs out within lowCoalHours, or is nearly empty (half again as
  // long before it clears, so a wobbling pace does not flap it) - or the burns stopped heating the flue: the hopper is
  // empty or the coal hangs, whatever the count says. Logged with the kg left.
  float lowHours = settings.lowCoalHours * (warnings[WARN_LOW_COAL].active ? 1.5f : 1);
  // Unknown while there is a factor but no estimate yet (after a power cut, before the clock).
  bool lowCoal = starvedSince || (!isnan(coalLevel) && (coalHoursLeft < lowHours || coalLevel < coalCap * 0.05f));
  bool coalKnown = starvedSince || !isnan(coalLevel) || isnan(strokeKg) || isnan(coalCap);
  // The kg left, or -1 when the burns went cold: an empty hopper is not "35 kg left", whatever the count says.
  setWarning(WARN_LOW_COAL, lowCoal, coalKnown, 2, 5, heating && settings.coalOn && settings.lowCoalOn,
             starvedSince ? -1.0f : upTo320(coalLevel),
             tick);

  // Stalled after a power cut: it burned when the power came back and keeps cooling - many furnace controllers
  // need a hand after a cut. Clears once the furnace warms up again; the watch ends after STALL_WATCH_MS.
  if (stallWatch && millis() >= STALL_WATCH_MS && !warnings[WARN_STALLED].active) stallWatch = false;
  bool stalled;
  if (warnings[WARN_STALLED].active) stalled = !(slope >= 2) && !(f.lastGood >= stallFrom - 1);
  else stalled = stallWatch && millis() < STALL_WATCH_MS && f.lastGood <= stallFrom - STALL_DROP && slope <= STALL_SLOPE;
  if (geco.fresh()) stalled = false;          // the controller's own word (WARN_STANDBY) replaces the guess
  setWarning(WARN_STALLED, stalled, !isnan(slope) || !stallWatch, 3, 10, heating && settings.stalledOn && FURNACE >= 0, f.lastGood,
             tick);

  // The controller not in automat (Geco bus, S3 only): after every power cut it comes back in standby and the fire
  // goes out unless someone presses A. Its own word, held two minutes (the keys in standby are someone at work);
  // unknown while the bus is quiet.
  setWarning(WARN_STANDBY, geco.state().mode == GecoBus::STANDBY, geco.fresh(), 2, 1, heating && settings.standbyOn,
             f.lastGood, tick);

  // The ash pan's reminder is gone (v34, not needed): off for good, an old one closes.
  setWarning(WARN_ASH, false, true, 1, 1, false, NAN, tick);

  // Hot water ready: not a problem, a notification "you can shower" when the boiler is warm enough - at most one
  // an hour, as a boiler around its "ready" temperature crosses it back and forth.
  const SensorState& b = BOILER >= 0 ? state[BOILER] : noProbe;
  if (BOILER < 0 && hotWaterReady) {                        // the boiler's role went to no probe
    hotWaterReady = false;
    saveAlerts();
  }
  if (!b.faulty && !isnan(b.lastGood)) {
    bool ready = b.lastGood >= settings.hotWater - (hotWaterReady ? HOT_WATER_HYSTERESIS : 0);
    if (ready != hotWaterReady) {
      hotWaterReady = ready;
      saveAlerts();
      static uint32_t lastToldMs = 0;
      if (ready && (!lastToldMs || millis() - lastToldMs >= 3600UL * 1000)) {
        lastToldMs = millis();
        logEvent(EV_HOT_WATER, BOILER, toHundredths(b.lastGood));
      }
    }
  }
}

// ------------------------------------------------------------------ the controller's work, hour by hour

// The flue at moment t, between its two minute readings; NAN without both.
float flueAt(uint32_t t) {
  if (FLUE < 0 || !minutes.newest()) return NAN;
  uint32_t m = t / 60;
  int16_t a = NO_VALUE, b = NO_VALUE;
  minutes.forEach(m, m + 1, [&](uint32_t k, const MinuteRecord& r) { (k == m ? a : b) = r.v[FLUE]; });
  return a == NO_VALUE || b == NO_VALUE ? NAN : (a + (b - a) * (int32_t)(t % 60) / 60.0f) / 100.0f;
}

// The controller's work from the hour `from` (its start) up to `until`, worked out from its log on flash and the
// changes not there yet: hour(w) for every hour that ends by `until`, then - when `partial` - the hour `until` falls
// in with its part so far. One pass over the log however many hours (all of it at the first start). A state lasts
// until the next change; every start closes the log (closeGecoLog), so no state runs on through a power cut.
void countWork(uint32_t from, uint32_t until, bool partial, const std::function<void(const WorkHour&)>& hour) {
  if (until <= from) return;
  enum { HEARD, BURNING, HOLDING, STANDBY, FAN, PUMP, DHW, FEED, FEED_BURN, SUMS };
  uint32_t sum[SUMS] = {}, at = 0, start = from;            // at: tenths of a second into the hour `start`
  uint8_t strokes = 0, blows = 0, burns = 0, marks = 0;
  uint32_t marked[12];                                      // when the burns to read the flue of began
  GecoBus::Event st = gecoLog.forRange(from, from, [](const GecoBus::Event&) {});
  bool inBurn = st.mode == GecoBus::BURNING;                // the last working mode: the service menu and a bus gap end no burn
  bool tail = false;                                        // a burn's end: the stroke finishing and the fan's after-run
  auto blowing = [](const GecoBus::Event& e) {
    return e.mode == GecoBus::HOLDING && (e.outputs & (GecoBus::FAN | GecoBus::FEEDER));
  };
  auto run = [&](uint32_t to) {                              // the state so far, up to `to` tenths into the hour
    if (to <= at) return;
    uint32_t d = to - at;
    at = to;
    if (st.mode == GecoBus::NO_DATA) return;
    sum[HEARD] += d;
    if (st.mode == GecoBus::BURNING) sum[BURNING] += d;
    else if (st.mode == GecoBus::HOLDING) sum[HOLDING] += d;
    else if (st.mode == GecoBus::STANDBY) sum[STANDBY] += d;
    if (st.outputs & GecoBus::FAN) sum[FAN] += d;
    if (st.outputs & GecoBus::CO_PUMP) sum[PUMP] += d;
    if (st.outputs & GecoBus::DHW_PUMP) sum[DHW] += d;
    if (st.outputs & GecoBus::FEEDER) {
      sum[FEED] += d;
      if (st.mode == GecoBus::BURNING || tail) sum[FEED_BURN] += d;
    }
  };
  auto close = [&]() {
    auto sec = [&](uint8_t k) { return (uint16_t)min<uint32_t>((sum[k] + 5) / 10, 3600); };
    uint32_t flue = 0;
    uint8_t flueN = 0;
    for (uint8_t k = 0; k < marks; k++) {
      float f = flueAt(marked[k] + FLUE_MARK_S);
      if (isnan(f)) continue;
      flue += lroundf(f);
      flueN++;
    }
    WorkHour w = {start, sec(HEARD), sec(BURNING), sec(HOLDING), sec(STANDBY), sec(FAN), sec(PUMP), sec(DHW),
                  (uint16_t)min<uint32_t>(sum[FEED], 36000), (uint16_t)min<uint32_t>(sum[FEED_BURN], 36000), 0,
                  (uint16_t)min<uint32_t>(flue, 65535), flueN, burns, strokes, blows};
    hour(w);
    memset(sum, 0, sizeof(sum));
    strokes = blows = burns = marks = 0;
    at = 0;
    start += 3600;
  };
  auto take = [&](const GecoBus::Event& e) {
    if (e.time < start) {                                    // before the hour being counted: newer than the state found
      st = e;                                                // on flash, or the clock stepped back
      return;
    }
    while (e.time >= start + 3600) {
      run(36000);
      close();
    }
    run((e.time - start) * 10 + e.tenths);
    if (e.mode != GecoBus::NO_DATA) tail = blowing(e) && (tail || inBurn);
    if (st.mode != GecoBus::NO_DATA && e.mode != GecoBus::NO_DATA) {
      if ((st.flags & GecoBus::PISTON_AWAY) && !(e.flags & GecoBus::PISTON_AWAY) && strokes < 255) strokes++;
      if (blowing(e) && !blowing(st) && !tail && blows < 255) blows++;
    }
    if (e.mode == GecoBus::BURNING && !inBurn) {
      if (burns < 255) burns++;
      if (!(e.outputs & GecoBus::DHW_PUMP) && marks < 12) marked[marks++] = e.time;
    }
    if (e.mode == GecoBus::BURNING || e.mode == GecoBus::HOLDING) inBurn = e.mode == GecoBus::BURNING;
    st = e;
  };
  gecoLog.forRange(from, until, take);
  for (const GecoBus::Event& e : gecoPending)
    if (e.time < until) take(e);
  while (start + 3600 <= until) {
    run(36000);
    close();
  }
  if (partial && start < until) {
    run((until - start) * 10);
    close();
  }
}

// The first hour not in workLog yet: the one after the newest stored, or the hour the controller's log begins in;
// 0 = the controller has never been heard.
uint32_t workFrom() {
  if (workDone) return workDone + 3600;
  uint32_t first = gecoLog.oldestTime();
  if (!first && !gecoPending.empty()) first = gecoPending.front().time;
  return first ? first - first % 3600 : 0;
}

// The controller's settings as its work shows them - user parameters never appear on the bus: u1 = the feeder's pause
// while burning, u2 = from the feeder stopping in podtrzymanie (or podtrzymanie beginning without it) to the next
// przedmuch, the after-run = how long the fan outlasts the feeder in podtrzymanie. Fed the changes in time order; told
// gets a setting that moved (value = old x 256 + new) and each visit to the service menu (value = seconds, at = when it
// began). The first value seen of a setting is only noted. A misreading costs a line in the journal, nothing more.
class SettingWatch {
public:
  enum Kind : uint8_t { U1, U2, AFTER_RUN, MENU, KINDS = MENU };
  uint16_t known[KINDS] = {};                                // 0 = not seen yet
  std::function<void(uint8_t kind, uint16_t value, uint32_t at)> told;

  void feed(const GecoBus::Event& e) {
    int64_t t = e.time * 10LL + e.tenths;
    if (started) step(last, e, t);
    last = e;
    started = true;
  }

private:
  GecoBus::Event last = {};
  bool    started = false;
  int64_t feederOff = 0, anchor = 0, fanAlone = 0, menuAt = 0, menuSeen = 0;   // tenths of a second, 0 = none
  uint16_t pauses[64];
  uint8_t pauseN = 0;

  void settle(uint8_t k, uint32_t value, uint32_t at, int tolerance) {
    value = min<uint32_t>(value, 255);
    if (!known[k]) {
      known[k] = value;
    } else if (abs((int)value - (int)known[k]) >= tolerance) {
      uint16_t old = known[k];
      known[k] = value;
      if (told) told(k, old * 256 + value, at);
    }
  }

  void step(const GecoBus::Event& p, const GecoBus::Event& e, int64_t t) {
    bool feedNow = e.outputs & GecoBus::FEEDER, feedWas = p.outputs & GecoBus::FEEDER;
    bool fanNow = e.outputs & GecoBus::FAN, fanWas = p.outputs & GecoBus::FAN;
    bool held = p.mode == GecoBus::HOLDING, holding = e.mode == GecoBus::HOLDING, burned = p.mode == GecoBus::BURNING;
    if (burned && e.mode == GecoBus::BURNING) {
      if (feedWas && !feedNow) feederOff = t;
      else if (!feedWas && feedNow && feederOff && pauseN < 64) pauses[pauseN++] = t - feederOff;
    } else if (burned) {
      if (pauseN >= 5) {
        std::sort(pauses, pauses + pauseN);
        settle(U1, (pauses[pauseN / 2] + 5) / 10, e.time, 1);
      }
      pauseN = 0;
      feederOff = 0;
    }
    if (!holding) {
      anchor = 0;
    } else if (!held) {
      anchor = burned && !feedNow ? t : 0;                   // after the menu or a quiet bus its timer is not known
    } else if (feedWas && !feedNow) {
      anchor = t;
    } else if (!feedWas && feedNow) {
      uint32_t m = anchor ? (t - anchor + 300) / 600 : 0;
      if (m >= 5 && m <= 250) settle(U2, m, e.time, 1);
      anchor = 0;
    }
    if (!holding) {
      fanAlone = 0;
    } else if (fanNow && !feedNow && (burned || (held && feedWas))) {
      fanAlone = t;
    } else if (held && fanWas && !fanNow && fanAlone) {
      settle(AFTER_RUN, (t - fanAlone + 5) / 10, e.time, 2);
      fanAlone = 0;
    }
    if (e.mode == GecoBus::MENU) {
      if (!menuAt) menuAt = t;
      menuSeen = t;
    } else if (menuAt && e.mode != GecoBus::NO_DATA) {
      int64_t end = t - menuSeen < 1200 ? t : menuSeen;      // back after 2 minutes of silence: a power cut, not the menu
      if (told && end - menuAt >= 30) told(MENU, min<int64_t>((end - menuAt) / 10, 32767), menuAt / 10);   // not the blink on the way out
      menuAt = 0;
    }
  }
};
SettingWatch settingWatch;

void saveSettingsSeen() {
  prefs.putUShort("gU1", settingWatch.known[SettingWatch::U1]);
  prefs.putUShort("gU2", settingWatch.known[SettingWatch::U2]);
  prefs.putUShort("gAft", settingWatch.known[SettingWatch::AFTER_RUN]);
}

// Once (v31) the settings and the menu visits in the log kept so far go into the journal, each in its place; from then
// on the changes as they come, and what was seen last comes back from flash at every start.
void beginSettingWatch() {
  settingWatch.known[SettingWatch::U1] = prefs.getUShort("gU1", 0);
  settingWatch.known[SettingWatch::U2] = prefs.getUShort("gU2", 0);
  settingWatch.known[SettingWatch::AFTER_RUN] = prefs.getUShort("gAft", 0);
  if (GECO_RX_PIN >= 0 && !prefs.getUChar("gScan", 0) && !updater.crashLoop()) {
    std::vector<EventRecord> found;
    settingWatch.told = [&](uint8_t kind, uint16_t value, uint32_t at) { found.push_back({at, (uint8_t)EV_SETTING, kind, (int16_t)value}); };
    gecoLog.forEach(0, [&](const GecoBus::Event& e) { settingWatch.feed(e); });
    if (!found.empty()) events.edit([&](std::vector<EventRecord>& all) {
      all.insert(all.end(), found.begin(), found.end());
      std::stable_sort(all.begin(), all.end(), [](const EventRecord& a, const EventRecord& b) { return a.time < b.time; });
    });
    saveSettingsSeen();
    prefs.putUChar("gScan", 1);
    techLog.printf("Controller settings: %u changes found back in the log\n", (unsigned)found.size());
  }
  settingWatch.told = [](uint8_t kind, uint16_t value, uint32_t at) {
    logEvent(EV_SETTING, kind, (int16_t)value, at);
    if (kind != SettingWatch::MENU) saveSettingsSeen();
  };
}

// The heat model's degree-minutes of an hour while the controller was heard, by the hour's heard share - stored with
// the hour (v32's apps took the feeder's share off a day's heat with it; v33 counts coal in strokes alone). span: the
// seconds the hour covers (the one running now: so far).
uint16_t heatHeardOf(const WorkHour& w, uint32_t span) {
  if (!w.heard || !span || FURNACE < 0 || RETURN_WATER < 0 || !minutes.newest()) return 0;
  float h = 0;
  minutes.forEach(w.time / 60, (w.time + span - 1) / 60, [&](uint32_t, const MinuteRecord& r) { h += heatOf(r); });
  return (uint16_t)min(65535L, lroundf(h * fminf(1, w.heard / (float)span)));
}

// Once (v30): v29 did not close the log at a start, so the state before a power cut ran on through it. Each power cut
// in the journal gets the NO_DATA record closeGecoLog writes now, where the log has none.
void closeOldCuts() {
  if (GECO_RX_PIN < 0 || prefs.getUChar("gwCuts", 0)) return;
  std::vector<GecoBus::Event> cuts;
  uint32_t first = gecoLog.oldestTime();
  if (first) events.forEach(first, [&](const EventRecord& e) {
    uint32_t off = (uint32_t)(e.sensor & 0x7F) << 16 | (uint16_t)e.value;
    if (e.type == EV_POWER_CUT && off < e.time) cuts.push_back({e.time - off, 0, GecoBus::NO_DATA, 0, 0});
  });
  if (!cuts.empty()) gecoLog.edit([&](std::vector<GecoBus::Event>& all) {
    for (const GecoBus::Event& c : cuts) {
      auto at = std::upper_bound(all.begin(), all.end(), c.time, [](uint32_t t, const GecoBus::Event& e) { return t < e.time; });
      if (at != all.begin() && (at - 1)->mode != GecoBus::NO_DATA) all.insert(at, c);
    }
  });
  prefs.putUChar("gwCuts", 1);
}

// Every start closes the controller's log at the last "still alive" mark before it (a power cut, an update, a
// crash): the state the log ended in must not run on through the time nobody listened. The first frame heard after
// the clock is set opens it again (logGecoChanges).
void closeGecoLog() {
  if (GECO_RX_PIN < 0) return;
  GecoBus::Event last = gecoLog.newest();
  if (!last.time || last.mode == GecoBus::NO_DATA) return;
  uint32_t seen = alive.newest() ? alive.newest() + ALIVE_INTERVAL_S / 2 : minutes.newest() ? minutes.newest() * 60 + 30 : 0;
  GecoBus::Event closed = {max(seen, last.time + 1), 0, GecoBus::NO_DATA, 0, 0};
  gecoLog.add(closed);
}

// Stores the hours the controller's log has finished: at the first pass all of them since the newest stored (or
// since the log began), then each hour a moment after it ends (WORK_DELAY_S). The first pass with the clock recounts the coal - the count made
// at boot had no clock and missed these hours - and every new hour scales the feed factor anew while it is not
// learnt yet.
void logWorkHours(time_t now) {
  static bool counted = false;
  if (GECO_RX_PIN < 0 || !clockSet(now) || updater.crashLoop()) return;
  uint32_t hour = (now - WORK_DELAY_S) - (now - WORK_DELAY_S) % 3600, from = workFrom();
  bool added = from && from < hour;
  if (added) {
    logGecoChanges();                                       // the changes still queued belong to the hours being closed
    std::vector<WorkHour> done;
    countWork(from, hour, false, [&](const WorkHour& w) {
      done.push_back(w);
      done.back().heatHeard = heatHeardOf(w, 3600);
    });
    workLog.addMany(done.data(), done.size());
    workDone = hour - 3600;
  }
  if (!counted) {
    counted = true;
    recountCoal();
  }
}

// ------------------------------------------------------------------ coal

// Heat the furnace gives off in one minute, in degrees: furnace minus return water (0 without both readings) - for
// the day summaries and the charts. The coal does not lean on it (v33): the water says too little about the coal.
float heatOf(const MinuteRecord& r) {
  if (FURNACE < 0 || RETURN_WATER < 0) return 0;
  int16_t f = r.v[FURNACE], w = r.v[RETURN_WATER];
  return f == NO_VALUE || w == NO_VALUE || f <= w ? 0 : (f - w) / 100.0f;
}

// The piston strokes of [from, to) and the seconds the controller was heard: the stored hours, then those not stored
// yet - the hour running now with its part so far; an hour partly inside counts by its part. Coal leaves the hopper
// only through the feeder, and every cycle of the piston pushes the same coal. The time the controller was not heard
// counts nothing: it was off, and so was its feeder (a power cut, a reset at refilling).
CoalUse useBetween(uint32_t from, uint32_t to) {
  CoalUse u;
  uint32_t now = time(nullptr), limit = min(to, now);
  if (limit <= from) return u;
  u.span = limit - from;
  std::vector<WorkHour> hours;
  uint32_t h0 = from - from % 3600, next = workFrom();
  workLog.forRange(h0, to, [&](const WorkHour& w) { hours.push_back(w); });
  size_t stored = hours.size();
  if (next && clockSet(now) && max(h0, next) < limit)
    countWork(max(h0, next), limit, true, [&](const WorkHour& w) { hours.push_back(w); });
  for (size_t k = 0; k < hours.size(); k++) {
    uint32_t t = hours[k].time, span = k < stored ? 3600 : (limit > t ? min<uint32_t>(3600, limit - t) : 0);
    uint32_t lo = max(from, t), hi = min(to, t + span);
    if (!span || hi <= lo) continue;
    float part = (float)(hi - lo) / span;
    u.strokes += hours[k].strokes * part;
    u.heard += hours[k].heard * part;
  }
  return u;
}

// Kilograms a CoalUse stands for: its strokes at the learnt kg per stroke; NAN until that is learnt.
float burnedOf(const CoalUse& u) {
  return isnan(strokeKg) ? NAN : strokeKg * u.strokes;
}

// Seconds of [from, to) without power, from the journal (EV_POWER_CUT: time = the power back, its length in it).
uint32_t cutSecondsIn(uint32_t from, uint32_t to) {
  uint32_t sum = 0;
  events.forEach(from, [&](const EventRecord& e) {
    if (e.type != EV_POWER_CUT) return;
    uint32_t len = (uint32_t)(e.sensor & 0x7F) << 16 | (uint16_t)e.value, a = e.time > len ? e.time - len : 0;
    uint32_t lo = max(a, from), hi = min(e.time, to);
    if (hi > lo) sum += hi - lo;
  });
  return sum;
}

// The strokes of [from, to), the time the controller was not heard while there was power charged at the pace: a
// dropout on the bus or a silent controller must not stop the count - more coal gone, never less. Without a pace,
// the strokes alone.
float strokesCharged(uint32_t from, uint32_t to) {
  CoalUse u = useBetween(from, to);
  float unheard = u.span - u.heard - (float)cutSecondsIn(from, to);
  return u.strokes + (!isnan(coalPace) && unheard > 0 ? unheard / 3600 * coalPace : 0);
}

// All refills, oldest first - one entered later for an earlier hour lands in its place. One sent twice (the app
// retrying through the relay: the same time, kg, fill and tag) counts once.
std::vector<RefillRecord> sortedRefills() {
  std::vector<RefillRecord> v;
  refills.forEach(0, [&](const RefillRecord& r) {
    for (const RefillRecord& o : v)
      if (o.time == r.time && o.kg10 == r.kg10 && o.full == r.full && o.reserved == r.reserved) return;
    v.push_back(r);
  });
  std::stable_sort(v.begin(), v.end(), [](const RefillRecord& a, const RefillRecord& b) { return a.time < b.time; });
  return v;
}

// The flue's peak, the burning and the feeding in each STARVE_WINDOW_S window of [from, to), the windows counted from
// `from`: the minute log's flue, the controller's log (and its changes not on flash yet) for the rest. relit: the last
// time in it the controller came back from standby or silence (0 = none). Empty without a flue probe.
std::vector<FlueWindow> flueWindows(uint32_t from, uint32_t to, uint32_t* relit) {
  std::vector<FlueWindow> w;
  if (relit) *relit = 0;
  if (FLUE < 0 || to <= from) return w;
  w.resize((to - from + STARVE_WINDOW_S - 1) / STARVE_WINDOW_S);
  auto slot = [&](double t) -> FlueWindow* { return t >= from && t < to ? &w[(uint32_t)(t - from) / STARVE_WINDOW_S] : nullptr; };
  if (minutes.newest())
    minutes.forEach(from / 60, (to - 1) / 60, [&](uint32_t m, const MinuteRecord& r) {
      FlueWindow* x = slot(m * 60.0);
      if (!x || r.v[FLUE] == NO_VALUE) return;
      float f = r.v[FLUE] / 100.0f;
      if (isnan(x->peak) || f > x->peak) x->peak = f;
    });
  GecoBus::Event st = gecoLog.forRange(from, from, [](const GecoBus::Event&) {});
  double began = from;                                      // when the state `st` began (or `from`)
  auto spread = [&](double a, double b, bool feeding) {     // [a, b) of the state `st`, its seconds by window
    a = max(a, (double)from);
    b = min(b, (double)to);
    while (a < b) {
      FlueWindow* x = slot(a);
      if (!x) return;
      double end = min(b, from + (floor((a - from) / STARVE_WINDOW_S) + 1) * STARVE_WINDOW_S);
      (feeding ? x->feed : x->burn) += end - a;
      a = end;
    }
  };
  auto state = [&](double a, double b) {
    if (st.mode == GecoBus::NO_DATA) return;
    if (st.outputs & GecoBus::FEEDER) spread(a, b, true);
    if (st.mode == GecoBus::BURNING) spread(a, b, false);
  };
  auto take = [&](const GecoBus::Event& e) {
    double t = e.time + e.tenths / 10.0;
    if (t >= began) {
      state(began, t);
      if (st.mode != GecoBus::NO_DATA && e.mode != GecoBus::NO_DATA && (st.flags & GecoBus::PISTON_AWAY) &&
          !(e.flags & GecoBus::PISTON_AWAY))
        if (FlueWindow* x = slot(t)) x->strokes++;
      if (relit && (e.mode == GecoBus::BURNING || e.mode == GecoBus::HOLDING) &&
          (st.mode == GecoBus::STANDBY || st.mode == GecoBus::NO_DATA))
        *relit = e.time;
      began = t;
    }
    st = e;
  };
  gecoLog.forRange(from, to, take);
  for (const GecoBus::Event& e : gecoPending)
    if (e.time < to) take(e);
  state(began, min<uint32_t>(to, time(nullptr)));
  return w;
}

// A window judged: the controller burned in it and fed coal.
bool fedWindow(const FlueWindow& x) {
  return x.burn >= STARVE_BURN_S && (x.strokes || x.feed >= 20);
}

// The usual peak: the median of the peaks of the judged windows among the first `count`; NAN with fewer than six.
float usualPeak(const std::vector<FlueWindow>& w, size_t count) {
  std::vector<float> p;
  for (size_t k = 0; k < count && k < w.size(); k++)
    if (fedWindow(w[k]) && !isnan(w[k].peak)) p.push_back(w[k].peak);
  if (p.size() < 6) return NAN;
  std::nth_element(p.begin(), p.begin() + p.size() / 2, p.end());
  return p[p.size() / 2];
}

bool starvedWindow(const FlueWindow& x, float usual) {
  return fedWindow(x) && !isnan(x.peak) && !isnan(usual) && x.peak < STARVE_SHARE * usual;
}

// The strokes before a refill at t into an empty hopper that pushed no coal: the last run of two or more starved
// windows in the STARVE_LOOKBACK_S before t (windows not judged neither end nor make one; the refill may be entered a
// while after it happened, so judged windows may follow) - its strokes, from its first window to its last. They are no
// coal of the interval ending at t. 0 without a flue probe or the logs that far back.
float starvedStrokes(uint32_t t) {
  uint32_t a = t - STARVE_LOOKBACK_S - STARVE_USUAL_S;
  std::vector<FlueWindow> w = flueWindows(a, t, nullptr);
  size_t first = STARVE_USUAL_S / STARVE_WINDOW_S;            // the windows before it make the usual peak
  float usual = usualPeak(w, first);
  int runStart = -1, runEnd = -1, start = -1, last = -1, n = 0;
  for (size_t k = first; k < w.size(); k++) {
    if (starvedWindow(w[k], usual)) {
      if (start < 0) start = k;
      last = k;
      n++;
    } else if (fedWindow(w[k])) {
      if (n >= 2) { runStart = start; runEnd = last; }
      start = last = -1;
      n = 0;
    }
  }
  if (n >= 2) { runStart = start; runEnd = last; }
  float strokes = 0;
  for (int k = runStart; k >= 0 && k <= runEnd; k++) strokes += w[k].strokes;
  return strokes;
}

// Every 10 minutes: do the burns still get coal? The two half hours just gone both burned and fed and both peaked below
// STARVE_SHARE of the day's usual: no - the hopper is empty or the coal hangs (starvedSince = when it began; the usual
// is kept from then). Not judged within STARVE_RELIT_S of the controller coming back from standby or silence. Over
// once a half hour heats the flue again, or with a refill (its first hour is not judged).
void watchStarving(uint32_t now) {
  bool fresh = lastRefill.time && now < lastRefill.time + 2 * STARVE_WINDOW_S;
  if (FLUE < 0 || GECO_RX_PIN < 0 || !clockSet(now) || fresh) {
    starvedSince = 0;
    starvedUsual = NAN;
    return;
  }
  uint32_t relit = 0;
  std::vector<FlueWindow> w = flueWindows(now - 2 * STARVE_WINDOW_S - STARVE_USUAL_S, now, &relit);
  size_t n = w.size();
  if (n < 2) return;
  const FlueWindow &a = w[n - 2], &b = w[n - 1];
  if (!starvedSince) {
    float usual = usualPeak(w, n - 2);
    if (now >= relit + STARVE_RELIT_S && starvedWindow(a, usual) && starvedWindow(b, usual)) {
      starvedSince = now - 2 * STARVE_WINDOW_S;
      starvedUsual = usual;
      techLog.printf("Coal: the burns stopped heating the flue (%.0f, %.0f C against %.0f usual)\n", a.peak, b.peak, usual);
    }
  } else if (fedWindow(b) && !isnan(b.peak) && b.peak >= STARVE_SHARE * starvedUsual) {
    starvedSince = 0;
    starvedUsual = NAN;
    techLog.printf("Coal: the flue heats again (%.0f C)\n", b.peak);
  }
}

// Learns the kg one piston stroke pushes from the latest COAL_INTERVALS intervals between refills to the top that the
// controller's hours cover (burned = everything put in after the first, the closing refill included; the strokes
// pushed = all but those after the hopper ran empty, when the closing refill says it was). The hopper's size is the
// setting. Then where its content was last known: the last refill to the top, or
// a later one saying how full it was - the lower of what it says and what the count makes of it. At boot, per refill
// and settings change. A learnt factor outlives its intervals (summer, a long gap) in flash; without two refills to
// the top it goes.
void recomputeCoal() {
  uint32_t hoursStart = workLog.oldestTime();
  if (!hoursStart) hoursStart = workFrom() ? workFrom() : UINT32_MAX;
  struct Span { uint32_t from, to; float put; bool empty; };
  std::vector<RefillRecord> all = sortedRefills();
  std::vector<Span> spans;                                  // between refills to the top, oldest first
  uint16_t fulls = 0;
  uint32_t prevFull = 0;
  float added = 0;
  for (const RefillRecord& r : all) {
    float kg = r.kg10 / 10.0f;
    if (!r.isFull()) {
      added += kg;
      continue;
    }
    fulls++;
    if (prevFull) spans.push_back({prevFull, r.time, added + kg, r.before() == 1});
    prevFull = r.time;
    added = 0;
  }
  lastFullAt = prevFull;
  lastRefill = all.empty() ? RefillRecord{} : all.back();
  coalCap = settings.hopperKg > 0 ? settings.hopperKg : NAN;
  float put = 0, pushed = 0;
  uint16_t used = 0;
  for (size_t i = spans.size(); i-- > 0 && used < COAL_INTERVALS;) {
    const Span& sp = spans[i];
    if (sp.from < hoursStart) break;                         // the hours do not cover it - nor any before it
    homeSpan.resetWatchdog();
    CoalUse u = useBetween(sp.from, sp.to);
    float strokes = u.strokes - (sp.empty ? starvedStrokes(sp.to) : 0);
    if (strokes < COAL_MIN_STROKES || u.heard < COAL_MIN_HEARD * (sp.to - sp.from)) continue;
    put += sp.put;
    pushed += strokes;
    used++;
  }
  if (used && put > 0) {
    strokeKg = put / pushed;
    strokeSamples = used;
    prefs.putFloat("strkKg", strokeKg);
    prefs.putUShort("strkN", strokeSamples);
  } else if (fulls < 2) {
    strokeKg = NAN;
    strokeSamples = 0;
    prefs.remove("strkKg");
    prefs.remove("strkN");
  }
  anchorAt = 0;
  anchorKg = NAN;
  addedSince = 0;
  if (isnan(coalCap)) return;
  for (const RefillRecord& r : all) {
    float kg = r.kg10 / 10.0f;
    if (r.isFull()) {
      anchorAt = r.time;
      anchorKg = coalCap;
      addedSince = 0;
    } else if (r.before()) {
      float said = (r.before() - 1) * 0.25f * coalCap;
      if (anchorAt && !isnan(strokeKg)) {
        float counted = anchorKg + addedSince - strokeKg * useBetween(anchorAt, r.time).strokes;
        said = fminf(said, fmaxf(counted, 0));
      }
      anchorAt = r.time;
      anchorKg = fminf(coalCap, said + kg);
      addedSince = 0;
    } else if (anchorAt) {
      addedSince += kg;
    }
  }
}

// Hopper level, burning pace and time left, the coal since the ash pan was emptied, and whether the burns still get
// coal - every 10 minutes, after a refill and after a settings change. The pace is strokes per hour heard, the busier
// of the last day and the last three: a colder day shows at once, a calm one does not stretch the time left.
void updateCoal(uint32_t now) {
  coalPace = coalRate = coalLevel = coalHoursLeft = ashBurned = NAN;
  if (!clockSet(now)) return;
  watchStarving(now);
  if (!settings.coalOn || isnan(strokeKg)) return;
  CoalUse day = useBetween(now - 86400, now), days = useBetween(now - 3 * 86400, now);
  if (day.heard >= 6 * 3600) coalPace = day.strokes * 3600 / day.heard;
  if (days.heard >= 24 * 3600) {
    float p = days.strokes * 3600 / days.heard;
    coalPace = isnan(coalPace) ? p : fmaxf(coalPace, p);
  }
  if (!isnan(coalPace)) coalRate = coalPace * strokeKg;
  if (!anchorAt || isnan(anchorKg) || isnan(coalCap)) return;
  float left = anchorKg + addedSince - strokeKg * strokesCharged(anchorAt, now);
  coalLevel = constrain(left, 0.0f, coalCap);
  if (!isnan(coalRate)) coalHoursLeft = coalRate > 0.01f ? coalLevel / coalRate : INFINITY;
}

// Baths and showers: hot water drawn makes the boiler drop BATH_DROP degrees within BATH_WINDOW minutes. Fed the
// boiler once a minute - live, and once over the minute log (rescanHistory); a gap starts over. feed() is true when a
// draw of BATH_MIN_MINUTES or more has ended (the boiler rising again, or flat for 10 minutes): from = when the fall
// began, drop = how far it fell, minutes = how long it took. A new draw starts only on a falling boiler and looks
// back no further than the last one's end. Information only - no notification.
class BathWatch {
public:
  time_t  from    = 0;
  float   drop    = 0;
  uint8_t minutes = 0;

  bool feed(time_t t, float b) {
    if (isnan(b) || t - last > 120) n = following = 0;
    last = t;
    if (isnan(b)) return false;
    memmove(ring + 1, ring, BATH_WINDOW * sizeof(float));
    ring[0] = b;
    if (n <= BATH_WINDOW) n++;
    if (!following) {
      if (n < 2 || b >= ring[1]) return false;
      uint8_t topAgo = 0;
      top = NAN;
      for (uint8_t k = 1; k < n && t - k * 60 > ended; k++)
        if (isnan(top) || ring[k] > top) {
          top = ring[k];
          topAgo = k;
        }
      if (isnan(top) || top - b < BATH_DROP) return false;
      following = t - topAgo * 60;
      low = b;
      lowAt = t;
    } else if (b < low) {
      low = b;
      lowAt = t;
    } else if (b >= low + 0.3f || t - lowAt >= 10 * 60) {
      uint32_t m = (lowAt - following) / 60;
      from = following;
      drop = top - low;
      minutes = m > 255 ? 255 : m;
      following = 0;
      ended = t;
      return minutes >= BATH_MIN_MINUTES;
    }
    return false;
  }

private:
  float   ring[BATH_WINDOW + 1];                         // the boiler, ring[k] = k minutes ago
  uint8_t n = 0;                                         // readings in the ring
  time_t  last = 0, following = 0, lowAt = 0, ended = 0; // following: the draw's start, 0 = none
  float   top = NAN, low = NAN;
};
BathWatch bathWatch;

// Switches the heating season (from the web app, or on by itself when the furnace burns in summer mode): the
// furnace and return alarms are judged again under the new limits, and the journal gets the change.
void setHeating(bool on, bool automatic) {
  if (settings.heating == on) return;
  settings.heating = on;
  prefs.putBool("heating", on);
  logEvent(EV_SEASON, automatic, on);
  techLog.printf("Heating season %s%s\n", on ? "on" : "off (summer mode)", automatic ? " - furnace burning" : "");
  if (FURNACE >= 0) rejudgeAlarm(FURNACE);
  if (RETURN_WATER >= 0) rejudgeAlarm(RETURN_WATER);
  evaluateWarnings();
}

// One reading cycle: probes -> HomeKit -> minute log -> trends -> warnings.
void readAll() {
  float values[SENSOR_COUNT];
  MinuteRecord rec;
  String line = "Readings:";
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    if (!slotUsed(i)) {                      // an empty or switched-off slot: nothing read, nothing wrong
      values[i] = NAN;
      rec.v[i] = NO_VALUE;
      continue;
    }
    float t = readSensor(i);
    if (!isnan(t)) t += settings.offset[i];          // calibration: everything downstream sees the corrected value
    bool ok = !isnan(t) && registerReading(i, t);
    if (isnan(t)) registerBadReading(i);
    values[i] = ok ? t : NAN;
    rec.v[i] = toHundredths(values[i]);
    reportSensor(i);
    line += ' ';
    line += isnan(values[i]) ? String('-') : String(values[i], 2);
  }
  Serial.println(line);

  time_t now = time(nullptr);
  healthTick(now);
  if (clockSet(now)) minutes.add(now / 60, rec);

  float furnace = FURNACE >= 0 ? values[FURNACE] : NAN;
  // The first reading after a power cut: was the furnace burning? Then watch whether it comes back (WARN_STALLED).
  if (stallWatch && isnan(stallFrom) && !isnan(furnace)) {
    stallFrom = furnace;
    if (furnace < burnThreshold()) stallWatch = false;
  }
  // Summer mode ends by itself once the furnace burns for SEASON_AUTO_MIN in a row - more alarms, never fewer.
  burningMinutes = !isnan(furnace) && furnace >= burnThreshold() ? min(burningMinutes + 1, 60000) : 0;
  if (!settings.heating && burningMinutes >= SEASON_AUTO_MIN) setHeating(true, true);
  pushRecent(values);
  if (BOILER >= 0 && clockSet(now) && bathWatch.feed(now, values[BOILER])) {
    logEvent(EV_BATH, bathWatch.minutes, toHundredths(bathWatch.drop), bathWatch.from);
    techLog.printf("Bath: boiler -%.1f in %u min\n", bathWatch.drop, bathWatch.minutes);
  }
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
    if (FURNACE >= 0 && r.v[FURNACE] != NO_VALUE && r.v[FURNACE] / 100.0f >= threshold) burn++;
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
// The controller's work log ("/gw"): every change of mode, fan, feeder, piston and pumps, to a tenth of a second,
// from GecoBus's task to flash in batches - a power cut loses at most the last minute. Changes from before the clock
// was set have no time; the state of the moment the clock is set is logged instead. /api/geco and countWork read the
// changes not on flash yet (gecoPending) too.
void logGecoChanges() {
  std::vector<GecoBus::Event>& batch = gecoPending;
  static uint32_t flushedMs = 0;
  static bool clockLogged = false;
  GecoBus::Event e;
  while (geco.nextEvent(e))
    if (clockSet(e.time)) {
      batch.push_back(e);
      settingWatch.feed(e);
    }
  time_t now = time(nullptr);
  if (!clockLogged && clockSet(now) && geco.lastFrame()) {
    e = geco.state();
    e.time = now;
    e.tenths = 0;
    batch.push_back(e);
    settingWatch.feed(e);
    clockLogged = true;
  }
  if (batch.empty() || (batch.size() < 64 && millis() - flushedMs < 60000)) return;
  gecoLog.addMany(batch.data(), batch.size());
  batch.clear();
  flushedMs = millis();
}

void logHistory() {
  time_t now = time(nullptr);
  if (!clockSet(now)) return;
  if (powerLostAt) logPowerCut(now);
  if (pendingEventCount) flushPendingEvents(now);
  if (!ashAt) {                                   // the ash pan counts from the first time the device has a clock
    ashAt = now;
    prefs.putUInt("ashAt", ashAt);
  }
  if (now - lastAliveMark >= ALIVE_INTERVAL_S) {
    alive.mark(now);
    lastAliveMark = now;
  }
  logWorkHours(now);

  uint32_t slot = now / HISTORY_INTERVAL_S;
  if (slot != lastHistorySlot) {
    lastHistorySlot = slot;
    updateCoal(now);
    for (auto& s : state) {
      if (!s.historyCount || !s.history) continue;
      s.history->add(now, s.historySum / s.historyCount);
      s.historySum = 0;
      s.historyCount = 0;
    }
  }

  morningSummary(now);
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

// The morning summary: yesterday's coal (kg and cost, once the coal use is learnt), burning hours, the
// furnace's low and high and the power cuts, as one notification from MORNING_HOUR - once a day, across restarts
// too. From the daily summary written at midnight; the phone words it (web/sw.js).
void morningSummary(time_t now) {
  static time_t lastLook = 0;                            // once a minute is plenty
  if (now - lastLook < 60) return;
  lastLook = now;
  struct tm lt;
  localtime_r(&now, &lt);
  uint32_t today = localMidnight(now);
  if (lt.tm_hour < MORNING_HOUR || summaryDay == today) return;
  summaryDay = today;
  prefs.putUInt("sumDay", today);
  uint32_t yesterday = localMidnight(today - 12 * 3600);
  DayRecord d = {};
  daily.forEach(yesterday, [&](const DayRecord& r) { if (r.time == yesterday) d = r; });
  if (!d.time) return;                                   // the device was off all day - nothing to tell
  uint16_t cuts = 0;
  uint32_t cutSeconds = 0;
  events.forEach(yesterday, [&](const EventRecord& e) {
    if (e.type != EV_POWER_CUT || e.time >= today) return;
    cuts++;
    cutSeconds += (uint32_t)(e.sensor & 0x7F) << 16 | (uint16_t)e.value;
  });
  String j = String("{\"t\":") + (uint32_t)now + ",\"e\":101,\"d\":" + yesterday + ",\"m\":" + (unsigned)d.burnMinutes;
  float kg = settings.coalOn ? burnedOf(useBetween(yesterday, today)) : NAN;
  if (!isnan(kg)) {
    j += ",\"kg\":" + String(lroundf(kg));
    if (settings.coalPrice > 0) j += ",\"z\":" + String(lroundf(kg * settings.coalPrice / 1000));
  }
  int16_t lo = FURNACE >= 0 ? d.min[FURNACE] : NO_VALUE, hi = FURNACE >= 0 ? d.max[FURNACE] : NO_VALUE;
  if (lo != NO_VALUE) j += ",\"lo\":" + String(lo) + ",\"hi\":" + String(hi);
  relay.alert(j + ",\"pc\":" + cuts + ",\"ps\":" + cutSeconds + "}", "summary");
}

// Coal was counted in buckets up to v21, in kilograms since v22 (the web app turns shovels and buckets into kg).
// Once, with the bucket weight it was all entered with: the refills, the coal warnings in the journal, the ash limit
// and the learnt factor. The hopper starts over at the default 100 kg (the owner's figure for this furnace). In
// steps ("coalKg"), so a power cut halfway converts nothing twice.
void coalToKilograms() {
  uint8_t step = prefs.getUChar("coalKg", 0);
  if (step >= 3) return;
  float kg = prefs.getFloat("bucketKg", 10);
  if (step < 1) {
    refills.update([&](RefillRecord& r) { r.kg10 = min(65535L, lroundf(r.kg10 * kg)); });
    prefs.putUChar("coalKg", 1);
  }
  if (step < 2) {
    events.update([&](EventRecord& e) {
      if ((e.type == EV_WARN_ON || e.type == EV_WARN_OFF) && (e.sensor == WARN_LOW_COAL || e.sensor == WARN_ASH) && e.value != NO_VALUE)
        e.value = toHundredths(upTo320(e.value / 100.0f * kg));
    });
    prefs.putUChar("coalKg", 2);
  }
  if (prefs.isKey("ashLim")) prefs.putFloat("ashKg", prefs.getFloat("ashLim", 15) * kg);
  if (prefs.isKey("coalK")) prefs.putFloat("coalK", prefs.getFloat("coalK", 0) * kg);
  for (const char* key : {"hopBk", "ashLim", "bucketKg"}) prefs.remove(key);
  prefs.putUChar("coalKg", 3);
  loadSettings();
  techLog.printf("Coal: now in kg (a bucket was %.1f kg)\n", kg);
}

// Once (v24): the baths from before their detection, found in the minute log (31 days back), and the "Piec wygasa"
// warnings the old rule raised on a fast fall alone - an ON more than goOutMargin above the min limit goes, with the
// OFF that closed it; so do v23's "baths" shorter than BATH_MIN_MINUTES. The journal is written back in time order.
void rescanHistory() {
  if (prefs.getUChar("rescan", 0)) return;
  std::vector<EventRecord> baths;
  BathWatch watch;
  if (BOILER >= 0 && minutes.newest()) minutes.forEach(minutes.oldest(), minutes.newest(), [&](uint32_t m, const MinuteRecord& r) {
    if (watch.feed(m * 60, r.v[BOILER] == NO_VALUE ? NAN : r.v[BOILER] / 100.0f))
      baths.push_back({(uint32_t)watch.from, (uint8_t)EV_BATH, watch.minutes, toHundredths(watch.drop)});
  });
  float low = FURNACE >= 0 ? state[FURNACE].low : NAN;
  uint16_t added = 0, dropped = 0;
  events.edit([&](std::vector<EventRecord>& all) {
    std::vector<EventRecord> keep;
    bool closeNext = false;                              // the OFF of a dropped ON goes too
    for (const EventRecord& e : all) {
      bool goingOut = (e.type == EV_WARN_ON || e.type == EV_WARN_OFF) && e.sensor == WARN_GOING_OUT;
      if (goingOut && e.type == EV_WARN_ON) {
        closeNext = !isnan(low) && e.value != NO_VALUE && e.value / 100.0f > low + settings.goOutMargin;
        if (closeNext) {
          dropped++;
          continue;
        }
      } else if (goingOut && closeNext) {
        closeNext = false;
        continue;
      }
      if (e.type == EV_BATH && e.sensor < BATH_MIN_MINUTES) continue;
      keep.push_back(e);
    }
    for (const EventRecord& b : baths) {                 // a bath v23 logged live stays as it is
      bool known = false;
      for (const EventRecord& e : keep)
        if (e.type == EV_BATH && e.time + 1800 > b.time && e.time < b.time + 1800) known = true;
      if (!known) {
        keep.push_back(b);
        added++;
      }
    }
    for (size_t i = 1; i < keep.size(); i++)            // nearly in order already: an insertion sort is enough
      for (size_t j = i; j && keep[j].time < keep[j - 1].time; j--) std::swap(keep[j], keep[j - 1]);
    all.swap(keep);
  });
  prefs.putUChar("rescan", 1);
  techLog.printf("History: %u baths found back, %u false going-out warnings dropped\n", added, dropped);
}

// A backup of fewer probes was restored (Backup.h, "fromProbes" - the classic board's 4 into the S3's slots): its
// daily summaries are as wide as it was. Each record gets the missing slots as "no value"; the new file is renamed
// over the old one, so a power cut leaves either.
void widenDays() {
  uint8_t from = prefs.getUChar("fromProbes", 0);
  if (!from) return;
  const size_t oldSize = 4 + 6 * from + 6, N = SENSOR_COUNT;
  int16_t none[SENSOR_COUNT];
  for (auto& v : none) v = NO_VALUE;
  uint8_t done = prefs.getUChar("widened", 0);              // files already widened (a power cut halfway)
  for (uint8_t k = 0; k < 2; k++) {
    String path = "/day" + String(k) + ".bin";
    if (done >> k & 1) continue;
    File f = LittleFS.open(path, "r");
    if (!f || from >= N) {
      if (f) f.close();
      continue;
    }
    std::vector<uint8_t> old(f.size());
    f.read(old.data(), old.size());
    f.close();
    std::vector<DayRecord> wide;
    for (size_t at = 0; at + oldSize <= old.size(); at += oldSize) {
      const uint8_t* r = old.data() + at;
      DayRecord d;
      uint8_t* w = (uint8_t*)&d;
      memcpy(w, r, 4);                                         // time
      for (size_t k = 0; k < 3; k++) {                         // min, avg, max
        memcpy(w + 4 + 2 * N * k, none, 2 * N);
        memcpy(w + 4 + 2 * N * k, r + 4 + 2 * from * k, 2 * from);
      }
      memcpy(w + 4 + 6 * N, r + 4 + 6 * from, 6);              // burnMinutes, heat
      wide.push_back(d);
    }
    String fresh = path + ".new";
    f = LittleFS.open(fresh, "w");
    if (!f) continue;
    f.write((const uint8_t*)wide.data(), wide.size() * sizeof(DayRecord));
    f.close();
    if (!LittleFS.rename(fresh, path)) continue;
    prefs.putUChar("widened", done |= 1 << k);
    techLog.printf("%s: %u days widened from %u probes\n", path.c_str(), (unsigned)wide.size(), from);
  }
  prefs.remove("fromProbes");
  prefs.remove("widened");
}

// wipe: the first start after a full reset - everything goes.
void setupHistory(bool wipe) {
  if (!LittleFS.begin(true)) techLog.println("LittleFS mount failed - no history");
  // Files of an older layout would eat the space of the new ones: a layout change wipes the history once.
  bool newLayout = prefs.getUChar("fsLayout", 0) != FS_LAYOUT;
  if (newLayout || wipe) {
    techLog.println(wipe ? "Full reset - wiping the history" : "History layout changed - formatting LittleFS");
    LittleFS.format();
    prefs.putUChar("fsLayout", FS_LAYOUT);
  }
  widenDays();
  if (!minutes.begin()) techLog.println("No \"minutes\" partition - per-minute history disabled");
  if (wipe) minutes.clear();
  if (!alive.begin()) techLog.println("No \"alive\" partition - power cuts timed from the minute log");
  if (newLayout || wipe) alive.clear();                       // old LittleFS blocks / the last home's marks
  daily.begin();
  events.begin();
  refills.begin();
  health.begin();
  gecoLog.begin();
  for (const char* old : {"/wh0.bin", "/wh1.bin", "/wk0.bin", "/wk1.bin"})   // the hours of v30 and v31: counted anew from the log
    if (LittleFS.exists(old)) LittleFS.remove(old);
  workLog.begin();
  workDone = workLog.newestTime();
  techLog.printf("LittleFS: %u of %u KB used\n", (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
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
    uint8_t role = slots[i].role < ROLE_COUNT ? slots[i].role : ROLE_OTHER;
    state[i].low  = prefs.isKey(lo.c_str()) ? prefs.getFloat(lo.c_str()) : ROLE_LOW[role];
    state[i].high = prefs.isKey(hi.c_str()) ? prefs.getFloat(hi.c_str()) : ROLE_HIGH[role];
  }
}

void loadSettings() {
  Settings d;
  settings.goOutOn      = prefs.getBool("goOn", d.goOutOn);
  settings.goOutMargin  = prefs.getFloat("goMar", d.goOutMargin);
  settings.freezeOn     = prefs.getBool("frOn", d.freezeOn);
  settings.freezeTemp   = prefs.getFloat("frTemp", d.freezeTemp);
  settings.burnbackOn   = prefs.getBool("bbOn", d.burnbackOn);
  settings.burnbackRate = prefs.getFloat("bbRate", d.burnbackRate);
  settings.hotWater     = prefs.getFloat("hotWater", d.hotWater);
  settings.coalOn       = prefs.getBool("coalOn", d.coalOn);
  settings.hopperKg     = prefs.getFloat("hopKg", d.hopperKg);
  settings.coalPrice    = prefs.getFloat("coalPr", d.coalPrice);
  settings.lowCoalOn    = prefs.getBool("lcOn", d.lowCoalOn);
  settings.lowCoalHours = prefs.getFloat("lcHrs", d.lowCoalHours);
  settings.heating      = prefs.getBool("heating", d.heating);
  settings.stalledOn    = prefs.getBool("stallOn", d.stalledOn);
  settings.standbyOn    = prefs.getBool("stbyOn", d.standbyOn);
  settings.ashOn        = prefs.getBool("ashOn", d.ashOn);
  settings.ashLimit     = prefs.getFloat("ashKg", d.ashLimit);
  settings.ashDays      = prefs.getFloat("ashDays", d.ashDays);
  for (size_t i = 0; i < SENSOR_COUNT; i++) settings.offset[i] = prefs.getFloat(prefKey("off", i).c_str(), 0);
  settings.lat          = prefs.getFloat("lat", d.lat);
  settings.lon          = prefs.getFloat("lon", d.lon);
  prefs.getString("place", settings.place, sizeof(settings.place));
}

void saveSettings() {
  prefs.putBool("goOn", settings.goOutOn);
  prefs.putFloat("goMar", settings.goOutMargin);
  prefs.putBool("frOn", settings.freezeOn);
  prefs.putFloat("frTemp", settings.freezeTemp);
  prefs.putBool("bbOn", settings.burnbackOn);
  prefs.putFloat("bbRate", settings.burnbackRate);
  prefs.putFloat("hotWater", settings.hotWater);
  prefs.putBool("coalOn", settings.coalOn);
  prefs.putFloat("hopKg", settings.hopperKg);
  prefs.putFloat("coalPr", settings.coalPrice);
  prefs.putBool("lcOn", settings.lowCoalOn);
  prefs.putFloat("lcHrs", settings.lowCoalHours);
  prefs.putBool("heating", settings.heating);
  prefs.putBool("stallOn", settings.stalledOn);
  prefs.putBool("stbyOn", settings.standbyOn);
  prefs.putBool("ashOn", settings.ashOn);
  prefs.putFloat("ashKg", settings.ashLimit);
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
bool parseLimit(String v, float& out, float top) {
  v.trim();
  if (!v.length()) {
    out = NAN;
    return true;
  }
  float f = parseNumber(v);
  if (isnan(f) || f < -55 || f > top) return false;
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
    techLog.println("Clock set from the web app - no NTP");
  }
  String j;
  j.reserve(2600);
  j += "{\"time\":";     j += (uint32_t)time(nullptr);
  j += ",\"uptime\":";   j += (uint32_t)(millis() / 1000);
  j += ",\"rssi\":";     j += (int)WiFi.RSSI();
  j += ",\"ip\":\"";     j += WiFi.localIP().toString();
  j += "\",\"heap\":";   j += (uint32_t)ESP.getFreeHeap();
  j += ",\"heapBlock\":"; j += (uint32_t)ESP.getMaxAllocHeap();   // a TLS connection needs ~40 KB in one piece
  j += ",\"fsUsed\":";   j += (uint32_t)LittleFS.usedBytes();
  j += ",\"fsTotal\":";  j += (uint32_t)LittleFS.totalBytes();
  j += ",\"oldest\":";   j += minutes.oldest() * 60;
  j += ",\"minuteDays\":"; j += minutes.capacityMinutes() / 1440;
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
  j += ",\"sig\":";       j += (int)updater.signatures();   // int8_t would go in as a character
  j += ",\"sensors\":[";
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    const SensorState& s = state[i];
    if (i) j += ',';
    j += "{\"name\":\"";  j += slots[i].name;
    j += "\",\"role\":\""; j += ROLE_KEYS[slots[i].role < ROLE_COUNT ? slots[i].role : 0];
    j += "\",\"addr\":\""; j += isUnset(slots[i].address) ? String() : hexAddress(slots[i].address);
    j += "\",\"kind\":\""; j += probeKind(slots[i].address);
    j += "\",\"on\":";     j += slotUsed(i) ? "true" : "false";
    j += ",\"errors\":";   j += s.errors;
    j += ",\"temp\":";  j += s.faulty ? String("null") : jsonNumber(s.lastGood);
    j += ",\"trend\":";   j += jsonNumber(trendPerHour(i), 1);
    j += ",\"low\":";     j += jsonNumber(s.low, 1);
    j += ",\"high\":";    j += jsonNumber(s.high, 1);
    j += ",\"alarm\":";   j += (int)s.alarm;
    j += ",\"fault\":";   j += s.faulty ? "true" : "false";
    j += ",\"water\":";   j += isWater(i) ? "true" : "false";
    j += ",\"offset\":";  j += jsonNumber(settings.offset[i], 1);
    for (const Takeover& h : takeovers)          // [slot, time]: the history before that time shown from that slot
      if (takeoverUsed(h) && h.to == i) { j += ",\"took\":["; j += h.from; j += ','; j += h.at; j += ']'; }
    j += '}';
  }
  j += ']';
  if (GECO_RX_PIN >= 0) {                     // the controller as its panel bus tells it (GecoBus.h)
    GecoBus::Event g = geco.state();
    j += ",\"geco\":{\"age\":"; j += geco.lastFrame() ? String((millis() - geco.lastFrame()) / 1000) : String(-1);
    j += ",\"mode\":";  j += (int)g.mode;
    j += ",\"out\":";   j += (int)g.outputs;
    j += ",\"flags\":"; j += (int)g.flags;
    j += ",\"since\":"; j += g.time;
    j += ",\"set\":[";                                       // u1 s, u2 min, the fan's after-run s - as its work shows them
    for (uint8_t k = 0; k < SettingWatch::KINDS; k++) { if (k) j += ','; j += settingWatch.known[k]; }
    j += "],\"t\":[";
    for (uint8_t ch = 1; ch <= 3; ch++) j += (ch > 1 ? "," : "") + jsonNumber(geco.temp(ch), 1);
    j += "],\"good\":"; j += geco.goodFrames();
    j += ",\"bad\":";   j += geco.badFrames();
    j += ",\"bytes\":"; j += geco.rawBytes();             // the connection check (GecoBus.h)
    j += ",\"pin\":";   j += (int)geco.pin();
    j += ",\"inv\":";   j += geco.inverted() ? "true" : "false";
    j += ",\"line\":\"";
    if (geco.line()) j += geco.line();
    j += "\",\"raw\":\""; j += geco.rawHex(); j += '"';
    j += '}';
  }
  j += ",\"warnings\":[";
  for (uint8_t w = 0; w < WARN_COUNT; w++) {
    if (w) j += ',';
    j += warnings[w].active ? "true" : "false";
  }
  j += "],\"forecast\":"; j += goOutForecast < 0 ? String("null") : String(goOutForecast);
  j += ",\"hotWater\":";  j += hotWaterReady ? "true" : "false";
  j += ",\"stallFrom\":"; j += jsonNumber(stallFrom, 1);
  j += ",\"ash\":{\"at\":"; j += ashAt;
  j += ",\"kg\":";        j += jsonNumber(ashBurned, 1);
  j += '}';
  j += ",\"coal\":{\"k\":null,\"strokeKg\":"; j += jsonNumber(strokeKg, 4);   // kg per piston stroke, null = not
                                                    // learnt; k: v32's apps took the water's factor from it
  j += ",\"cap\":";       j += jsonNumber(coalCap, 1);  // kg the hopper holds (the setting)
  j += ",\"strokeN\":";   j += strokeSamples;           // refill-to-refill intervals it was learnt from
  j += ",\"lastFull\":";  j += lastFullAt;
  j += ",\"anchor\":";    j += anchorAt;                // the hopper's content was last known then
  j += ",\"level\":";     j += jsonNumber(coalLevel, 1);
  j += ",\"pace\":";      j += jsonNumber(coalPace, 2); // piston strokes per hour
  j += ",\"rate\":";      j += jsonNumber(coalRate, 2);
  j += ",\"hoursLeft\":"; j += isinf(coalHoursLeft) ? String("1e9") : jsonNumber(coalHoursLeft, 1);
  j += ",\"starved\":";   j += starvedSince;            // the burns stopped heating the flue then, 0 = they heat
  j += ",\"lastRefill\":";
  if (lastRefill.time) {
    j += '[';  j += lastRefill.time;  j += ',';  j += lastRefill.kg10;  j += ',';  j += (int)lastRefill.isFull();
    j += ',';  j += lastRefill.before();  j += ']';
  } else {
    j += "null";
  }
  j += "},\"settings\":{\"goOutOn\":"; j += settings.goOutOn ? "true" : "false";
  j += ",\"goOutMargin\":";  j += jsonNumber(settings.goOutMargin, 1);
  j += ",\"freezeOn\":";     j += settings.freezeOn ? "true" : "false";
  j += ",\"freezeTemp\":";   j += jsonNumber(settings.freezeTemp, 1);
  j += ",\"burnbackOn\":";   j += settings.burnbackOn ? "true" : "false";
  j += ",\"burnbackRate\":"; j += jsonNumber(settings.burnbackRate, 1);
  j += ",\"hotWater\":";     j += jsonNumber(settings.hotWater, 1);
  j += ",\"coalOn\":";       j += settings.coalOn ? "true" : "false";
  j += ",\"hopperKg\":";     j += jsonNumber(settings.hopperKg, 0);
  j += ",\"coalPrice\":";    j += jsonNumber(settings.coalPrice, 0);
  j += ",\"lowCoalOn\":";    j += settings.lowCoalOn ? "true" : "false";
  j += ",\"lowCoalHours\":"; j += jsonNumber(settings.lowCoalHours, 0);
  j += ",\"heating\":";      j += settings.heating ? "true" : "false";
  j += ",\"stalledOn\":";    j += settings.stalledOn ? "true" : "false";
  j += ",\"standbyOn\":";    j += settings.standbyOn ? "true" : "false";
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

// History handed on (v35): a slot given a furnace role in place of a probe switched off (the controller's boiler for
// the "Piec" probe, 12.11) shows that probe's history as its own before the change - where it has nothing of its own,
// or where its own is more than TAKEOVER_DISAGREE off (the controller's 30 C while its bus came and went at a refill,
// 11.11 19:27) - and the old slot shows nothing from before it, so a probe put into it later starts clean. Only how
// /api/series, /api/csv and /api/daily show the history: the logs keep what was measured. Set by h<i> in /api/slots,
// dropped when the slot that took over gets another probe or the history is cleared (Takeover, up with the records).
bool takeoverUsed(const Takeover& h) { return h.to != h.from && h.to < SENSOR_COUNT && h.from < SENSOR_COUNT; }

void loadTakeovers() {
  if (prefs.getBytes("takeover", takeovers, sizeof(takeovers)) != sizeof(takeovers)) memset(takeovers, 0, sizeof(takeovers));
}

// One moment's values (a minute's readings, or a day's min, avg or max) as the history shows them.
void handOn(uint32_t t, int16_t* v) {
  for (const Takeover& h : takeovers) {
    if (!takeoverUsed(h) || t >= h.at) continue;
    int16_t own = v[h.to], old = v[h.from];
    if (old != NO_VALUE && (own == NO_VALUE || abs(own - old) > TAKEOVER_DISAGREE)) v[h.to] = old;
    v[h.from] = NO_VALUE;
  }
}

// Walks the minute log in buckets of `step` seconds and calls f(bucketStart, count, min, sum, max) per bucket
// that has any reading; values are degrees x 100, the history handed on (handOn).
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
    int16_t shown[SENSOR_COUNT];
    for (size_t i = 0; i < SENSOR_COUNT; i++) shown[i] = r.v[i];
    handOn(m * 60, shown);
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
      int16_t v = shown[i];
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
// A range from 0 at a minute step would keep the loop busy for a minute: at most ~20000 buckets an answer.
uint32_t stepFor(uint32_t from, uint32_t to, uint32_t step) {
  uint32_t least = to > from ? (to - from) / 20000 : 0;
  return max(max((uint32_t)60, step / 60 * 60), (least + 59) / 60 * 60);
}

void handleSeries() {
  uint32_t to = argOr("to", time(nullptr)), from = argOr("from", to - 6 * 3600);
  uint32_t step = stepFor(from, to, argOr("step", 60));
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

// The last crash the core dump partition holds: the task, where it stopped and the backtrace - addresses to look up
// in that build's ELF (release/publish.ps1 keeps them, named by the build's ELF SHA prefix shown here).
String lastCrash() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  if (esp_core_dump_image_check() != ESP_OK) return "none stored";
  esp_core_dump_summary_t* s = (esp_core_dump_summary_t*)calloc(1, sizeof(esp_core_dump_summary_t));
  if (!s) return "no memory";
  String r = "unreadable";
  if (esp_core_dump_get_summary(s) == ESP_OK) {
    char line[96];
    snprintf(line, sizeof(line), "build %s, task %s, PC 0x%08lx", (const char*)s->app_elf_sha256, s->exc_task, (unsigned long)s->exc_pc);
    r = line;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    snprintf(line, sizeof(line), ", cause %lu, address 0x%08lx\nbacktrace:", (unsigned long)s->ex_info.exc_cause,
             (unsigned long)s->ex_info.exc_vaddr);
    r += line;
    for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16; i++) {
      snprintf(line, sizeof(line), " 0x%08lx", (unsigned long)s->exc_bt_info.bt[i]);
      r += line;
    }
    if (s->exc_bt_info.corrupted) r += " (corrupted)";
#endif
  }
  free(s);
  return r;
#else
  return "not kept on this board";
#endif
}

// GET: the diagnostic report, plain text for a support request - the device, its health since the start and the
// technical log (TechLog.h). No passwords or keys.
void handleDiag() {
  String r;
  r.reserve(TechLog::SIZE + 800);
  char build[10];
  esp_app_get_elf_sha256(build, sizeof(build));
  r += "Furnace monitor v" + String(FW_VERSION) + " (build " + build + ") on " + ESP.getChipModel() + " (PSRAM " + String(ESP.getPsramSize() / 1048576)
     + " MB), up " + String(millis() / 60000) + " min, started: " + TechLog::startReason();
  r += "\nheap " + String(ESP.getFreeHeap() / 1024) + " KB, lowest " + String(ESP.getMinFreeHeap() / 1024) + " KB, largest block "
     + String(ESP.getMaxAllocHeap() / 1024) + " KB";
  r += "\nWiFi " + String(WiFi.RSSI()) + " dBm, connected " + String(networkUps) + "x; relay " + relay.state() + ", connected "
     + String(relay.connections()) + "x";
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    if (slotUsed(i))
      r += "\nslot " + String(i + 1) + " " + slots[i].name + " (" + ROLE_KEYS[slots[i].role % ROLE_COUNT] + ", "
         + probeKind(slots[i].address) + " " + hexAddress(slots[i].address) + "): "
         + (state[i].faulty ? String("FAULTY") : jsonNumber(state[i].lastGood, 2)) + ", read errors " + String(state[i].errors);
  r += "\nupdate: " + updater.status() + ", signatures " + String(updater.signatures());
  r += "\nsupport code: " + String(relay.supportUntil() > (uint32_t)time(nullptr) ? "active" : "none");
  if (GECO_RX_PIN >= 0) {
    GecoBus::Event g = geco.state();
    r += "\nGeco bus: " + (geco.lastFrame() ? String((millis() - geco.lastFrame()) / 1000) + " s since a frame" : String("never heard"))
       + ", frames " + String(geco.goodFrames()) + " ok / " + String(geco.badFrames()) + " bad, mode 0x" + String(g.mode, HEX)
       + ", outputs 0x" + String(g.outputs, HEX) + ", P3 0x" + String(g.flags, HEX);
  }
  r += "\nlast crash: " + lastCrash();
  r += "\n--- days: WiFi / relay connects, probe errors, failed updates, least RAM KB, longest loop ms, weakest RSSI, hottest chip C, starts";
  time_t now = time(nullptr);
  auto day = [&](const HealthRecord& h, const char* label) {
    char d[12] = "?", line[120];
    time_t t = h.time;
    struct tm lt;
    if (t) {
      localtime_r(&t, &lt);
      strftime(d, sizeof(d), "%Y-%m-%d", &lt);
    }
    snprintf(line, sizeof(line), "\n%s%s  %u / %u  %u  %u  %u  %u  %d  %d  %u", d, label, h.wifiUps, h.relayUps, h.probeErrors,
             h.updateFails, h.heapMinKB == 0xFFFF ? 0 : h.heapMinKB, h.loopMaxMs, h.rssiMin, h.chipMax, h.starts);
    r += line;
  };
  health.forEach(clockSet(now) ? localMidnight(now) - 14 * 86400 : 0, [&](const HealthRecord& h) { day(h, ""); });
  day(healthToday, " (today)");
  r += "\nLittleFS " + String(LittleFS.usedBytes() / 1024) + " of " + String(LittleFS.totalBytes() / 1024) + " KB, minute log "
     + String(minutes.newest() ? (minutes.newest() - minutes.oldest() + 1) / 1440 : 0) + " days\n--- log\n";
  r += techLog.text();
  server.send(200, "text/plain; charset=utf-8", r);
}

// GET since. Binary DayRecords, oldest first, the history handed on (handOn).
void handleDaily() {
  ChunkWriter w;
  w.begin("application/octet-stream");
  daily.forEach(argOr("since", 0), [&](const DayRecord& d) {
    DayRecord x = d;
    for (size_t k = 0; k < 3; k++) {             // min, avg, max lie one after another; copied, as x is packed
      int16_t v[SENSOR_COUNT];
      uint8_t* field = (uint8_t*)&x + offsetof(DayRecord, min) + k * sizeof(v);
      memcpy(v, field, sizeof(v));
      handOn(x.time, v);
      memcpy(field, v, sizeof(v));
    }
    w.put(&x, sizeof(x));
  });
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
  uint32_t step = stepFor(from, to, argOr("step", 60));
  server.sendHeader("Content-Disposition", "attachment; filename=\"piec.csv\"");
  ChunkWriter w;
  w.begin("text/csv; charset=utf-8");
  String head = "\xEF\xBB\xBF" "Czas";
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    if (slotUsed(i)) head += String(";") + slots[i].name;
  w.put(head + "\r\n");
  forEachBucket(from, to, step, [&](uint32_t t, const uint16_t* count, const int16_t*, const int32_t* sum, const int16_t*) {
    time_t tt = t;
    struct tm lt;
    localtime_r(&tt, &lt);
    char line[192];
    size_t n = strftime(line, sizeof(line), "%Y-%m-%d %H:%M", &lt);
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
      if (!slotUsed(i)) continue;
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
  float top = isThermocouple(slots[i].address) ? 1000 : 125;   // what the probe can read
  if (!parseLimit(server.arg("low"), low, top) || !parseLimit(server.arg("high"), high, top) || low >= high) {
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
  number("goOutMargin", settings.goOutMargin, 1, 20);
  flag("freezeOn", settings.freezeOn);
  number("freezeTemp", settings.freezeTemp, -20, 20);
  flag("burnbackOn", settings.burnbackOn);
  number("burnbackRate", settings.burnbackRate, 1, 60);
  number("hotWater", settings.hotWater, 20, 90);
  flag("coalOn", settings.coalOn);
  number("hopperKg", settings.hopperKg, 0, 2000);
  number("coalPrice", settings.coalPrice, 0, 20000);
  flag("lowCoalOn", settings.lowCoalOn);
  number("lowCoalHours", settings.lowCoalHours, 1, 96);
  flag("stalledOn", settings.stalledOn);
  flag("standbyOn", settings.standbyOn);
  flag("ashOn", settings.ashOn);
  number("ashLimit", settings.ashLimit, 5, 2000);
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
  recomputeCoal();                                       // the hopper's size may have changed (it is the level's top)
  if (clockSet(time(nullptr))) updateCoal(time(nullptr));
  evaluateWarnings();
  handleNow();
}

// GET. JSON [[time, kg x 10, full, before], ...] in the order they were entered (the last one can be taken back); one
// sent twice shows once. before: how full the hopper was - 0 not said, 1 empty, 2-4 quarters (RefillRecord::before).
void handleRefills() {
  ChunkWriter w;
  w.begin("application/json");
  w.put("[", 1);
  bool first = true;
  std::vector<RefillRecord> seen;
  refills.forEach(0, [&](const RefillRecord& r) {
    for (const RefillRecord& o : seen)
      if (o.time == r.time && o.kg10 == r.kg10 && o.full == r.full && o.reserved == r.reserved) return;
    seen.push_back(r);
    char line[48];
    int n = snprintf(line, sizeof(line), "%s[%u,%u,%u,%u]", first ? "" : ",", (unsigned)r.time, r.kg10, r.isFull() ? 1 : 0,
                     r.before());
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

// before: how full the hopper was (0 not said, 1 empty, 2-4 quarters); tag: the app's mark of this entry (1-31), so a
// retry through the relay - the same time, kg, fill and tag - is not added twice, two real ones in a minute are.
void addRefill(float kg, bool full, uint8_t before, uint8_t tag, uint32_t t) {
  RefillRecord r = {t, (uint16_t)lroundf(kg * 10), (uint8_t)(full ? 1 : 0), (uint8_t)(before | tag << 3)};
  bool twice = false;
  refills.forEach(0, [&](const RefillRecord& o) {
    twice |= o.time == r.time && o.kg10 == r.kg10 && o.full == r.full && o.reserved == r.reserved;
  });
  if (!twice) refills.add(r);
  recountCoal();
}

// POST kg, full (1/0), before (how full the hopper was: 1 empty, 2-4 a quarter, half, three quarters; 0 or none: not
// said), tag (1-31, the app's mark of the entry), time (Unix; optional - a refill entered later, up to 7 days back).
// Answers /api/now.
void handleRefill() {
  uint32_t now = time(nullptr), t = argOr("time", now), before = argOr("before", 0), tag = argOr("tag", 0);
  float kg = parseNumber(server.arg("kg"));
  if (!clockSet(now) || isnan(kg) || kg < 0.5f || kg > 2000 || t > now + 300 || t + 7 * 86400 < now || before > 4 || tag > 31) {
    server.send(400, "text/plain", "bad refill");
    return;
  }
  addRefill(kg, server.arg("full") == "1", (uint8_t)before, (uint8_t)tag, min(t, now));
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
  logEvent(EV_ASH, 0, 0);
  updateCoal(now);
  evaluateWarnings();
}

// GET from, to (Unix time): the controller's work log - binary, 8 bytes a change (GecoBus::Event), oldest first. The
// first record is the state the range began in (all zero when the log holds nothing older).
void handleGeco() {
  uint32_t to = argOr("to", time(nullptr)) + 1, from = argOr("from", to - 86400);
  ChunkWriter w;
  w.begin("application/octet-stream");
  GecoBus::Event before = gecoLog.forRange(from, from, [](const GecoBus::Event&) {});
  for (const GecoBus::Event& e : gecoPending)
    if (e.time < from) before = e;
  w.put(&before, sizeof(before));
  gecoLog.forRange(from, to, [&](const GecoBus::Event& e) { w.put(&e, sizeof(e)); });
  for (const GecoBus::Event& e : gecoPending)       // newer than anything on flash
    if (e.time >= from && e.time < to) w.put(&e, sizeof(e));
  w.end();
}

// GET from, to (Unix time): the controller's work hour by hour - binary, 30 bytes an hour (WorkHour), oldest first;
// the hours not stored yet come last, the one running now with its part so far.
void handleWork() {
  uint32_t now = time(nullptr), to = argOr("to", now) + 1, from = argOr("from", to - 86400);
  from -= from % 3600;
  ChunkWriter w;
  w.begin("application/octet-stream");
  workLog.forRange(from, to, [&](const WorkHour& h) { w.put(&h, sizeof(h)); });
  uint32_t next = workFrom();
  if (next && clockSet(now) && max(from, next) < min(to, now))
    countWork(max(from, next), min(to, now), true, [&](const WorkHour& h) {
      WorkHour c = h;
      c.heatHeard = heatHeardOf(c, min<uint32_t>(3600, min(to, now) - c.time));
      w.put(&c, sizeof(c));
    });
  w.end();
}

void handleBus() {
  server.send(200, "text/plain", busReport());
}

// GET: every probe found - [{"addr","kind","t","slot"}] (slot -1 = no slot has it). The DS18B20s give what their last
// conversion left and the next one starts here, so the loop never waits the 750 ms; the Czujniki page asks every few
// seconds while it is open, and a probe warmed in the hand shows it by the next answer.
void handleProbes() {
  String j = "[";
  auto add = [&](const DeviceAddress a, float t) {
    if (j.length() > 1) j += ',';
    j += "{\"addr\":\"" + hexAddress(a) + "\",\"kind\":\"" + probeKind(a) + "\",\"t\":" + jsonNumber(t, 2)
       + ",\"slot\":" + slotOf(a) + "}";
  };
  DeviceAddress a;
  oneWire.reset_search();
  while (oneWire.search(a)) {
    if (!bus.validAddress(a) || !bus.validFamily(a)) continue;
    float t = bus.getTempC(a);
    add(a, t == DEVICE_DISCONNECTED_C ? NAN : t);
  }
  DeviceAddress tc;
  if (findThermocouple(tc)) add(tc, readThermocouple());
  for (uint8_t ch = 1; ch <= 3 && geco.fresh(); ch++) {
    DeviceAddress g = {KIND_GECO, ch};
    add(g, geco.temp(ch));
  }
  if (!conversionPending) bus.requestTemperatures();        // the minute's reading has one of its own under way
  server.send(200, "application/json", j + "]");
}

// POST the whole slot table: a<i> address (16 hex digits, empty = an empty slot), n<i> name, r<i> role key, o<i> 1/0
// for every slot. A probe in one slot only, each furnace role once among the slots in use. Saved and answered, then
// the ESP restarts (Home needs the accessories anew).
uint32_t restartAtMs = 0;
void handleSlots() {
  SlotConfig next[SENSOR_COUNT] = {};
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    String k = String(i), name = server.arg("n" + k);
    SlotConfig& c = next[i];
    if (!parseAddress(server.arg("a" + k), c.address)) return server.send(400, "text/plain", "bad address in slot " + String(i + 1));
    name.trim();
    if (name.isEmpty()) name = "Czujnik " + String(i + 1);
    // A quote or a backslash would break /api/now and Home's accessory list (HomeSpan writes names unescaped).
    bool bad = !validUtf8(name);
    for (char ch : name) bad |= (uint8_t)ch < 0x20 || ch == '"' || ch == 0x5C;
    if (bad) return server.send(400, "text/plain", "bad name in slot " + String(i + 1));
    if (name.length() >= sizeof(c.name)) return server.send(400, "text/plain", "name too long in slot " + String(i + 1));
    strlcpy(c.name, name.c_str(), sizeof(c.name));
    for (uint8_t r = 0; r < ROLE_COUNT; r++)
      if (server.arg("r" + k) == ROLE_KEYS[r]) c.role = r;
    c.on = server.arg("o" + k) == "1";
  }
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    for (size_t k = 0; k < i; k++) {
      if (!isUnset(next[i].address) && memcmp(next[i].address, next[k].address, 8) == 0)
        return server.send(400, "text/plain", "one probe in two slots");
      if (next[i].role != ROLE_OTHER && next[i].role == next[k].role && next[i].on && next[k].on)
        return server.send(400, "text/plain", String("role twice: ") + ROLE_KEYS[next[i].role]);
    }
  // h<i> = the slot whose history slot i takes over (handOn): one switched off or emptied in this save. A slot that
  // gets another probe drops what it took over; the others keep theirs (the web app sends no h).
  Takeover nextTk[TAKEOVER_MAX];
  memcpy(nextTk, takeovers, sizeof(nextTk));
  for (auto& h : nextTk)
    if (takeoverUsed(h) && memcmp(next[h.to].address, slots[h.to].address, 8) != 0) h = {};
  for (size_t i = 0; i < SENSOR_COUNT; i++) {
    String a = server.arg("h" + String(i));
    if (a.isEmpty()) continue;
    long from = a.toInt();
    time_t at = time(nullptr);
    if (from < 0 || from >= (long)SENSOR_COUNT || from == (long)i || !next[i].on || isUnset(next[i].address) ||
        next[from].on || !clockSet(at))
      return server.send(400, "text/plain", "bad history takeover in slot " + String(i + 1));
    Takeover* h = nullptr;
    for (auto& x : nextTk) if (!h && takeoverUsed(x) && x.to == i && x.from == from) h = &x;
    for (auto& x : nextTk) if (!h && !takeoverUsed(x)) h = &x;
    if (!h) return server.send(400, "text/plain", "no room for another history takeover");
    *h = {(uint8_t)i, (uint8_t)from, (uint32_t)at};
  }
  // The running slots stay as they are until the restart - the loop reads with them meanwhile.
  for (size_t i = 0; i < SENSOR_COUNT; i++)
    if (memcmp(next[i].address, slots[i].address, 8) != 0) forgetSlot(i, isUnset(next[i].address));
  prefs.putBytes("slots", next, sizeof(next));
  prefs.putBytes("takeover", nextTk, sizeof(nextTk));
  memcpy(takeovers, nextTk, sizeof(takeovers));
  techLog.println("Probe slots changed - restarting");
  server.send(200, "application/json", "{\"restart\":true}");
  restartAtMs = millis() + 1500;
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

// GET: the read-only support code (Relay.h) and until when it works. POST hours = 1..168: a new one (the old one
// stops working), revoke = 1: none. The support code itself cannot reach this (Relay.h refuses it).
void handleSupport() {
  if (server.method() == HTTP_POST) {
    uint32_t now = time(nullptr);
    if (server.arg("revoke") == "1") {                  // the key stays: its phone is told the code ended
      prefs.putUInt("supUntil", 1);
      relay.endSupport();
      techLog.println("Support code revoked");
    } else {
      long hours = server.arg("hours").toInt();
      if (hours < 1 || hours > 168) return server.send(400, "text/plain", "hours 1-168");
      if (!clockSet(now)) return server.send(503, "text/plain", "no clock yet");
      uint8_t s[32];
      esp_fill_random(s, 32);
      uint32_t until = now + hours * 3600;
      prefs.putBytes("supKey", s, 32);
      prefs.putUInt("supUntil", until);
      relay.setSupport(s, until);
      techLog.printf("Support code made for %ld h\n", hours);
    }
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"until\":" + String(relay.supportUntil()) + ",\"code\":\"" + relay.supportCode() + "\"}");
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
      techLog.println("Restoring a backup");
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
  techLog.println("Backup restored - restarting");
  delay(500);
  updater.confirmBeforeRestart();
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
  prefs.remove("takeover");
  techLog.println("History cleared from the web app - restarting");
  delay(300);
  minutes.clear();
  LittleFS.format();
  updater.confirmBeforeRestart();
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
  techLog.println("OTA password restored after a factory reset");
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
  techLog.println("Firmware uploaded from the web app - restarting");
  delay(500);
  ESP.restart();
}

// Called by HomeSpan every time WiFi comes up; the web server is started once and survives reconnects.
void onNetworkUp(int count) {
  networkUps = count;
  healthToday.wifiUps = add16(healthToday.wifiUps, 1);
  techLog.printf("Network up (#%d), IP %s - web app http://%s.local:%u\n",
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
  server.on("/api/diag", HTTP_GET, handleDiag);
  server.on("/api/events", HTTP_GET, handleEvents);
  server.on("/api/csv", HTTP_GET, handleCsv);
  server.on("/api/limits", HTTP_POST, handleLimits);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/bus", HTTP_GET, handleBus);
  server.on("/api/probes", HTTP_GET, handleProbes);
  server.on("/api/geco", HTTP_GET, handleGeco);
  server.on("/api/work", HTTP_GET, handleWork);
  server.on("/api/slots", HTTP_POST, handleSlots);
  server.on("/api/refills", HTTP_GET, handleRefills);
  server.on("/api/refill", HTTP_POST, handleRefill);
  server.on("/api/refill-undo", HTTP_POST, handleRefillUndo);
  server.on("/api/ash", HTTP_POST, handleAsh);
  server.on("/api/update", HTTP_POST, handleUpdate);
  server.on("/api/remote", HTTP_GET, handleRemote);
  server.on("/api/backup", HTTP_GET, handleBackup);
  server.on("/api/restore", HTTP_POST, handleRestoreDone, handleRestoreChunk);
  server.on("/api/remote", HTTP_POST, handleRemote);
  server.on("/api/support", HTTP_GET, handleSupport);
  server.on("/api/support", HTTP_POST, handleSupport);
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
        techLog.println("BOOT held 15 s - release for a FULL reset (WiFi, HomeKit pairing, history)");
      } else if (level < 1 && held >= WIFI_RESET_HOLD_MS) {
        level = 1;
        techLog.println("BOOT held 5 s - release to forget the WiFi, keep holding for a full reset");
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
    techLog.println(level == 2 ? "FULL reset - restarting" : "WiFi forgotten - restarting");
    delay(300);
    esp_restart();
  }
}

// ------------------------------------------------------------------ status LED

#if CONFIG_IDF_TARGET_ESP32S3
// HomeSpan blinks its status device from a task of its own with a 1 KB stack (HomeSpan 2.1.8, Blinker.cpp) - enough
// for a plain LED, not for the RGB pixel's RMT driver: the S3 crashed in it (2026-10-27, a stack overflow in
// BlinkTask). Here on() and off() only pass the state on; a task with room drives the pixel.
class StatusPixel : public Blinkable {
public:
  void begin(uint8_t pin) {
    this->pin = pin;
    xTaskCreatePinnedToCore(task, "statusPixel", 4096, this, 1, &handle, 0);
  }
  void on() override { if (handle) xTaskNotify(handle, 1, eSetValueWithOverwrite); }
  void off() override { if (handle) xTaskNotify(handle, 0, eSetValueWithOverwrite); }
  int getPin() override { return pin; }

private:
  uint8_t      pin    = 0;
  TaskHandle_t handle = nullptr;

  static void task(void* self) {
    Pixel pixel(static_cast<StatusPixel*>(self)->pin);
    pixel.setOnColor(Pixel::HSV(0, 100, 100));          // red, like HomeSpan's own status pixel
    for (uint32_t lit;;) {
      xTaskNotifyWait(0, 0, &lit, portMAX_DELAY);
      if (lit) pixel.on();
      else pixel.off();
    }
  }
};
StatusPixel statusPixel;
#endif

// ------------------------------------------------------------------ setup / loop

void setup() {
  Serial.begin(115200);
  techLog.begin();
  healthBegin();
  setenv("TZ", TIME_ZONE, 1);     // local days from the first second - after a soft restart the clock is already set
  tzset();
  snprintf(firmwareRevision, sizeof(firmwareRevision), "%u", (unsigned)FW_VERSION);
  updater.begin();

  bus.begin();
  bus.setWaitForConversion(false);
  geco.begin(GECO_RX_PIN, GECO_SCAN_PINS, sizeof(GECO_SCAN_PINS));   // listens from now on; loadSlots gives it
                                                                     // a moment to be heard
  prefs.begin("furnace", false);
  loadSlots();
  Serial.print(busReport());
  // After a full reset with the BOOT button the new home starts empty: no history, no old problems, its own coal
  // use and ash count (limits and settings stay).
  bool wipe = prefs.getBool("wipe", false);
  if (wipe) {
    for (const char* key : {"alerts", "coalK", "coalN", "feedK", "feedN", "strkKg", "strkN", "gU1", "gU2", "gAft", "gScan", "gwCuts", "ashAt", "relayKey", "supKey", "supUntil", "takeover"}) prefs.remove(key);   // old phones out
    prefs.putBool("cleared", true);
  }
  loadLimits();
  loadSettings();
  loadTakeovers();
  restoreAlerts();
  setupHistory(wipe);
  closeOldCuts();
  closeGecoLog();
  beginSettingWatch();
  coalToKilograms();
  if (!updater.crashLoop()) rescanHistory();
  if (wipe) prefs.remove("wipe");
  strokeSamples = prefs.getUShort("strkN", 0);
  strokeKg = strokeSamples ? prefs.getFloat("strkKg", NAN) : NAN;
  for (const char* key : {"coalK", "coalN", "feedK", "feedN"}) prefs.remove(key);   // v32's factors (water, feeder seconds)
  ashAt = prefs.getUInt("ashAt", 0);
  if (!updater.crashLoop()) recomputeCoal();      // after crash restarts: no heavy work until a fix gets in
  keepOtaPassword();
  for (const char* key : {"tgToken", "tgPeople"}) prefs.remove(key);   // Telegram (v13) is gone
  uint8_t secret[32];
  relaySecret(secret, false);
  relay.begin(WEB_PORT, prefs.getString("relayHost", RELAY_HOST), secret);
  uint8_t support[32];
  if (prefs.getBytes("supKey", support, 32) == 32) relay.setSupport(support, prefs.getUInt("supUntil", 0));
  updater.onNeedMemory = [](bool on) { relay.pause(on); };
  updater.proxyHost = [] { return relay.host(); };
  offerTold = prefs.getUInt("offerTold", 0);
  summaryDay = prefs.getUInt("sumDay", 0);
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
#if CONFIG_IDF_TARGET_ESP32S3
  statusPixel.begin(STATUS_PIXEL_PIN);
  homeSpan.setStatusDevice(&statusPixel);
#else
  homeSpan.setStatusPin(STATUS_LED_PIN);
#endif
  homeSpan.setGetCharacteristicsCallback(EveHistory::onGetCharacteristics);
  homeSpan.setApSSID(AP_SSID);
  homeSpan.setApPassword(AP_PASSWORD);
  homeSpan.setApTimeout(AP_TIMEOUT_S);
  homeSpan.enableAutoStartAP();              // no stored WiFi -> open the setup hotspot
  homeSpan.setHostNameSuffix("");            // host name is exactly DEVICE_NAME
  if (otaHashStored().length()) homeSpan.enableOTA();   // password lives in flash (CLI "O"), not in this public code
  else techLog.println("No OTA password set (serial \"O\") - network OTA and the web upload stay off");
  homeSpan.setConnectionCallback(onNetworkUp);
  homeSpan.begin(Category::Bridges, "Czujniki pieca", DEVICE_NAME);
  createAccessories();

  new SpanUserCommand('a', "- list the probes found", [](const char*) { Serial.print(busReport()); });
  new SpanUserCommand('u', "- check for a firmware update now", [](const char*) { updater.requestCheck(); });

  lastRequestMs = millis() - READ_INTERVAL_MS;   // first reading right away
}

void loop() {
  uint32_t loopStart = millis();
  if (restartAtMs && (int32_t)(millis() - restartAtMs) >= 0) {   // after a slot change (handleSlots)
    updater.confirmBeforeRestart();
    ESP.restart();
  }
  homeSpan.resetWatchdog();
  homeSpan.poll();
  if (webStarted) server.handleClient();
  if (!serialClosed && millis() >= SERIAL_WINDOW_MS) {
    homeSpan.setSerialInputDisable(true);
    serialClosed = true;
  }
  logHistory();
  logGecoChanges();
  updater.loop();
  if (updater.takeJustConfirmed()) logEvent(EV_UPDATED, 0, FW_VERSION);
  // A newer version found by the hourly check: once per version (also across restarts), a notification to open
  // the app and tap "Zainstaluj". A forced one installs by itself - "updated" comes then.
  // A new version is told once, and only when the hourly check found it: after "Sprawdz aktualizacje" the app
  // already shows it.
  uint32_t offer = updater.available();
  if (offer && offer != offerTold && !updater.forced()) {
    offerTold = offer;
    prefs.putUInt("offerTold", offer);
    if (!updater.checkedOnRequest())
      relay.alert(String("{\"t\":") + (uint32_t)time(nullptr) + ",\"e\":100,\"v\":" + offer + "}", "update");
  }

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
  uint32_t took = millis() - loopStart;
  if (took > healthToday.loopMaxMs) healthToday.loopMaxMs = min(took, (uint32_t)65535);
}
