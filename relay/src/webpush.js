// Web Push with WebCrypto only (no libraries): VAPID (RFC 8292) and the aes128gcm payload encryption (RFC 8291).
// Kept free of Cloudflare imports so the browser can test it against the RFC 8291 example.

const enc = new TextEncoder();

export const b64u = bytes => btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
export const unb64u = s => Uint8Array.from(atob(s.replace(/-/g, '+').replace(/_/g, '/')), c => c.charCodeAt(0));

export function concat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

async function hmac(key, data) {
  const k = await crypto.subtle.importKey('raw', key, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return new Uint8Array(await crypto.subtle.sign('HMAC', k, data));
}

// The payload for one browser subscription ({endpoint, keys: {p256dh, auth}}), one aes128gcm record.
// `fixed` (tests only) supplies the sender's key pair and the salt the RFC example uses.
export async function encryptPayload(sub, payload, fixed = {}) {
  const uaPublic = unb64u(sub.keys.p256dh), authSecret = unb64u(sub.keys.auth);
  const local = fixed.keys || await crypto.subtle.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, true, ['deriveBits']);
  const asPublic = new Uint8Array(await crypto.subtle.exportKey('raw', local.publicKey));
  const uaKey = await crypto.subtle.importKey('raw', uaPublic, { name: 'ECDH', namedCurve: 'P-256' }, false, []);
  const ecdh = new Uint8Array(await crypto.subtle.deriveBits({ name: 'ECDH', public: uaKey }, local.privateKey, 256));
  const prkKey = await hmac(authSecret, ecdh);
  const ikm = await hmac(prkKey, concat(enc.encode('WebPush: info\0'), uaPublic, asPublic, new Uint8Array([1])));
  const salt = fixed.salt || crypto.getRandomValues(new Uint8Array(16));
  const prk = await hmac(salt, ikm);
  const cek = (await hmac(prk, concat(enc.encode('Content-Encoding: aes128gcm\0'), new Uint8Array([1])))).slice(0, 16);
  const nonce = (await hmac(prk, concat(enc.encode('Content-Encoding: nonce\0'), new Uint8Array([1])))).slice(0, 12);
  const key = await crypto.subtle.importKey('raw', cek, 'AES-GCM', false, ['encrypt']);
  const data = typeof payload === 'string' ? enc.encode(payload) : payload;
  const body = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv: nonce }, key, concat(data, new Uint8Array([2]))));
  const head = new Uint8Array(16 + 4 + 1 + 65);
  head.set(salt);
  new DataView(head.buffer).setUint32(16, 4096);              // record size
  head[20] = 65;
  head.set(asPublic, 21);
  return concat(head, body);
}

// A VAPID key pair as storable JSON: the private key as JWK, the public one raw, base64url (applicationServerKey).
export async function newVapid() {
  const pair = await crypto.subtle.generateKey({ name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']);
  return {
    privateJwk: await crypto.subtle.exportKey('jwk', pair.privateKey),
    publicKey: b64u(await crypto.subtle.exportKey('raw', pair.publicKey)),
  };
}

// "Authorization" for one push service. `subject` identifies the sender to the push service (a URL, no e-mail).
export async function vapidAuth(endpoint, vapid, subject) {
  const part = o => b64u(enc.encode(JSON.stringify(o)));
  const unsigned = `${part({ typ: 'JWT', alg: 'ES256' })}.${part({ aud: new URL(endpoint).origin, exp: Math.floor(Date.now() / 1000) + 12 * 3600, sub: subject })}`;
  const key = await crypto.subtle.importKey('jwk', vapid.privateJwk, { name: 'ECDSA', namedCurve: 'P-256' }, false, ['sign']);
  const sig = await crypto.subtle.sign({ name: 'ECDSA', hash: 'SHA-256' }, key, enc.encode(unsigned));   // raw r|s, as JWT wants
  return `vapid t=${unsigned}.${b64u(sig)}, k=${vapid.publicKey}`;
}

// Sends one notification; returns the push service's HTTP status (404/410 = the subscription is gone).
export async function sendPush(sub, payload, vapid, subject) {
  const res = await fetch(sub.endpoint, {
    method: 'POST',
    headers: {
      Authorization: await vapidAuth(sub.endpoint, vapid, subject),
      'Content-Encoding': 'aes128gcm',
      'Content-Type': 'application/octet-stream',
      TTL: '86400',
      Urgency: 'high',
    },
    body: await encryptPayload(sub, payload),
  });
  return res.status;
}
