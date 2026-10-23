// Telegram bot transport: the ESP talks to api.telegram.org itself (outbound HTTPS - no port forwarding, no server,
// free). A task of its own does the network: long polling for new messages and sending what the sketch queued, so
// the readings and HomeKit never wait for Telegram. The sketch decides what to say and to whom (PiecTemp.ino,
// "Telegram"): everything that reads or changes the furnace's state stays in the loop - this file only moves
// messages through two queues.

#pragma once

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

class TelegramBot {
public:
  // A message for one chat, or a reply to a button press. Buttons: "label=data|label=data" per row, rows split by
  // '\n'; `menu` attaches the keyboard at the bottom of the chat.
  struct Out {
    enum Kind : uint8_t { SEND, ANSWER, CLEAR_BUTTONS };
    int64_t chat;
    Kind    kind;
    bool    menu;
    int32_t messageId;          // CLEAR_BUTTONS: the message whose buttons go
    char    callbackId[24];     // ANSWER: the button press being answered (text = the short pop-up)
    char    text[700];
    char    buttons[220];
  };
  // A message or a button press from someone.
  struct In {
    int64_t chat;
    int32_t messageId;
    bool    button;
    char    callbackId[24];
    char    name[24];
    char    text[64];           // the message, or the pressed button's data
  };

  void begin() {
    outbox = xQueueCreate(10, sizeof(Out));
    inbox = xQueueCreate(8, sizeof(In));
    xTaskCreatePinnedToCore(task, "telegram", 12288, this, 1, nullptr, 0);
  }

  // The bot's token from @BotFather; empty switches the bot off. Checked with getMe on the next round.
  void setToken(const String& t) {
    strlcpy(token, t.c_str(), sizeof(token));
    tokenVersion++;
  }
  bool configured() const { return token[0]; }
  const char* botName() const { return bot; }       // "@name" once getMe answered
  const char* status() const { return state; }      // "ok", or what went wrong last

  bool send(const Out& o) { return outbox && xQueueSend(outbox, &o, 0) == pdTRUE; }
  bool receive(In& in) { return inbox && xQueueReceive(inbox, &in, 0) == pdTRUE; }

private:
  QueueHandle_t   outbox = nullptr;
  QueueHandle_t   inbox  = nullptr;
  char            token[64] = "";
  volatile uint32_t tokenVersion = 0;
  char            bot[40]   = "";
  char            state[48] = "not set";

  static void task(void* self) { static_cast<TelegramBot*>(self)->run(); }

  void setState(const char* s) { strlcpy(state, s, sizeof(state)); }

  void run() {
    uint32_t seenVersion = 0;
    int64_t offset = 0;
    for (;;) {
      if (!token[0] || WiFi.status() != WL_CONNECTED) {
        if (!token[0]) setState("not set");
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }
      if (seenVersion != tokenVersion) {               // a new token: who is the bot?
        seenVersion = tokenVersion;
        bot[0] = 0;
        offset = 0;
        checkBot();
      }
      Out o;
      while (xQueueReceive(outbox, &o, 0) == pdTRUE) deliver(o);
      // Waits up to 10 s for new messages - unless something is waiting to go out.
      poll(offset, uxQueueMessagesWaiting(outbox) ? 0 : 10);
    }
  }

  String url(const char* method) const { return String("https://api.telegram.org/bot") + token + "/" + method; }

  // One HTTPS call; the JSON reply in `reply`. False with the state set when it failed.
  bool call(const char* method, const String& body, String& reply, uint16_t waitS = 0) {
    NetworkClientSecure client;
    client.setInsecure();                                 // like the updates; the token is still encrypted
    client.setHandshakeTimeout(10);
    HTTPClient http;
    http.setTimeout((waitS + 10) * 1000);
    if (!http.begin(client, url(method))) return setState("no connection"), false;
    int code;
    if (body.length()) {
      http.addHeader("Content-Type", "application/json");
      code = http.POST(body);
    } else {
      code = http.GET();
    }
    reply = code > 0 ? http.getString() : "";
    http.end();
    if (code == 200) return setState("ok"), true;
    if (code == 401 || code == 404) setState("wrong token");
    else if (code < 0) setState("no internet");
    else setState((String("error ") + code).c_str());
    return false;
  }

  void checkBot() {
    String reply;
    if (!call("getMe", "", reply)) return;
    JsonDocument doc;
    if (deserializeJson(doc, reply)) return;
    snprintf(bot, sizeof(bot), "@%s", doc["result"]["username"] | "");
  }

  static void addButtons(JsonObject markup, const char* spec) {
    JsonArray rows = markup["inline_keyboard"].to<JsonArray>();
    String all = spec;
    for (int start = 0; start < (int)all.length();) {
      int end = all.indexOf('\n', start);
      if (end < 0) end = all.length();
      String line = all.substring(start, end);
      JsonArray row = rows.add<JsonArray>();
      for (int a = 0; a < (int)line.length();) {
        int b = line.indexOf('|', a);
        if (b < 0) b = line.length();
        String item = line.substring(a, b);
        int eq = item.lastIndexOf('=');
        JsonObject button = row.add<JsonObject>();
        button["text"] = item.substring(0, eq);
        button["callback_data"] = item.substring(eq + 1);
        a = b + 1;
      }
      start = end + 1;
    }
  }

  void deliver(const Out& o) {
    JsonDocument doc;
    const char* method = "sendMessage";
    if (o.kind == Out::ANSWER) {
      method = "answerCallbackQuery";
      doc["callback_query_id"] = o.callbackId;
      if (o.text[0]) doc["text"] = o.text;
    } else if (o.kind == Out::CLEAR_BUTTONS) {
      method = "editMessageReplyMarkup";
      doc["chat_id"] = o.chat;
      doc["message_id"] = o.messageId;
      doc["reply_markup"]["inline_keyboard"].to<JsonArray>();
    } else {
      doc["chat_id"] = o.chat;
      doc["text"] = o.text;
      if (o.buttons[0]) {
        addButtons(doc["reply_markup"].to<JsonObject>(), o.buttons);
      } else if (o.menu) {
        JsonObject k = doc["reply_markup"].to<JsonObject>();
        k["resize_keyboard"] = true;
        k["is_persistent"] = true;
        const char* labels[2][2] = {{"Stan", "Dosypałem"}, {"Popielnik", "Dziennik"}};
        JsonArray rows = k["keyboard"].to<JsonArray>();
        for (auto& r : labels) {
          JsonArray row = rows.add<JsonArray>();
          for (const char* l : r) row.add<JsonObject>()["text"] = l;
        }
      }
    }
    String body, reply;
    serializeJson(doc, body);
    call(method, body, reply);
  }

  void poll(int64_t& offset, uint16_t waitS) {
    String reply;
    String q = String("?timeout=") + waitS + "&limit=5&allowed_updates=%5B%22message%22,%22callback_query%22%5D";
    if (offset) q += String("&offset=") + String((long long)offset);
    NetworkClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(10);
    HTTPClient http;
    http.setTimeout((waitS + 10) * 1000);
    if (!http.begin(client, url("getUpdates") + q)) {
      setState("no connection");
      vTaskDelay(pdMS_TO_TICKS(5000));
      return;
    }
    int code = http.GET();
    if (code == 200) reply = http.getString();
    http.end();
    if (code != 200) {
      setState(code == 401 || code == 404 ? "wrong token" : code < 0 ? "no internet" : "error");
      vTaskDelay(pdMS_TO_TICKS(code == 401 || code == 404 ? 60000 : 10000));
      return;
    }
    setState("ok");

    JsonDocument filter;
    JsonObject f = filter["result"].add<JsonObject>();
    f["update_id"] = true;
    f["message"]["message_id"] = true;
    f["message"]["chat"]["id"] = true;
    f["message"]["from"]["first_name"] = true;
    f["message"]["text"] = true;
    f["callback_query"]["id"] = true;
    f["callback_query"]["data"] = true;
    f["callback_query"]["from"]["first_name"] = true;
    f["callback_query"]["message"]["message_id"] = true;
    f["callback_query"]["message"]["chat"]["id"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, reply, DeserializationOption::Filter(filter))) return;
    for (JsonObject u : doc["result"].as<JsonArray>()) {
      offset = (u["update_id"] | (int64_t)0) + 1;
      In in = {};
      JsonObject cb = u["callback_query"];
      in.button = !cb.isNull();
      JsonObject m = in.button ? cb["message"].as<JsonObject>() : u["message"].as<JsonObject>();
      if (m.isNull()) continue;
      in.chat = m["chat"]["id"] | (int64_t)0;
      in.messageId = m["message_id"] | 0;
      JsonObject from = in.button ? cb["from"].as<JsonObject>() : m["from"].as<JsonObject>();
      strlcpy(in.name, from["first_name"] | "", sizeof(in.name));
      if (in.button) {
        strlcpy(in.callbackId, cb["id"] | "", sizeof(in.callbackId));
        strlcpy(in.text, cb["data"] | "", sizeof(in.text));
      } else {
        strlcpy(in.text, m["text"] | "", sizeof(in.text));
      }
      if (in.chat) xQueueSend(inbox, &in, 0);
    }
  }
};
