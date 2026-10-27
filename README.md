# Furnace monitor

ESP32 + four DS18B20 probes on a coal-fired furnace, straight into Apple Home - no Homebridge, no Raspberry Pi.

- **Apple Home** (HomeSpan): four temperature sensors (Piec, Powrót, Zasobnik węgla, Bojler) with a fault flag.
  Alarms, warnings and "hot water ready" are notifications of the app (below), not Home accessories.
- **Eve app graphs**: every sensor keeps an Eve-compatible history on the ESP itself (`EveHistory.h`, protocol as
  reverse-engineered by [fakegato-history](https://github.com/simont77/fakegato-history)).
- **Warnings** (tuned in the web app, sent as notifications): furnace dying out (falling and within a few degrees
  of its min limit - a fast fall alone, e.g. from a bath, is no sign), freezing water, embers creeping into the
  coal hopper.
- **Baths**: hot water drawn shows as the boiler losing 2 degrees within 10 minutes; each draw falling 5 minutes or
  more (shorter = a sink) goes into the journal (how far it fell, how long), shows as a blue mark on the furnace and
  boiler charts and is counted per month. v24 also found the baths of the minute log (31 days back) once and dropped
  the "Piec wygasa" warnings the old fast-fall rule had raised far above the min limit.
- **Web app** `http://furnace.local:8080` on the home WiFi (in Polish): live cards, charts from 6 hours to years
  with pinch-zoom, alarm limits and warnings, event journal, furnace burning hours, outdoor temperature
  (Open-Meteo), CSV export, firmware updates. The "Miesiące" tab goes back season by season (September to
  August): a bar per month (coal or burning hours, the biggest labelled), and per month its days, coal and cost,
  refills, temperatures, power cuts and alarms against the same month a year before.
- **Notifications** (`web/sw.js`): in the remote app, Ustawienia -> Dostęp zdalny -> "Włącz powiadomienia":
  alarms and their end, dead probes, warnings, power cuts of a minute or more, hot water ready, a new version to
  install and a finished update - each phone picks which kinds it gets (the relay filters by kind) - sealed by the
  ESP with the pairing key, so the relay passes them to the push services unread - and "Piec offline" from the
  relay itself when the ESP has been gone 5 minutes (a power cut or no internet). On an iPhone the app must be on
  the home screen.
- **Remote access** (`Relay.h`, `relay/`): the same web app from GitHub Pages
  (https://klimabartlomiej.github.io/furnace-monitor/) works anywhere. The ESP keeps one outbound WebSocket to a
  relay on Cloudflare's free plan; the phones connect to it too. Every message is AES-256-GCM encrypted with a key
  only the ESP and the paired phones have - the relay sees noise and stores nothing but when the ESP was last seen
  (the app shows "offline since" after a power cut). Nothing to open on the router, works behind CGNAT. Pairing:
  home web app -> Ustawienia -> Dostęp zdalny -> "Otwórz na tym telefonie" (or copy the code to another phone).
- **Coal** (feeder furnace with a hopper, coal carried up loose): "Dosypałem" in the web app records a refill in
  kilograms - typed, or counted in shovels (~3.2 kg) and buckets (~9.6 kg) - to the top or not, also for an
  earlier hour, or from how full the hopper was before. Coal burned is taken as proportional to the heat output
  (furnace minus return, summed per minute); the factor is learnt from refills to the top. Gives daily use in kg
  and costs, the hopper level (its size in Ustawienia -> Dane kotła, 100 kg by default), time left, a "Mało
  węgla" warning, a 30-day chart with the frost dependence and the season so far. Can be switched off in the
  settings. Up to v21 coal was counted in buckets; v22 converts the stored data once (coalToKilograms).
- **Hot water**: "Ciepła woda gotowa" - a notification (at most one an hour) when the boiler reaches its "ready"
  temperature.
- **Summer mode**: switches off "too cold" on the furnace and the return plus the burning warnings; the web app
  suggests it after three days without burning, and it ends by itself after two hours of burning.
- **More warnings**: "Piec stoi po zaniku prądu" (the furnace keeps cooling after a power cut - its controller
  did not restart) and "Opróżnij popielnik" (after a number of buckets burned or days, reset with a button).
  The coal card also forecasts tomorrow's use from the weather forecast and the learnt frost dependence.
- **Power cuts**: the ESP marks "still alive" every 2 s in its own small flash partition, so a start after a power
  cut logs the cut to about two seconds - blips of a few seconds as well as hours - in the journal; the web app
  shows them as bands on the charts and per day (how many, how long) for 30 days. Alarm, dead-probe and warning states
  survive a restart, so a problem that outlasts a power cut or an update is not announced again.
- **Backup** (`Backup.h`): Ustawienia -> Kopia zapasowa downloads the whole device as one file (history,
  settings, WiFi, HomeKit pairing, remote-access key); restored on a new board, the Home app and the phones see it
  as the old one. Home WiFi only.
- **History**: every minute for 31 days (raw flash partition), daily summaries for 5 years, the journal and 14 days
  of Eve entries (LittleFS). All rings - nothing ever fills up.
- **Updates over the internet**: the ESP checks `release/manifest.json` every hour. A newer version is only
  offered: the web app shows what it brings and installs it on "Zainstaluj", with a progress bar (the download
  runs in its own task, resumes after a break-off and goes in turn to GitHub and a jsDelivr mirror). A release published with `-Forced` installs by itself on
  every older ESP. Automatic rollback if the new version does not come online. Releases are signed (ECDSA P-256,
  `ReleaseKeys.h`): the ESP installs only a manifest signed with a release key, so nobody on the way can slip in
  another firmware - its verifier first proves itself on a signed sample, so a broken check never locks it out.

## Hardware

| Part | Note |
| --- | --- |
| ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM) | flash through the "COM/UART" USB-C port; `release/publish.ps1 -Target s3` |
| ESP32 dev board (ESP32-D0WD), the first board | frozen at v25 (flash full) - only urgent fixes, `-Target classic` |
| DS18B20 (waterproof probes) | data on **GPIO4** (S3) / **D22** (classic), 4.7 kOhm pull-up to 3.3 V, powered 3-wire |
| MAX6675 + K thermocouple (the flue, S3 only) | CS **GPIO10**, SCK **GPIO12**, SO **GPIO13**, 3.3 V |

Probes sit in slots (8 on the S3, 4 on the classic board), each with a role - Piec, Powrót, Zasobnik węgla,
Bojler, Spaliny or Inny - set in the web app (Ustawienia -> Czujniki). The furnace logic looks probes up by role;
history stays with the slot, so a replacement probe ("Użyj zamiast…") carries it on. A probe found at a start that
no slot knows takes a free slot by itself - a thermocouple straight into the flue role. Until the slots are saved,
the probes at dad's (`DEFAULT_SLOTS` in `PiecTemp.ino`) are the defaults. A backup of the classic board (4 probes)
restores into the S3's 8 slots. `diagnostics/PiecTempTest` finds and health-checks DS18B20s.

Diagnostics: Ustawienia -> Urządzenie -> "Raport diagnostyczny" gives the device state, health counters per day
(reconnects, probe errors, failed updates, least RAM, longest loop, weakest WiFi, hottest chip, starts), the last
crash from the core dump (backtrace addresses: `xtensa-esp32s3-elf-addr2line -pfiaC -e <elf>`, the ELFs are kept in
`%USERPROFILE%\.furnace-monitor\elf` by `publish.ps1`, named by the build SHA the report shows) and the technical
log. A support code (Ustawienia -> Dostęp zdalny) gives someone a read-only view through the relay for 24 hours
or 7 days: GET only, never the backup, the pairing code or the support code; revoked any time.

## First start

1. Without stored WiFi the ESP opens the open hotspot **Furnace-Setup** - join it with a phone, pick the network,
   type its password, leave the setup code empty.
2. Home app -> Add Accessory -> More options -> **Czujniki pieca**, setup code **466-37-726**.
3. Notifications come from the web app (Ustawienia -> Powiadomienia), not from the Home app.

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
- Release: bump `FW_VERSION`, add its note to `CHANGES` in `web/index.html`, run `release/publish.ps1 [-Forced]`
  (builds, writes `release/firmware-vN.bin` and `manifest.json` with the notes, signs it, pushes).
- Release keys: made once by `release/keygen.ps1` into `%USERPROFILE%\.furnace-monitor` (never the repo):
  `release-key.bin` signs every release, `release-key-backup.bin` is the spare (`publish.ps1 -KeyFile ...`) - keep
  copies of both off this PC. Without them the ESPs take updates only over USB or from a file on the home WiFi. On the same network the
  IDE's network port "furnace" works too (OTA password set with the serial command `O`).
- Remote web app: every push that changes `web/` is published to GitHub Pages (`.github/workflows/pages.yml`).
- Relay: `cd relay`, `npx wrangler login` once, `npx wrangler deploy`; the printed `*.workers.dev` host goes into
  the home web app (Ustawienia -> Dostęp zdalny).
