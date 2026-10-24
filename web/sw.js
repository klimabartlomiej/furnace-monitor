// Service worker of the remote app (GitHub Pages): the furnace's notifications. The relay brings either its own
// news (the ESP is offline / back, a test) or the ESP's alert sealed with the pairing key (Relay.h), opened here -
// the pairing code is kept in IndexedDB by the page when notifications are switched on.

const WARNINGS = ['Piec wygasa', 'Ryzyko zamarznięcia', 'Cofanie żaru', 'Mało węgla', 'Piec stoi po zaniku prądu', 'Opróżnij popielnik'];
const LOCALE = 'pl-PL';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
self.addEventListener('push', e => e.waitUntil(onPush(e)));
self.addEventListener('notificationclick', e => {
  e.notification.close();
  e.waitUntil((async () => {
    const open = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
    if (open.length) return open[0].focus();
    return self.clients.openWindow('./');
  })());
});

function idbGet(key) {
  return new Promise(resolve => {
    const req = indexedDB.open('furnace', 1);
    req.onupgradeneeded = () => req.result.createObjectStore('kv');
    req.onerror = () => resolve(null);
    req.onsuccess = () => {
      const get = req.result.transaction('kv').objectStore('kv').get(key);
      get.onsuccess = () => resolve(get.result || null);
      get.onerror = () => resolve(null);
    };
  });
}

const b64url = s => Uint8Array.from(atob(s.replace(/-/g, '+').replace(/_/g, '/')), c => c.charCodeAt(0));
const n1 = v => v.toFixed(1).replace('.', ',');
const hhmm = ms => new Date(ms).toLocaleTimeString(LOCALE, { hour: '2-digit', minute: '2-digit' });
const dur = s => { const m = Math.round(s / 60), h = Math.floor(m / 60); return h ? `${h} godz. ${m % 60} min` : `${m} min`; };
const durCut = s => (s < 60 ? `${s} s` : dur(s));
const plural = (n, one, few, many) => n === 1 ? one : n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14) ? few : many;
const buckets = b => { const r = b >= 20 ? Math.round(b) : Math.round(b * 2) / 2; return `${String(r).replace('.', ',')} ${r === 1 ? 'wiadro' : Number.isInteger(r) ? plural(r, 'wiadro', 'wiadra', 'wiader') : 'wiadra'}`; };

// The ESP's alert: the pairing code -> key (as in Relay.h), AES-GCM with AAD 'P'.
async function openAlert(sealed) {
  const code = await idbGet('pair');
  const secret = code && code.split('/')[1];
  if (!secret) return null;
  try {
    const label = new TextEncoder(), s = b64url(secret);
    const k = await crypto.subtle.digest('SHA-256', new Uint8Array([...label.encode('furnace-key'), ...s]));
    const key = await crypto.subtle.importKey('raw', k, 'AES-GCM', false, ['decrypt']);
    const u = b64url(sealed);
    const plain = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: u.subarray(0, 12), additionalData: new Uint8Array([80]) }, key, u.subarray(12));
    return JSON.parse(new TextDecoder().decode(plain));
  } catch (e) {
    return null;
  }
}

// The same words as the journal in the app (eventView in index.html).
function describe(a) {
  const v = a.v === -32768 ? null : a.v / 100, temp = v === null ? '' : `${n1(v)}°`, name = a.n || '';
  const warn = WARNINGS[a.s] || 'Ostrzeżenie';
  switch (a.e) {
    case 2: return { title: 'Piec – zaktualizowano', body: `Działa wersja ${a.v}.`, tag: 'update' };
    case 3: return { title: `${name}: za gorąco`, body: `${temp} – przekroczony próg maks.`, tag: `alarm-${a.s}` };
    case 4: return { title: `${name}: za zimno`, body: `${temp} – poniżej progu min.`, tag: `alarm-${a.s}` };
    case 5: return { title: `${name}: wróciło do normy`, body: temp, tag: `alarm-${a.s}` };
    case 6: return { title: `${name}: czujnik nie odpowiada`, body: 'Sprawdź przewód czujnika.', tag: `fault-${a.s}` };
    case 7: return { title: `${name}: czujnik znów działa`, body: temp, tag: `fault-${a.s}` };
    case 8: return { title: warn, body: a.s === 3 ? (v !== null ? `Zostało ~${buckets(v)} węgla.` : '') : a.s === 5 ? (v !== null ? `Spalono ~${buckets(v)} od opróżnienia.` : '') : temp, tag: `warn-${a.s}` };
    case 9: return { title: `${warn} – koniec`, body: a.s === 3 ? '' : temp, tag: `warn-${a.s}` };
    case 14: return { title: 'Ciepła woda gotowa', body: `${name} ${temp} – można się kąpać.`, tag: 'water' };
    case 101: {                                         // the morning summary of the day before
      const parts = [];
      if (a.b !== undefined) parts.push(`${buckets(a.b / 10)} węgla${a.z !== undefined ? ` (≈${a.z} zł)` : ''}`);
      parts.push(`piec palił ${dur(a.m * 60)}`);
      if (a.lo !== undefined) parts.push(`piec ${n1(a.lo / 100)}–${n1(a.hi / 100)}°`);
      parts.push(a.pc ? `${a.pc} ${plural(a.pc, 'zanik', 'zaniki', 'zaników')} prądu (${durCut(a.ps)})` : 'bez zaników prądu');
      return { title: 'Wczoraj przy piecu', body: parts.join(' · '), tag: 'summary' };
    }
    case 100: return { title: 'Dostępna nowa wersja', body: `Wersja ${a.v} – otwórz aplikację i stuknij „Zainstaluj”.`, tag: 'update' };
    case 11: {
      const sec = (a.s & 0x7F) * 65536 + (a.v & 0xFFFF);
      return { title: 'Był brak prądu', body: `${hhmm((a.t - sec) * 1000)}–${hhmm(a.t * 1000)} · ${dur(sec)}`, tag: 'power' };
    }
    default: return { title: 'Piec', body: 'Nowe zdarzenie – otwórz aplikację.', tag: 'furnace' };
  }
}

async function onPush(e) {
  let m = {};
  try { m = e.data ? e.data.json() : {}; } catch (err) {}
  let n = { title: 'Piec', body: 'Nowe zdarzenie – otwórz aplikację.', tag: 'furnace' };
  if (m.t === 'offline') n = { title: 'Piec offline', body: `Centralka bez połączenia od ${hhmm(m.since)} – brak prądu lub internetu?`, tag: 'device' };
  else if (m.t === 'online') n = { title: 'Piec znów online', body: `Przerwa ${dur((m.at - m.since) / 1000)}, od ${hhmm(m.since)}.`, tag: 'device' };
  else if (m.t === 'test') n = { title: 'Piec', body: 'Powiadomienia działają ✅', tag: 'test' };
  else if (m.d) {
    const a = await openAlert(m.d);
    if (a) {
      n = describe(a);
      if (a.t && Date.now() / 1000 - a.t > 300) n.body += ` (o ${hhmm(a.t * 1000)})`;   // held back, e.g. by a power cut
    }
  }
  return self.registration.showNotification(n.title, { body: n.body, tag: n.tag, renotify: true, icon: 'icon.png', badge: 'icon.png', data: { url: './' } });
}
