// Furnace relay: a dumb pipe between one furnace monitor (the ESP32) and its phones, so the web app works from
// anywhere without opening anything on the home router. The ESP keeps one outbound WebSocket here; phones connect
// to the same room. Every message is end-to-end encrypted (Relay.h, web/index.html) - the relay cannot read or
// forge it. It stores only when the ESP was last seen and the phones' push subscriptions.
//
//   GET  /ws?role=device&room=<32 hex>      the ESP (one per room; a new connection replaces the old one)
//   GET  /ws?role=app&room=<32 hex>         a phone
//   GET  /push/key?room=                    the room's VAPID public key (applicationServerKey)
//   POST /push/subscribe?room=              a phone's PushSubscription (JSON) - its notifications
//   POST /push/unsubscribe?room=            {endpoint}
//   POST /push/test?room=                   {endpoint} - a test notification to that phone
//   POST /handoff?id=<32 hex>               an iPhone's pairing on its way from Safari to the home-screen icon
//   GET  /handoff?id=                       ... taken once by the icon (encrypted; gone after HANDOFF_MS)
//
// WebSocket binary messages: phone -> ESP, and ESP -> every phone of the room. Text "ping" is answered "pong" by
// the runtime itself, without waking the room up. The phones get the ESP's state as text:
//   {"t":"device","online":true|false,"since":<ms - online since / last seen>}
// The ESP's alerts come as text {"t":"push","d":"<sealed, base64url>"} and go to every subscribed phone as Web
// Push; only the phone opens "d" (web/sw.js). The relay itself tells the phones when the ESP has been gone for
// OFFLINE_ALERT_MS (a power cut or no internet at the furnace) and when it is back.
// A room is a Durable Object that hibernates between messages; while the ESP is connected, an alarm looks every
// CHECK_MS whether it is still there.

import { DurableObject } from 'cloudflare:workers';
import { newVapid, sendPush } from './webpush.js';

const ROOM = /^[0-9a-f]{32}$/;
const MAX_MESSAGE = 64 * 1024;          // a request is < 1 KB, an answer chunk ~2 KB
const MAX_APPS = 10;                    // phones connected at once; the oldest goes when another comes
const MAX_SUBSCRIPTIONS = 20;
const SILENT_MS = 100 * 1000;           // the ESP pings every 30 s: silent this long = gone
const CHECK_MS = 90 * 1000;             // how often a connected ESP is looked at (a dead TCP looks open)
const OFFLINE_ALERT_MS = 5 * 60 * 1000; // gone this long -> "Piec offline" on the phones
const HANDOFF_MS = 30 * 60 * 1000;      // a pairing waits this long for the home-screen icon
const SUBJECT = 'https://klimabartlomiej.github.io/furnace-monitor/';   // who sends, for the push services
const CORS = { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Methods': 'GET, POST, OPTIONS', 'Access-Control-Allow-Headers': 'Content-Type' };

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === 'OPTIONS') return new Response(null, { headers: CORS });
    if (url.pathname === '/handoff') {
      const id = url.searchParams.get('id') || '';
      if (!ROOM.test(id)) return new Response('bad id\n', { status: 400, headers: CORS });
      return env.ROOMS.get(env.ROOMS.idFromName('handoff:' + id)).fetch(request);
    }
    const room = url.searchParams.get('room') || '';
    const isWs = url.pathname === '/ws', isPush = url.pathname.startsWith('/push/');
    if (!isWs && !isPush) return new Response('furnace relay\n', { status: 404 });
    if (!ROOM.test(room)) return new Response('bad room\n', { status: 400, headers: CORS });
    if (isWs) {
      const role = url.searchParams.get('role');
      if (request.headers.get('Upgrade') !== 'websocket') return new Response('WebSocket expected\n', { status: 426 });
      if (role !== 'device' && role !== 'app') return new Response('bad role\n', { status: 400 });
    }
    return env.ROOMS.get(env.ROOMS.idFromName(room)).fetch(request);
  },
};

export class Room extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/handoff') return this.handoff(request);
    if (url.pathname !== '/ws') return this.push(url.pathname, request);
    const [client, server] = Object.values(new WebSocketPair());
    const at = Date.now();
    if (url.searchParams.get('role') === 'device') {
      for (const old of this.ctx.getWebSockets('device')) this.close(old, 4000, 'replaced');
      this.ctx.acceptWebSocket(server, ['device']);
      server.serializeAttachment({ at });
      const alerted = await this.ctx.storage.get('alerted');
      await this.ctx.storage.put('seen', at);
      await this.ctx.storage.delete('alerted');
      await this.ctx.storage.setAlarm(at + CHECK_MS);
      this.tell(this.ctx.getWebSockets('app'), { online: true, since: at });
      if (alerted) this.ctx.waitUntil(this.notify({ t: 'online', since: alerted, at }));
    } else {
      const apps = this.ctx.getWebSockets('app');
      if (apps.length >= MAX_APPS) this.close(apps[0], 4001, 'too many phones');
      this.ctx.acceptWebSocket(server, ['app']);
      server.serializeAttachment({ at });
      this.tell([server], await this.deviceState());
    }
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, message) {
    const fromDevice = this.ctx.getTags(ws).includes('device');
    if (typeof message === 'string') {
      if (!fromDevice || message.length > 2048) return;
      let m;
      try { m = JSON.parse(message); } catch (e) { return; }
      if (m.t === 'push' && typeof m.d === 'string') await this.notify({ d: m.d });
      return;
    }
    if (message.byteLength > MAX_MESSAGE) return this.close(ws, 1009, 'too big');
    if (fromDevice) {
      for (const app of this.ctx.getWebSockets('app')) {
        try { app.send(message); } catch (e) {}
      }
      return;
    }
    const device = await this.device();
    if (device) {
      try { device.send(message); return; } catch (e) {}
    }
    this.tell([ws], await this.deviceState());                 // nobody to ask: the phone shows "offline since"
  }

  async webSocketClose(ws) {
    if (this.ctx.getTags(ws).includes('device')) await this.lost(ws);
    this.close(ws, 1000, 'bye');
  }

  async webSocketError(ws) {
    if (this.ctx.getTags(ws).includes('device')) await this.lost(ws);
  }

  // Every CHECK_MS while the ESP is connected; once it is gone, at OFFLINE_ALERT_MS after it was last heard.
  // In a handoff's object: its time is up.
  async alarm() {
    if (await this.ctx.storage.get('handoff')) return this.ctx.storage.deleteAll();
    if (await this.device()) return this.ctx.storage.setAlarm(Date.now() + CHECK_MS);
    const seen = await this.ctx.storage.get('seen');
    if (!seen || (await this.ctx.storage.get('alerted'))) return;
    if (Date.now() - seen < OFFLINE_ALERT_MS) return this.ctx.storage.setAlarm(seen + OFFLINE_ALERT_MS);
    await this.ctx.storage.put('alerted', seen);
    await this.notify({ t: 'offline', since: seen });
  }

  // The ESP's socket - unless it has gone silent: a dead TCP connection (power cut) can look open for many
  // minutes, so a silent one is closed and the ESP counts as gone since it was last heard.
  async device() {
    for (const ws of this.ctx.getWebSockets('device')) {
      if (this.lastHeard(ws) > Date.now() - SILENT_MS) return ws;
      await this.lost(ws);
      this.close(ws, 4002, 'silent');
    }
    return null;
  }

  lastHeard(ws) {
    const ping = this.ctx.getWebSocketAutoResponseTimestamp(ws);
    const { at } = ws.deserializeAttachment() || { at: 0 };
    return Math.max(ping ? ping.getTime() : 0, at);
  }

  async deviceState() {
    const ws = await this.device();
    if (ws) return { online: true, since: ws.deserializeAttachment().at };
    return { online: false, since: (await this.ctx.storage.get('seen')) || null };
  }

  // The ESP's connection ended (unless a newer one replaced it): the phones hear it, the offline alert is armed.
  async lost(ws) {
    if (this.ctx.getWebSockets('device').some(other => other !== ws && this.lastHeard(other) > Date.now() - SILENT_MS)) return;
    const seen = this.lastHeard(ws);
    await this.ctx.storage.put('seen', seen);
    if (!(await this.ctx.storage.get('alerted'))) await this.ctx.storage.setAlarm(seen + OFFLINE_ALERT_MS);
    this.tell(this.ctx.getWebSockets('app'), { online: false, since: seen });
  }

  tell(sockets, state) {
    const text = JSON.stringify({ t: 'device', ...state });
    for (const ws of sockets) {
      try { ws.send(text); } catch (e) {}
    }
  }

  close(ws, code, reason) {
    try { ws.close(code, reason); } catch (e) {}
  }

  // ---- pairing handoff

  // An iPhone keeps Safari's and the home-screen icon's storage apart, and the icon starts without the link's
  // #fragment. So Safari leaves the pairing here - encrypted with a key that travels only in the icon's address,
  // which the relay never sees - and the icon takes it once.
  async handoff(request) {
    const headers = { ...CORS, 'Content-Type': 'text/plain' };
    if (request.method === 'POST') {
      const body = await request.text();
      if (!body || body.length > 512) return new Response('bad\n', { status: 400, headers });
      await this.ctx.storage.put('handoff', body);
      await this.ctx.storage.setAlarm(Date.now() + HANDOFF_MS);
      return new Response('ok\n', { headers });
    }
    const body = await this.ctx.storage.get('handoff');
    if (body) await this.ctx.storage.deleteAll();
    return new Response(body || '', { status: body ? 200 : 404, headers });
  }

  // ---- notifications

  async vapid() {
    let v = await this.ctx.storage.get('vapid');
    if (!v) {
      v = await newVapid();
      await this.ctx.storage.put('vapid', v);
    }
    return v;
  }

  static async subKey(endpoint) {
    const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(endpoint)));
    return 'sub:' + Array.from(hash.slice(0, 12), b => b.toString(16).padStart(2, '0')).join('');
  }

  // To every subscribed phone; the ones whose subscription has expired are forgotten.
  async notify(message, only) {
    const subs = only ? new Map([[await Room.subKey(only.endpoint), only]]) : await this.ctx.storage.list({ prefix: 'sub:' });
    if (!subs.size) return;
    const vapid = await this.vapid(), payload = JSON.stringify(message);
    await Promise.all([...subs].map(async ([key, sub]) => {
      try {
        const status = await sendPush(sub, payload, vapid, SUBJECT);
        if (status === 404 || status === 410) await this.ctx.storage.delete(key);
      } catch (e) {}
    }));
  }

  async push(path, request) {
    const json = (body, status = 200) => new Response(JSON.stringify(body), { status, headers: { ...CORS, 'Content-Type': 'application/json' } });
    if (path === '/push/key') return json({ key: (await this.vapid()).publicKey });
    if (request.method !== 'POST') return json({ error: 'POST expected' }, 405);
    let body;
    try { body = await request.json(); } catch (e) { return json({ error: 'bad JSON' }, 400); }
    if (!body || typeof body.endpoint !== 'string' || !/^https:\/\//.test(body.endpoint)) return json({ error: 'bad endpoint' }, 400);
    const key = await Room.subKey(body.endpoint);
    if (path === '/push/unsubscribe') {
      await this.ctx.storage.delete(key);
      return json({ ok: true });
    }
    const stored = await this.ctx.storage.get(key);
    if (path === '/push/test') {
      if (!stored) return json({ error: 'not subscribed' }, 404);
      await this.notify({ t: 'test' }, stored);
      return json({ ok: true });
    }
    if (path !== '/push/subscribe') return json({ error: 'not found' }, 404);
    if (!body.keys || typeof body.keys.p256dh !== 'string' || typeof body.keys.auth !== 'string') return json({ error: 'bad keys' }, 400);
    if (!stored && (await this.ctx.storage.list({ prefix: 'sub:' })).size >= MAX_SUBSCRIPTIONS) return json({ error: 'too many phones' }, 409);
    await this.ctx.storage.put(key, { endpoint: body.endpoint, keys: { p256dh: body.keys.p256dh, auth: body.keys.auth } });
    return json({ ok: true });
  }
}
