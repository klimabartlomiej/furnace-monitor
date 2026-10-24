// Updates over the internet: the ESP itself checks a manifest on GitHub every hour, so fixes reach it in another
// home without a laptop. A newer version is only offered (available()): the web app shows it with its release
// notes and it is installed when someone taps "Zainstaluj" (requestInstall) - nobody gets an update they did not
// want. Two exceptions install the newest version by themselves: a release marked forced (the manifest's "min" is
// above this version - publish.ps1 -Forced), and a crash loop, where nobody could tap anything.
// Releases are made with release/publish.ps1.
//
//   manifest: {"version":3,"size":1515752,"sha256":"<64 hex>","url":"https://.../firmware-v3.bin",
//              "mirror":"https://cdn.jsdelivr.net/.../firmware-v3.bin","min":0,"sig":"<128 hex>","notes":{...}}
//
// The manifest counts only when it is signed with a release key (release/sign.ps1, ReleaseKeys.h - ECDSA P-256 over
// "furnace-fw|version|size|sha256|min"), and the image only when its SHA-256 matches the manifest: nobody on the way
// can slip in another firmware - the relay leg is plain HTTP and TLS here pins no certificate. The verifier first
// proves itself on a signed manifest and a forged copy; a build whose check fails that does not demand signatures,
// so a broken verifier can never shut the ESP out of updates. A freshly installed image runs on probation:
// it is confirmed after CONFIRM_AFTER_MS on the network; if it never gets online within ROLLBACK_AFTER_MS, or
// crashes before that, the bootloader goes back to the previous version (needs #include "SpanRollback.h" in the
// sketch). A version that went back is remembered and not installed again - only a newer one is.
// After crash restarts in a row the first check comes within seconds, so a fixed release can still get in.
//
// The check and the download run in a task of their own: the readings, HomeKit and the web app go on meanwhile,
// and the web app shows the progress (progress()). Getting the file must not fail on one bad connection:
//   - a download that breaks off resumes where it stopped (an HTTP Range request),
//   - first through the relay's /fw over plain HTTP (proxyHost): no second TLS connection, so no RAM for one,
//     and the relay stays connected - the phones watch the progress from anywhere. Plain HTTP is safe for the
//     file: what gets installed is decided by the signed manifest's SHA-256,
//   - then straight from GitHub and the manifest's "mirror" (jsDelivr - another CDN, the same file) over TLS;
//     a TLS connection needs ~40 KB of RAM, so the relay (onNeedMemory) lets its own go for those tries.
// The restart into the new version happens in the loop, never in the middle of its work.

#pragma once

#include <HomeSpan.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <functional>
#include <vector>
#include <mbedtls/sha256.h>
#include <mbedtls/ecdsa.h>
#include "ReleaseKeys.h"

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
  // The relay's host (Relay.h) for downloads over plain HTTP; empty = straight from GitHub only.
  std::function<String()> proxyHost;

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
  // 1 = signatures demanded, 0 = the verifier failed its self-test (SHA-256 only), -1 = not checked yet.
  int8_t signatures() const { return sigCheck; }

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
  int8_t      sigCheck       = -1;   // see signatures()
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

  // Exactly 2 x len hex digits into out.
  static bool fromHex(const String& hex, uint8_t* out, size_t len) {
    if (hex.length() != 2 * len) return false;
    for (size_t i = 0; i < 2 * len; i++) {
      char c = hex[i];
      int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (v < 0) return false;
      out[i / 2] = i % 2 ? out[i / 2] | v : v << 4;
    }
    return true;
  }

  // The manifest's "sig" signs "furnace-fw|version|size|sha256|min" - the values check() acts on, read the same
  // way - with one of RELEASE_KEYS: ECDSA P-256 over SHA-256, r||s in 128 hex digits (release/sign.ps1).
  static bool signedByUs(const String& manifest) {
    uint8_t sig[64], key[65], hash[32];
    if (!fromHex(field(manifest, "sig"), sig, sizeof(sig))) return false;
    String sha = field(manifest, "sha256");
    sha.toLowerCase();
    String msg = "furnace-fw|" + String(field(manifest, "version").toInt()) + "|" + String(field(manifest, "size").toInt())
               + "|" + sha + "|" + String(field(manifest, "min").toInt());
    if (mbedtls_sha256((const uint8_t*)msg.c_str(), msg.length(), hash, 0)) return false;
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = false;
    if (!mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) && !mbedtls_mpi_read_binary(&r, sig, 32)
        && !mbedtls_mpi_read_binary(&s, sig + 32, 32))
      for (const char* k : RELEASE_KEYS)
        if (fromHex(k, key, sizeof(key)) && !mbedtls_ecp_point_read_binary(&grp, &q, key, sizeof(key))
            && !mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &q, &r, &s)) {
          ok = true;
          break;
        }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
  }

  // A plain HTTP URL through the relay for a GitHub/jsDelivr one, empty without a relay.
  String viaRelay(const String& url) const {
    String host = proxyHost ? proxyHost() : String();
    if (host.isEmpty()) return String();
    String q;
    for (char c : url) {
      if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') q += c;
      else q += String("%") + "0123456789ABCDEF"[(uint8_t)c >> 4] + "0123456789ABCDEF"[c & 15];
    }
    return "http://" + host + "/fw?u=" + q;
  }

  // The places to try in turn: the relay (plain HTTP) for each source first, then each source over TLS.
  std::vector<String> sources(const String& url, const String& mirror) const {
    std::vector<String> out;
    for (const String* s : {&url, &mirror})
      if (s->length() && viaRelay(*s).length()) out.push_back(viaRelay(*s));
    for (const String* s : {&url, &mirror})
      if (s->length()) out.push_back(*s);
    return out;
  }

  // Plain HTTP (the relay) or TLS (GitHub, no CA pinned: what gets installed is decided by the SHA-256 from the
  // signed manifest, so a corrupted, truncated or swapped download can never be flashed). A TLS try gets all the
  // memory first.
  bool open(HTTPClient& http, NetworkClient& plain, NetworkClientSecure& tls, const String& url) {
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (url.startsWith("http://")) return http.begin(plain, url);
    needMemory();
    tls.setInsecure();
    tls.setHandshakeTimeout(10);      // seconds; the default of 120 would hold a dead check for minutes
    return http.begin(tls, url);
  }

  // The manifest: through the relay, then from GitHub over TLS (twice).
  bool fetchManifest(String& manifest, String& why) {
    std::vector<String> tries = sources(manifestUrl, String());
    tries.push_back(manifestUrl);
    for (size_t attempt = 0; attempt < tries.size(); attempt++) {
      if (attempt) delay(2000);
      NetworkClient plain;
      NetworkClientSecure tls;
      HTTPClient http;
      if (!open(http, plain, tls, tries[attempt])) {
        why = "bad manifest URL";
        continue;
      }
      int code = http.GET();
      if (code == 200) {
        manifest = http.getString();
        http.end();
        if (manifest.indexOf("\"version\"") >= 0) return true;
        why = "not a manifest";
        continue;
      }
      why = "manifest HTTP " + String(code) + tlsError(tls);
      http.end();
    }
    return false;
  }

  void check() {
    String manifest, why;
    if (!fetchManifest(manifest, why)) return fail(why);
    if (sigCheck < 0) {
      String forged = RELEASE_SELFTEST;
      forged.replace("\"version\":1,", "\"version\":2,");
      sigCheck = signedByUs(RELEASE_SELFTEST) && !signedByUs(forged);
      Serial.printf("Update: signature check %s\n", sigCheck ? "passed its self-test" : "FAILED its self-test - SHA-256 only");
    }
    if (sigCheck && !signedByUs(manifest)) {
      offered = 0;
      mustUpdate = false;
      return fail("manifest not signed");
    }

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
    std::vector<String> from = sources(url, mirror);
    uint8_t failures = 0;
    for (size_t n = 0; written < size && failures < ATTEMPTS; n++) {
      if (n) delay(3000);
      size_t before = written;
      int code = 0;
      // A source that made progress is kept; one that failed hands over to the next.
      why = download(from.front(), written, size, ctx, code);
      if (why.startsWith("flash")) break;      // the flash refused it - another connection will not help
      if (written > before) {
        failures = 0;
      } else {
        failures++;
        String failed = from.front();          // the next source first, this one at the end
        from.erase(from.begin());
        from.push_back(failed);
      }
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
    NetworkClient plain;
    NetworkClientSecure client;
    HTTPClient http;
    if (!open(http, plain, client, url)) return "bad firmware URL";
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
