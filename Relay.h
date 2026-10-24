// Remote access: the ESP keeps one outbound WebSocket to a relay (a Cloudflare Worker - relay/ in this repo) and the
// web app, opened from GitHub Pages anywhere, talks to the ESP through it. Outbound only: no port forwarding, works
// behind any router or CGNAT. The relay only passes messages on - each one is AES-256-GCM encrypted with a key that
// never leaves the ESP and the paired phones (the pairing code), so the relay sees noise and stores nothing.
//
// A phone's request is answered by the ESP's own web server: the task replays it over HTTP to 127.0.0.1 and sends
// the answer back in encrypted chunks. The API is exactly the one the app uses on the home WiFi, and the loop stays
// the only place that reads or changes the furnace's state. A task of its own does the network, so the readings and
// HomeKit never wait for the relay.
//
// Keys: a 32-byte secret (flash "relayKey"); key = SHA-256("furnace-key" | secret), room = the first 16 bytes of
// SHA-256("furnace-room" | secret) in hex - the relay's name for this ESP and its phones.
// Pairing code: "<relay host>/<secret, base64url>".
// WebSocket binary messages, both ways: nonce[12] | ciphertext | tag[16]; AAD 'Q' for requests, 'R' for answers.
// Plaintext (little-endian):
//   request:  2 | id u32 | the phone's Unix time u32 | method u8 (0 GET, 1 POST) | path length u16 | path | body
//   answer:   1 | id u32 | seq u16 | last u8 | HTTP status u16 | body chunk
// Text messages: "ping" / "pong" (the relay answers pings itself, without waking up), and the furnace's alerts
// for the phones' notifications: {"t":"push","c":"<kind>","d":"<nonce | ciphertext | tag, base64url>"}, AAD 'P'
// - the relay hands "d" to the push services as it is (only the phone's service worker opens it, web/sw.js) to
// the phones that want that kind (alarm, warn, power, water, update).

#pragma once

#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <time.h>

class Relay {
public:
  static constexpr uint32_t PING_MS         = 30 * 1000;   // keeps the router's NAT entry, proves the ESP is alive
  static constexpr uint32_t PONG_WAIT_MS    = 15 * 1000;   // no answer by then = the connection is dead
  static constexpr uint32_t REQUEST_SKEW_S  = 300;         // a request this far off the clock is refused (replays)
  static constexpr uint32_t LOCAL_WAIT_MS   = 15 * 1000;   // the web server's answer may take this long
  static constexpr size_t   CHUNK           = 2048;        // answer bytes per message
  static constexpr size_t   MAX_REQUEST     = 1024;        // largest message accepted from the relay
  static constexpr uint8_t  ALERTS          = 8;           // notifications waiting for the relay at most

  void begin(uint16_t webPort, const String& host, const uint8_t secret[32]) {
    port = webPort;
    alerts = xQueueCreate(ALERTS, sizeof(Alert));
    configure(host, secret);
    xTaskCreatePinnedToCore(task, "relay", 10240, this, 1, nullptr, 0);
  }

  // A new relay address or key (empty host = remote access off); the task reconnects with it.
  void configure(const String& host, const uint8_t secret[32]) {
    uint8_t k[32], r[32];
    derive("furnace-key", secret, k);
    derive("furnace-room", secret, r);
    char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02x", r[i]);
    portENTER_CRITICAL(&mux);
    strlcpy(cfgHost, host.c_str(), sizeof(cfgHost));
    memcpy(cfgSecret, secret, 32);
    memcpy(cfgKey, k, 32);
    memcpy(cfgRoom, hex, sizeof(cfgRoom));
    cfgVersion++;
    portEXIT_CRITICAL(&mux);
  }

  // A firmware download needs the memory of a second TLS connection: the relay steps aside meanwhile. Called
  // from the update task, pause(true) waits until the connection is really closed - an answer being passed on
  // to a phone finishes first (measured: a fixed 1.5 s was not enough while the app was busy) - at most 20 s.
  void pause(bool p) {
    paused = p;
    for (int i = 0; p && connectedSince && i < 200; i++) vTaskDelay(pdMS_TO_TICKS(100));
  }

  // A notification for the paired phones (a JSON the phone words, web/sw.js). Kept until the relay is reached -
  // a power cut's alert goes out after the restart; when too many wait, the oldest goes.
  void alert(const String& json, const char* kind) {
    if (!alerts) return;
    Alert a;
    a.len = min((size_t)json.length(), sizeof(a.text));
    memcpy(a.text, json.c_str(), a.len);
    strlcpy(a.kind, kind, sizeof(a.kind));
    if (xQueueSend(alerts, &a, 0) != pdTRUE) {
      Alert oldest;
      xQueueReceive(alerts, &oldest, 0);
      xQueueSend(alerts, &a, 0);
    }
  }

  String host() const {
    portENTER_CRITICAL(&mux);
    String h = cfgHost;
    portEXIT_CRITICAL(&mux);
    return h;
  }

  // What the phones need: "<host>/<secret>", empty while remote access is off.
  String pairingCode() const {
    uint8_t s[32];
    portENTER_CRITICAL(&mux);
    String h = cfgHost;
    memcpy(s, cfgSecret, 32);
    portEXIT_CRITICAL(&mux);
    return h.length() ? h + "/" + base64(s, 32, true) : String();
  }

  // "off", "no WiFi", "connecting", "connected", "paused" or "error: ..." - the web app shows it in Polish.
  String state() const {
    portENTER_CRITICAL(&mux);
    String s = stateText;
    portEXIT_CRITICAL(&mux);
    return s;
  }
  uint32_t connectedAt() const { return connectedSince; }   // Unix time, 0 = not connected
  uint32_t served() const { return requests; }

  static String base64(const uint8_t* p, size_t n, bool url) {
    const char* abc = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                          : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String s;
    for (size_t i = 0; i < n; i += 3) {
      uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
      s += abc[v >> 18 & 63];
      s += abc[v >> 12 & 63];
      if (i + 1 < n) s += abc[v >> 6 & 63]; else if (!url) s += '=';
      if (i + 2 < n) s += abc[v & 63];      else if (!url) s += '=';
    }
    return s;
  }

private:
  enum : uint8_t { OP_TEXT = 1, OP_BINARY = 2, OP_CLOSE = 8, OP_PING = 9, OP_PONG = 10 };
  static constexpr size_t HEAD = 14;                    // room for a frame header in front of the payload

  mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  char     cfgHost[64]   = "";
  uint8_t  cfgSecret[32] = {};
  uint8_t  cfgKey[32]    = {};
  char     cfgRoom[33]   = "";
  volatile uint32_t cfgVersion = 0;
  volatile bool     paused     = false;
  char     stateText[64] = "off";
  volatile uint32_t connectedSince = 0;
  volatile uint32_t requests = 0;
  uint16_t port = 80;

  struct Alert {
    uint8_t len;
    char    kind[8];
    char    text[160];
  };
  QueueHandle_t alerts = nullptr;

  // The task's own copies and buffers.
  char     host_[64];
  char     room[33];
  mbedtls_gcm_context gcm;
  uint8_t  in[MAX_REQUEST];
  uint8_t  out[HEAD + 12 + 10 + CHUNK + 16];

  static void task(void* self) { static_cast<Relay*>(self)->run(); }

  void setState(const String& s) {
    portENTER_CRITICAL(&mux);
    strlcpy(stateText, s.c_str(), sizeof(stateText));
    portEXIT_CRITICAL(&mux);
  }

  static void derive(const char* label, const uint8_t secret[32], uint8_t out[32]) {
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const uint8_t*)label, strlen(label));
    mbedtls_sha256_update(&c, secret, 32);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
  }

  static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
  static uint16_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }
  static void wr32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
  static void wr16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

  void run() {
    mbedtls_gcm_init(&gcm);
    uint32_t backoff = 0;
    for (;;) {
      uint8_t key[32];
      portENTER_CRITICAL(&mux);
      uint32_t version = cfgVersion;
      strlcpy(host_, cfgHost, sizeof(host_));
      memcpy(room, cfgRoom, sizeof(room));
      memcpy(key, cfgKey, 32);
      portEXIT_CRITICAL(&mux);
      if (!host_[0]) {
        setState("off");
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      if (paused) {
        setState("paused");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      if (WiFi.status() != WL_CONNECTED) {
        setState("no WiFi");
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
      setState("connecting");
      uint32_t start = millis();
      String why = session(version);
      connectedSince = 0;
      setState(why);
      // Right back after a connection that worked for a while; longer and longer while it keeps failing.
      backoff = millis() - start > 60000 ? 3000 : min(backoff ? backoff * 2 : 5000, (uint32_t)120000);
      if (why == "reconnecting" || why == "paused") backoff = 0;
      for (uint32_t waited = 0; waited < backoff && cfgVersion == version; waited += 500) vTaskDelay(pdMS_TO_TICKS(500));
    }
  }

  static String tlsError(NetworkClientSecure& tls, const char* what) {
    char e[80];
    return tls.lastError(e, sizeof(e)) ? String(what) + " (" + e + ")" : String(what);
  }

  // One connection to the relay; returns why it ended.
  String session(uint32_t version) {
    NetworkClientSecure tls;
    tls.setInsecure();               // no CA pinned: what the relay carries is end-to-end encrypted anyway
    tls.setHandshakeTimeout(10);
    tls.setConnectionTimeout(10000);
    if (!tls.connect(host_, 443)) return "error: " + tlsError(tls, "no connection");
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    String req = String("GET /ws?role=device&room=") + room + " HTTP/1.1\r\nHost: " + host_ +
                 "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                 base64(nonce, sizeof(nonce), false) + "\r\nUser-Agent: furnace-monitor\r\n\r\n";
    tls.write((const uint8_t*)req.c_str(), req.length());
    String head = readHead(tls, 10000);
    if (!head.startsWith("HTTP/1.1 101")) {
      String line = head.substring(0, head.indexOf('\r'));
      return "error: relay answered " + (line.length() ? line : String("nothing"));
    }
    connectedSince = time(nullptr);
    setState("connected");

    uint32_t lastPing = millis(), pingSent = 0;
    for (;;) {
      if (cfgVersion != version) return "reconnecting";
      if (paused) {
        sendFrame(tls, OP_CLOSE, 0);
        return "paused";
      }
      if (WiFi.status() != WL_CONNECTED) return "no WiFi";
      uint32_t ms = millis();
      if (pingSent && ms - pingSent > PONG_WAIT_MS) return "error: the relay stopped answering";
      if (!pingSent && ms - lastPing >= PING_MS) {
        memcpy(out + HEAD, "ping", 4);
        if (!sendFrame(tls, OP_TEXT, 4)) return "error: " + tlsError(tls, "send failed");
        pingSent = lastPing = ms;
      }
      Alert a;
      if (alerts && xQueuePeek(alerts, &a, 0) == pdTRUE) {
        if (!sendAlert(tls, a)) return "error: " + tlsError(tls, "send failed");
        xQueueReceive(alerts, &a, 0);
      }
      int avail = tls.available();
      if (avail <= 0) {
        if (!tls.connected()) return "error: " + tlsError(tls, "connection lost");
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      uint8_t op;
      size_t len;
      if (!readFrame(tls, op, len)) return "error: " + tlsError(tls, "broken message");
      if (op == OP_TEXT && len == 4 && !memcmp(in, "pong", 4)) pingSent = 0;
      else if (op == OP_BINARY) answer(tls, len);
      else if (op == OP_PING) {
        memcpy(out + HEAD, in, len);
        sendFrame(tls, OP_PONG, len);
      } else if (op == OP_CLOSE) return "error: closed by the relay";
    }
  }

  // Status line and headers, up to the blank line (byte by byte: nothing after it is consumed).
  static String readHead(Client& c, uint32_t timeoutMs) {
    String h;
    uint32_t start = millis();
    while (millis() - start < timeoutMs && h.length() < 2048) {
      if (c.available() <= 0) {
        if (!c.connected()) break;
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      int ch = c.read();
      if (ch < 0) continue;
      h += (char)ch;
      if (h.endsWith("\r\n\r\n")) break;
    }
    return h;
  }

  static bool readExact(NetworkClientSecure& tls, uint8_t* p, size_t n) {
    uint32_t start = millis();
    while (n) {
      int avail = tls.available();
      if (avail <= 0) {
        if (!tls.connected() || millis() - start > 10000) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      int r = tls.read(p, min((size_t)avail, n));
      if (r <= 0) return false;
      p += r;
      n -= r;
    }
    return true;
  }

  // One message from the relay into `in`. Server frames come unmasked and, from this relay, never fragmented.
  bool readFrame(NetworkClientSecure& tls, uint8_t& op, size_t& len) {
    uint8_t h[8];
    if (!readExact(tls, h, 2)) return false;
    op = h[0] & 0x0F;
    bool fin = h[0] & 0x80, masked = h[1] & 0x80;
    uint64_t n = h[1] & 0x7F;
    if (n == 126) {
      if (!readExact(tls, h, 2)) return false;
      n = h[0] << 8 | h[1];
    } else if (n == 127) {
      if (!readExact(tls, h, 8)) return false;
      n = 0;
      for (int i = 0; i < 8; i++) n = n << 8 | h[i];
    }
    uint8_t mask[4] = {};
    if (!fin || n > sizeof(in) || (masked && !readExact(tls, mask, 4)) || !readExact(tls, in, n)) return false;
    if (masked) for (size_t i = 0; i < n; i++) in[i] ^= mask[i & 3];
    len = n;
    return true;
  }

  // Sends the n payload bytes at out + HEAD as one masked frame (a client must mask).
  bool sendFrame(NetworkClientSecure& tls, uint8_t op, size_t n) {
    uint8_t* payload = out + HEAD;
    size_t hn = n < 126 ? 6 : 8;
    uint8_t* h = payload - hn;
    h[0] = 0x80 | op;
    if (n < 126) {
      h[1] = 0x80 | n;
    } else {
      h[1] = 0x80 | 126;
      h[2] = n >> 8;
      h[3] = n;
    }
    uint8_t* mask = payload - 4;
    esp_fill_random(mask, 4);
    for (size_t i = 0; i < n; i++) payload[i] ^= mask[i & 3];
    return tls.write(h, hn + n) == hn + n;
  }

  bool sendAlert(NetworkClientSecure& tls, const Alert& a) {
    uint8_t sealed[12 + sizeof(a.text) + 16];
    esp_fill_random(sealed, 12);
    const uint8_t aad = 'P';
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, a.len, sealed, 12, &aad, 1, (const uint8_t*)a.text, sealed + 12, 16,
                              sealed + 12 + a.len);
    String msg = "{\"t\":\"push\",\"c\":\"" + String(a.kind) + "\",\"d\":\"" + base64(sealed, 12 + a.len + 16, true) + "\"}";
    memcpy(out + HEAD, msg.c_str(), msg.length());
    return sendFrame(tls, OP_TEXT, msg.length());
  }

  // A phone's request: replayed to the ESP's own web server, its answer sent back.
  void answer(NetworkClientSecure& tls, size_t len) {
    if (len < 12 + 16 + 12) return;
    size_t n = len - 28;
    const uint8_t aad = 'Q';
    uint8_t* p = in + 12;
    if (mbedtls_gcm_auth_decrypt(&gcm, n, in, 12, &aad, 1, in + 12 + n, 16, p, p) != 0) return;   // not our key
    if (p[0] != 2) return;
    uint32_t id = rd32(p + 1), sent = rd32(p + 5);
    uint8_t method = p[9];
    uint16_t pathLen = rd16(p + 10);
    if (method > 1 || 12 + (size_t)pathLen > n) return;
    String path((const char*)p + 12, pathLen);
    String body((const char*)p + 12 + pathLen, n - 12 - pathLen);
    requests++;
    uint32_t now = time(nullptr);
    if (now >= 1700000000 && (sent + REQUEST_SKEW_S < now || sent > now + REQUEST_SKEW_S))
      reply(tls, id, 401, "check the phone's clock");
    else if (!path.startsWith("/api/") || path.startsWith("/api/firmware"))
      reply(tls, id, 404, "not found");
    else
      forward(tls, id, method, path, body);
  }

  void reply(NetworkClientSecure& tls, uint32_t id, uint16_t status, const char* text) {
    size_t n = strlen(text);
    memcpy(out + HEAD + 12 + 10, text, n);
    sendChunk(tls, id, 0, true, status, n);
  }

  // The n body bytes at out + HEAD + 22 become one sealed answer frame.
  bool sendChunk(NetworkClientSecure& tls, uint32_t id, uint16_t seq, bool last, uint16_t status, size_t n) {
    uint8_t* nonce = out + HEAD;
    uint8_t* p = nonce + 12;
    p[0] = 1;
    wr32(p + 1, id);
    wr16(p + 5, seq);
    p[7] = last;
    wr16(p + 8, status);
    n += 10;
    esp_fill_random(nonce, 12);
    const uint8_t aad = 'R';
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, n, nonce, 12, &aad, 1, p, p, 16, p + n);
    return sendFrame(tls, OP_BINARY, 12 + n + 16);
  }

  // The local web server's answer, passed on as it comes. Its end is taken from the answer itself - a length, or
  // the last piece of a chunked one: NetworkClient does not notice in time that the server closed the connection.
  void forward(NetworkClientSecure& tls, uint32_t id, uint8_t method, const String& path, const String& body) {
    NetworkClient local;
    if (!local.connect(IPAddress(127, 0, 0, 1), port, 3000)) {
      reply(tls, id, 503, "web server busy");
      return;
    }
    String req = String(method ? "POST " : "GET ") + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    if (method) req += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " + String(body.length()) + "\r\n";
    req += "\r\n" + body;
    local.write((const uint8_t*)req.c_str(), req.length());
    String head = readHead(local, LOCAL_WAIT_MS);
    int status = head.startsWith("HTTP/1.") && head.length() > 12 ? head.substring(9, 12).toInt() : 0;
    if (!status) {
      reply(tls, id, 504, "no answer");
      return;
    }
    head.toLowerCase();
    int length = head.indexOf("content-length:");
    answerId = id;
    answerStatus = status;
    answerSeq = 0;
    answerHave = 0;
    if (head.indexOf("transfer-encoding: chunked") >= 0) {
      for (;;) {
        size_t n = strtoul(readLine(local).c_str(), nullptr, 16);
        if (!n) break;                                      // the last piece (or nothing more came)
        if (!copy(tls, local, n, LOCAL_WAIT_MS)) break;
        readLine(local);                                    // the CRLF after each piece
      }
    } else if (length >= 0) {
      copy(tls, local, strtoul(head.c_str() + length + 15, nullptr, 10), LOCAL_WAIT_MS);
    } else {
      copy(tls, local, SIZE_MAX, 1000);                    // no length given: until it goes quiet
    }
    sendChunk(tls, answerId, answerSeq, true, answerStatus, answerHave);
  }

  uint32_t answerId     = 0;      // the answer being passed on: full chunks go at once, the rest with the last one
  uint16_t answerStatus = 0;
  uint16_t answerSeq    = 0;
  size_t   answerHave   = 0;

  static bool waitLocal(NetworkClient& c, uint32_t waitMs) {
    uint32_t start = millis();
    while (c.available() <= 0) {
      if (millis() - start > waitMs) return false;
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
  }

  static String readLine(NetworkClient& c) {
    String line;
    while (line.length() < 32 && waitLocal(c, LOCAL_WAIT_MS)) {
      int ch = c.read();
      if (ch == '\n') break;
      if (ch >= 0 && ch != '\r') line += (char)ch;
    }
    return line;
  }

  // n body bytes from the local server into the answer.
  bool copy(NetworkClientSecure& tls, NetworkClient& c, size_t n, uint32_t waitMs) {
    uint8_t* body0 = out + HEAD + 12 + 10;
    while (n) {
      if (!waitLocal(c, waitMs)) return false;
      int r = c.read(body0 + answerHave, min(n, CHUNK - answerHave));
      if (r <= 0) return false;
      answerHave += r;
      n -= r;
      if (answerHave == CHUNK) {
        if (!sendChunk(tls, answerId, answerSeq++, false, answerStatus, answerHave)) return false;
        answerHave = 0;
      }
    }
    return true;
  }
};
