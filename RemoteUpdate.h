// Updates over the internet: the ESP itself checks a manifest on GitHub every hour, so fixes reach it in another
// home without a laptop. A newer version is only offered (available()): the web app shows it with its release
// notes and it is installed when someone taps "Zainstaluj" (requestInstall) - nobody gets an update they did not
// want. Two exceptions install the newest version by themselves: a release marked forced (the manifest's "min" is
// above this version - publish.ps1 -Forced), and a crash loop, where nobody could tap anything.
// Releases are made with release/publish.ps1.
//
//   manifest: {"version":3,"size":1515752,"sha256":"<64 hex>","url":"https://.../firmware-v3.bin",
//              "mirror":"https://cdn.jsdelivr.net/.../firmware-v3.bin","min":0,"notes":{...}}
//
// The image is accepted only when its SHA-256 matches the manifest. A freshly installed image runs on probation:
// it is confirmed after CONFIRM_AFTER_MS on the network; if it never gets online within ROLLBACK_AFTER_MS, or
// crashes before that, the bootloader goes back to the previous version (needs #include "SpanRollback.h" in the
// sketch). A version that went back is remembered and not installed again - only a newer one is.
// After crash restarts in a row the first check comes within seconds, so a fixed release can still get in.
//
// The check and the download run in a task of their own: the readings, HomeKit and the web app go on meanwhile,
// and the web app shows the progress (progress()). Getting the file must not fail on one bad connection:
//   - a download that breaks off resumes where it stopped (an HTTP Range request),
//   - the tries go in turn to GitHub and to the manifest's "mirror" (jsDelivr - another CDN, the same file),
//   - a second TLS connection needs ~40 KB of RAM: the relay (onNeedMemory) lets its own go right away when the
//     heap is short, and after any failed try - the manifest fetch gets a second try the same way.
// The restart into the new version happens in the loop, never in the middle of its work.

#pragma once

#include <HomeSpan.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <functional>
#include <mbedtls/sha256.h>

class RemoteUpdate {
public:
  static constexpr uint32_t FIRST_CHECK_MS    = 2 * 60 * 1000;    // first check after boot
  static constexpr uint32_t CRASH_CHECK_MS    = 20 * 1000;        // ... after a crash restart
  static constexpr uint32_t CHECK_INTERVAL_MS = 60 * 60 * 1000;   // then every hour
  static constexpr uint32_t CONFIRM_AFTER_MS  = 60 * 1000;        // online this long -> the new version is good
  static constexpr uint32_t ROLLBACK_AFTER_MS = 10 * 60 * 1000;   // never online by then -> back to the old version
  static constexpr uint32_t CRASH_FREE_MS     = 10 * 60 * 1000;   // running this long ends a crash streak
  static constexpr uint32_t STALL_MS          = 15 * 1000;        // no bytes this long = the connection is dead
  static constexpr uint8_t  ATTEMPTS          = 6;                // connections one download may take

  enum Phase : uint8_t { IDLE, CHECKING, DOWNLOADING, RESTARTING };
  struct Progress {
    Phase    phase;
    uint32_t version;    // the version being installed
    uint32_t done;       // bytes written so far
    uint32_t size;
  };

  // Called with true when the firmware connection could not be opened (maybe no memory for a second TLS
  // connection): whoever holds one lets it go until the call with false, after the download.
  std::function<void(bool)> onNeedMemory;

  RemoteUpdate(const char* manifestUrl, uint32_t version) : manifestUrl(manifestUrl), version(version) {}

  void begin() {
    store.begin("update", false);
    esp_ota_img_states_t s;
    probation = esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY;
    if (probation) Serial.printf("Update: v%u is on probation\n", (unsigned)version);
    else esp_ota_mark_app_valid_cancel_rollback();   // flashed over USB: nothing to verify

    // "trying" is the version installed last; running another one means it went back.
    uint32_t trying = store.getUInt("trying", 0);
    if (trying && trying != version) {
      store.putUInt("bad", trying);
      Serial.printf("Update: v%u did not work - not installing it again\n", (unsigned)trying);
    }
    if (trying && (trying != version || !probation)) store.remove("trying");
    bad = store.getUInt("bad", 0);

    esp_reset_reason_t r = esp_reset_reason();
    bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    crashes = crash ? store.getUChar("crashes", 0) + 1 : 0;
    if (crashes != store.getUChar("crashes", 0)) store.putUChar("crashes", crashes);
    if (crashes) Serial.printf("Update: crash restart #%u - checking for a fix soon\n", crashes);
  }

  // Two crash restarts in a row: the sketch skips its heavy start-up work until a fix arrives or it settles.
  bool crashLoop() const { return crashes >= 2; }

  void networkUp() {
    if (!networkUpMs) networkUpMs = millis();
  }

  void requestCheck() { checkRequested = true; }

  // Install the offered version (or a newer one the manifest has by now).
  void requestInstall(uint32_t v) {
    installWanted = v;
    checkRequested = true;
  }

  uint32_t available() const { return offered; }   // a newer version waiting to be installed, 0 = none
  bool forced() const { return mustUpdate; }         // ... that this version may not refuse (installs by itself)

  String status() const {
    portENTER_CRITICAL(&mux);
    String s = statusText;
    portEXIT_CRITICAL(&mux);
    return s;
  }

  Progress progress() const { return {phase, target, done, total}; }
  bool busy() const { return phase != IDLE; }

  // True once, right after a freshly installed version has been confirmed (for the event journal).
  bool takeJustConfirmed() {
    bool v = justConfirmed;
    justConfirmed = false;
    return v;
  }

  void loop() {
    if (phase == RESTARTING && millis() - restartAt >= 500) {
      Serial.printf("Update: v%u written, restarting\n", (unsigned)target);
      ESP.restart();
    }
    if (crashes && millis() >= CRASH_FREE_MS) {
      crashes = 0;
      store.putUChar("crashes", 0);
    }
    if (probation) {
      if (networkUpMs && millis() - networkUpMs >= CONFIRM_AFTER_MS) {
        esp_ota_mark_app_valid_cancel_rollback();
        probation = false;
        justConfirmed = true;
        store.remove("trying");
        setStatus("v" + String(version) + " installed and confirmed");
      } else if (!networkUpMs && millis() >= ROLLBACK_AFTER_MS) {
        Serial.println("Update: the new version never got online - rolling back");
        delay(200);
        esp_ota_mark_app_invalid_rollback_and_reboot();
      }
      return;   // no further updates while on probation
    }

    if (phase != IDLE || WiFi.status() != WL_CONNECTED || Update.isRunning()) return;   // isRunning: a web upload
    bool due = lastCheckMs ? millis() - lastCheckMs >= CHECK_INTERVAL_MS
                           : millis() >= (crashes ? CRASH_CHECK_MS : FIRST_CHECK_MS);
    if (!due && !checkRequested) return;
    checkRequested = false;
    lastCheckMs = millis();
    target = done = total = 0;
    phase = CHECKING;
    xTaskCreatePinnedToCore(task, "update", 10240, this, 1, nullptr, 0);
  }

private:
  const char* manifestUrl;
  uint32_t    version;
  bool        probation      = false;
  bool        justConfirmed  = false;
  bool        checkRequested = false;
  uint32_t    networkUpMs    = 0;
  uint32_t    lastCheckMs    = 0;
  uint32_t    bad            = 0;    // a version that went back here - skipped
  uint8_t     crashes        = 0;    // crash restarts in a row
  Preferences store;
  mutable portMUX_TYPE mux   = portMUX_INITIALIZER_UNLOCKED;
  char        statusText[120] = "not checked yet";
  volatile Phase    phase    = IDLE;
  volatile uint32_t target   = 0;
  volatile uint32_t done     = 0;
  volatile uint32_t total    = 0;
  uint32_t    restartAt      = 0;
  volatile uint32_t offered  = 0;
  volatile bool     mustUpdate = false;
  volatile uint32_t installWanted = 0;
  uint8_t     buf[1024];

  bool        freed          = false;    // the relay let its connection go for us

  static void task(void* self) {
    RemoteUpdate* u = static_cast<RemoteUpdate*>(self);
    u->check();
    if (u->phase != RESTARTING) u->releaseMemory();
    u->installWanted = 0;                       // a failed install waits for the next tap
    if (u->phase != RESTARTING) u->phase = IDLE;
    vTaskDelete(nullptr);
  }

  void setStatus(const String& s) {
    portENTER_CRITICAL(&mux);
    strlcpy(statusText, s.c_str(), sizeof(statusText));
    portEXIT_CRITICAL(&mux);
    Serial.println("Update: " + s);
  }

  void fail(const String& why) { setStatus("check failed: " + why); }

  void needMemory() {
    if (freed || !onNeedMemory) return;
    onNeedMemory(true);                         // returns once the relay's connection is closed
    freed = true;
    delay(300);
  }

  void releaseMemory() {
    if (!freed) return;
    onNeedMemory(false);
    freed = false;
  }

  // Tiny reader for the flat manifest: "key":value or "key":"value".
  static String field(const String& json, const char* key) {
    int k = json.indexOf("\"" + String(key) + "\"");
    if (k < 0) return "";
    int p = json.indexOf(':', k) + 1;
    while (p < (int)json.length() && (json[p] == ' ' || json[p] == '"')) p++;
    int end = p;
    while (end < (int)json.length() && json[end] != '"' && json[end] != ',' && json[end] != '}') end++;
    return json.substring(p, end);
  }

  // GitHub is reached over TLS without pinning a CA; what gets installed is decided by the SHA-256 from the
  // manifest, so a corrupted or truncated download can never be flashed.
  static bool open(HTTPClient& http, NetworkClientSecure& client, const String& url) {
    client.setInsecure();
    client.setHandshakeTimeout(10);   // seconds; the default of 120 would hold a dead check for minutes
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    return http.begin(client, url);
  }

  // The manifest; more tries with all the memory when the first fails.
  bool fetchManifest(String& manifest, String& why) {
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
      if (attempt) needMemory();
      if (attempt == 2) delay(3000);
      NetworkClientSecure client;
      HTTPClient http;
      if (!open(http, client, manifestUrl)) {
        why = "bad manifest URL";
        return false;
      }
      int code = http.GET();
      if (code == 200) {
        manifest = http.getString();
        http.end();
        return manifest.length() > 0;
      }
      why = "manifest HTTP " + String(code) + tlsError(client);
      http.end();
    }
    return false;
  }

  void check() {
    // Someone tapped "Zainstaluj": let the relay take the answer to the phone, then give the download all the
    // memory from the start (measured: the manifest failed twice while the busy app kept the relay working).
    if (installWanted) {
      delay(1500);
      needMemory();
    }
    String manifest, why;
    if (!fetchManifest(manifest, why)) return fail(why);

    uint32_t available = field(manifest, "version").toInt();
    mustUpdate = (uint32_t)field(manifest, "min").toInt() > version;
    if (available <= version) {
      offered = 0;
      return setStatus("up to date (v" + String(version) + ")");
    }
    if (available == bad) {
      offered = 0;
      return setStatus("v" + String(available) + " did not work here - waiting for a newer one");
    }
    offered = available;
    if (!(installWanted && available >= installWanted) && !mustUpdate && !crashLoop())
      return setStatus("v" + String(available) + " available");
    String url = field(manifest, "url");
    String mirror = field(manifest, "mirror");
    String sha = field(manifest, "sha256");
    size_t size = field(manifest, "size").toInt();
    if (url.isEmpty() || sha.length() != 64 || !size) return fail("incomplete manifest");
    Serial.printf("Update: v%u available (%u bytes), installing\n", (unsigned)available, (unsigned)size);
    install(url, mirror, sha, size, available);
  }

  void install(const String& url, const String& mirror, String sha, size_t size, uint32_t available) {
    if (!Update.begin(size)) return fail(String("no room: ") + Update.errorString());
    target = available;
    total = size;
    done = 0;
    phase = DOWNLOADING;
    setStatus("installing v" + String(available));
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    size_t written = 0;
    String why;
    if (ESP.getMaxAllocHeap() < 40000 || ESP.getFreeHeap() < 80000) needMemory();
    uint8_t failures = 0;
    for (uint8_t n = 0; written < size && failures < ATTEMPTS; n++) {
      if (n) delay(3000);
      const String& from = mirror.length() && n % 2 ? mirror : url;   // GitHub and the mirror in turn
      size_t before = written;
      int code = 0;
      why = download(from, written, size, ctx, code);
      if (why.startsWith("flash")) break;      // the flash refused it - another connection will not help
      failures = written > before ? 0 : failures + 1;   // progress made: a break-off costs no attempt
      if (written < size) needMemory();        // whatever broke it, the next try gets all the memory
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    Serial.printf("Update: %u of %u bytes, heap %u (largest block %u)%s%s\n", (unsigned)written, (unsigned)size,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), why.length() ? " - " : "", why.c_str());

    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    sha.toLowerCase();
    if (written != size) {
      Update.abort();
      return fail("download stopped at " + String(written) + " of " + String(size) + " bytes (" + why + ")");
    }
    if (sha != hex) {
      Update.abort();
      return fail("SHA-256 mismatch");
    }
    if (!Update.end(true)) return fail(String("flash: ") + Update.errorString());

    store.putUInt("trying", available);
    setStatus("v" + String(available) + " written, restarting");
    restartAt = millis();
    phase = RESTARTING;
  }

  // One connection, from `written` on (a Range request after a break-off). Returns why it stopped, "" = complete;
  // `code` is the HTTP status, negative when no connection was made.
  String download(const String& url, size_t& written, size_t size, mbedtls_sha256_context& ctx, int& code) {
    NetworkClientSecure client;
    HTTPClient http;
    if (!open(http, client, url)) return "bad firmware URL";
    if (written) http.addHeader("Range", "bytes=" + String(written) + "-");
    code = http.GET();
    if (code != 200 && code != 206) {
      String why = "HTTP " + String(code) + tlsError(client);
      http.end();
      return why;
    }
    // A server that ignored the Range sends everything again: what is written already is skipped.
    size_t skip = written && code == 200 ? written : 0;
    NetworkClient* stream = http.getStreamPtr();
    String why;
    uint32_t lastData = millis();
    while (written < size && millis() - lastData < STALL_MS) {
      int avail = stream->available();
      if (avail <= 0) {
        if (!stream->connected()) {
          why = "connection closed";
          break;
        }
        delay(2);
        continue;
      }
      int n = stream->readBytes(buf, min((size_t)avail, min(sizeof(buf), skip ? skip : size - written)));
      if (n <= 0) continue;
      lastData = millis();
      if (skip) {
        skip -= n;
        continue;
      }
      mbedtls_sha256_update(&ctx, buf, n);
      if (Update.write(buf, n) != (size_t)n) {
        why = String("flash: ") + Update.errorString();
        break;
      }
      written += n;
      done = written;
    }
    if (written < size && why.isEmpty()) why = "no data for 15 s";
    if (written < size) why += tlsError(client);
    http.end();
    return written < size ? why : String();
  }

  static String tlsError(NetworkClientSecure& client) {
    char e[80];
    return client.lastError(e, sizeof(e)) ? String(", TLS: ") + e : String();
  }
};
