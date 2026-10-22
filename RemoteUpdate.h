// Updates over the internet: the ESP itself checks a manifest on GitHub and installs newer firmware, so fixes
// reach it in another home without a laptop. Releases are made with release/publish.ps1.
//
//   manifest: {"version":3,"size":1515752,"sha256":"<64 hex>","url":"https://.../firmware-v3.bin"}
//
// The image is accepted only when its SHA-256 matches the manifest. A freshly installed image runs on probation:
// it is confirmed after CONFIRM_AFTER_MS on the network; if it is not online within ROLLBACK_AFTER_MS, or crashes
// before that, the bootloader goes back to the previous version (needs #include "SpanRollback.h" in the sketch).

#pragma once

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

class RemoteUpdate {
public:
  static constexpr uint32_t FIRST_CHECK_MS    = 2 * 60 * 1000;    // first check after boot
  static constexpr uint32_t CHECK_INTERVAL_MS = 60 * 60 * 1000;   // then every hour
  static constexpr uint32_t CONFIRM_AFTER_MS  = 60 * 1000;        // online this long -> the new version is good
  static constexpr uint32_t ROLLBACK_AFTER_MS = 4 * 60 * 1000;    // not online by then -> back to the old version

  RemoteUpdate(const char* manifestUrl, uint32_t version) : manifestUrl(manifestUrl), version(version) {}

  void begin() {
    esp_ota_img_states_t s;
    probation = esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY;
    if (probation) Serial.printf("Update: v%u is on probation\n", (unsigned)version);
    else esp_ota_mark_app_valid_cancel_rollback();   // flashed over USB: nothing to verify
  }

  void networkUp() {
    if (!networkUpMs) networkUpMs = millis();
  }

  void requestCheck() { checkRequested = true; }

  const String& status() const { return lastStatus; }

  // True once, right after a freshly installed version has been confirmed (for the event journal).
  bool takeJustConfirmed() {
    bool v = justConfirmed;
    justConfirmed = false;
    return v;
  }

  void loop() {
    if (probation) {
      if (networkUpMs && millis() - networkUpMs >= CONFIRM_AFTER_MS) {
        esp_ota_mark_app_valid_cancel_rollback();
        probation = false;
        justConfirmed = true;
        lastStatus = "v" + String(version) + " installed and confirmed";
        Serial.println("Update: " + lastStatus);
      } else if (millis() >= ROLLBACK_AFTER_MS) {
        Serial.println("Update: the new version never got online - rolling back");
        delay(200);
        esp_ota_mark_app_invalid_rollback_and_reboot();
      }
      return;   // no further updates while on probation
    }

    if (WiFi.status() != WL_CONNECTED) return;
    bool due = lastCheckMs ? millis() - lastCheckMs >= CHECK_INTERVAL_MS : millis() >= FIRST_CHECK_MS;
    if (!due && !checkRequested) return;
    checkRequested = false;
    lastCheckMs = millis();
    check();
  }

private:
  const char* manifestUrl;
  uint32_t    version;
  bool        probation      = false;
  bool        justConfirmed  = false;
  bool        checkRequested = false;
  uint32_t    networkUpMs    = 0;
  uint32_t    lastCheckMs    = 0;
  String      lastStatus     = "not checked yet";

  void fail(const String& why) {
    lastStatus = "check failed: " + why;
    Serial.println("Update: " + lastStatus);
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
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    return http.begin(client, url);
  }

  void check() {
    NetworkClientSecure client;
    HTTPClient http;
    if (!open(http, client, manifestUrl)) return fail("bad manifest URL");
    int code = http.GET();
    if (code != 200) {
      http.end();
      return fail("manifest HTTP " + String(code));
    }
    String manifest = http.getString();
    http.end();

    uint32_t available = field(manifest, "version").toInt();
    if (available <= version) {
      lastStatus = "up to date (v" + String(version) + ")";
      Serial.println("Update: " + lastStatus);
      return;
    }
    String url = field(manifest, "url");
    String sha = field(manifest, "sha256");
    size_t size = field(manifest, "size").toInt();
    if (url.isEmpty() || sha.length() != 64 || !size) return fail("incomplete manifest");
    Serial.printf("Update: v%u available (%u bytes), installing\n", (unsigned)available, (unsigned)size);
    install(url, sha, size, available);
  }

  void install(const String& url, String sha, size_t size, uint32_t available) {
    NetworkClientSecure client;
    HTTPClient http;
    if (!open(http, client, url)) return fail("bad firmware URL");
    int code = http.GET();
    if (code != 200) {
      http.end();
      return fail("firmware HTTP " + String(code));
    }
    if (!Update.begin(size)) {
      http.end();
      return fail(String("no room: ") + Update.errorString());
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    NetworkClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    size_t done = 0;
    uint32_t lastData = millis();
    while (done < size && millis() - lastData < 15000) {
      size_t avail = stream->available();
      if (!avail) {
        delay(2);
        continue;
      }
      int n = stream->readBytes(buf, min(avail, min(sizeof(buf), size - done)));
      if (n <= 0) continue;
      mbedtls_sha256_update(&ctx, buf, n);
      if (Update.write(buf, n) != (size_t)n) break;
      done += n;
      lastData = millis();
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    http.end();

    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    sha.toLowerCase();
    if (done != size) {
      Update.abort();
      return fail("download stopped at " + String(done) + " of " + String(size) + " bytes");
    }
    if (sha != hex) {
      Update.abort();
      return fail("SHA-256 mismatch");
    }
    if (!Update.end(true)) return fail(String("flash: ") + Update.errorString());

    Serial.printf("Update: v%u written, restarting\n", (unsigned)available);
    delay(300);
    ESP.restart();
  }
};
