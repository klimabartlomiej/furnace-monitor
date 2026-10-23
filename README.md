# Furnace monitor

ESP32 + four DS18B20 probes on a coal-fired furnace, straight into Apple Home - no Homebridge, no Raspberry Pi.

- **Apple Home** (HomeSpan): four temperature sensors (Piec, Powrót, Zasobnik węgla, Bojler) plus one "Alarm ..."
  contact sensor per probe, which opens when a temperature leaves its limits or a probe dies - turn on its
  notifications for a push on the phone.
- **Eve app graphs**: every sensor keeps an Eve-compatible history on the ESP itself (`EveHistory.h`, protocol as
  reverse-engineered by [fakegato-history](https://github.com/simont77/fakegato-history)).
- **Warnings** (each one a contact sensor in Apple Home, tuned in the web app): furnace dying out (falling fast or
  heading below its min limit), freezing water, embers creeping into the coal hopper.
- **Web app** `http://furnace.local:8080` (home WiFi only, in Polish): live cards, charts from 6 hours to years
  with pinch-zoom, alarm limits and warnings, event journal, furnace burning hours, outdoor temperature
  (Open-Meteo), CSV export, firmware updates.
- **Coal** (feeder furnace with a hopper, coal carried up loose in buckets): "Dosypałem" in the web app records a
  refill in buckets (to the top or not, also for an earlier hour; or from how full the hopper was before). Coal
  burned is taken as proportional to the heat output (furnace minus return, summed per minute); the factor is
  learnt from refills to the top. Gives daily use, kilograms and costs (from one bucket's weight), the hopper
  level, time left, a "Mało węgla" contact in Apple Home, a 30-day chart with the frost dependence and the
  season so far. Can be switched off in the settings.
- **Hot water**: a "Ciepła woda gotowa" contact opens when the boiler reaches its "ready" temperature.
- **Summer mode**: switches off "too cold" on the furnace and the return plus the burning warnings; the web app
  suggests it after three days without burning, and it ends by itself after two hours of burning.
- **More warnings**: "Piec stoi po zaniku prądu" (the furnace keeps cooling after a power cut - its controller
  did not restart) and "Opróżnij popielnik" (after a number of buckets burned or days, reset with a button).
  The coal card also forecasts tomorrow's use from the weather forecast and the learnt frost dependence.
- **Power cuts**: the ESP marks "still alive" every 2 s in its own small flash partition, so a start after a power
  cut logs the cut to about two seconds - blips of a few seconds as well as hours - in the journal; the web app
  shows them as bands on the charts and per day (how many, how long) for 30 days. Alarm, dead-probe and warning states
  survive a restart, so a problem that outlasts a power cut or an update is not announced again.
- **History**: every minute for 31 days (raw flash partition), daily summaries for 5 years, the journal and 14 days
  of Eve entries (LittleFS). All rings - nothing ever fills up.
- **Updates over the internet**: the ESP checks `release/manifest.json` every hour and installs newer firmware,
  with automatic rollback if the new version does not come online.

## Hardware

| Part | Note |
| --- | --- |
| ESP32 dev board (ESP32-D0WD) | USB-serial CH340 |
| 4 x DS18B20 (waterproof probes) | data on **D22**, 4.7 kOhm pull-up to 3.3 V, powered 3-wire |

Probe addresses are pinned in `SENSORS` in `PiecTemp.ino`; `diagnostics/PiecTempTest` finds and health-checks them.

## First start

1. Without stored WiFi the ESP opens the open hotspot **Furnace-Setup** - join it with a phone, pick the network,
   type its password, leave the setup code empty.
2. Home app -> Add Accessory -> More options -> **Czujniki pieca**, setup code **466-37-726**.
3. Turn on notifications for the "Alarm ..." accessories.

BOOT button: hold ~5 s and release = new WiFi; hold ~15 s and release = full reset (WiFi, HomeKit pairing and
history; the OTA password stays) before moving the device to another home. A router that is down does not open
the hotspot - the ESP keeps recording and reconnects by itself.

Network OTA and "install from file" in the web app need the OTA password (serial command `O`, typed within 5
minutes of a boot - serial input is switched off after that); without one they stay off. Updates from GitHub
need no password.

## Building and releasing

- Arduino IDE / arduino-cli, board **ESP32 Dev Module**, libraries HomeSpan, OneWire 2.3.8+, DallasTemperature.
  `partitions.csv` here gives 2 x 1.69 MB app slots, a 352 KB minute ring, 16 KB of alive marks and a 208 KB
  LittleFS (changing it needs a USB upload).
- Web app source: `web/index.html` -> `web/build.ps1` -> `WebApp.h`.
- Release: bump `FW_VERSION`, run `release/publish.ps1` (builds, writes `release/firmware-vN.bin` and
  `manifest.json`, pushes). On the same network the IDE's network port "furnace" works too (OTA password set with
  the serial command `O`).
