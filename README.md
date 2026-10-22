# Furnace monitor

ESP32 + four DS18B20 probes on a coal-fired furnace, straight into Apple Home - no Homebridge, no Raspberry Pi.

- **Apple Home** (HomeSpan): four temperature sensors (Piec, Powrót, Zasobnik węgla, Bojler) plus one "Alarm ..."
  contact sensor per probe, which opens when a temperature leaves its limits or a probe dies - turn on its
  notifications for a push on the phone.
- **Eve app graphs**: every sensor keeps an Eve-compatible history on the ESP itself (`EveHistory.h`, protocol as
  reverse-engineered by [fakegato-history](https://github.com/simont77/fakegato-history)).
- **Web app** `http://furnace.local:8080` (home WiFi only, in Polish): live values, charts up to a year, alarm limits.
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
history) before moving the device to another home. After 5 minutes without WiFi the hotspot also opens by itself.

## Building and releasing

- Arduino IDE / arduino-cli, board **ESP32 Dev Module**, libraries HomeSpan, OneWire 2.3.8+, DallasTemperature.
  `partitions.csv` here gives 2 x 1.69 MB app slots and 512 KB LittleFS.
- Web app source: `web/index.html` -> `web/build.ps1` -> `WebApp.h`.
- Release: bump `FW_VERSION`, run `release/publish.ps1` (builds, writes `release/firmware-vN.bin` and
  `manifest.json`, pushes). On the same network the IDE's network port "furnace" works too (OTA password set with
  the serial command `O`).
