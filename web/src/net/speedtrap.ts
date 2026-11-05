// Optional browser HUD over the hub's existing UDP transport. Other clients ignore these packets.
import * as P from './proto';
export interface TrapState {
  v: 1; id: string; name: string; active: boolean; distance: number; bearing: number;
  best: number; last: number; passes: number; session: string;
  sequence: number;
  event: { id: string; name: string; speed: number; best: boolean } | null;
  nearby: { id: string; distance: number; bearing: number }[];
  gate: [number, number, number, number];
  rows: { name: string; speed: number; me: boolean }[];
}
const decoder = new TextDecoder();
const marker = (b: Uint8Array, s: string) => b.length >= P.F_DATA + 4 && [...s].every((c,i) => b[P.F_DATA+i] === c.charCodeAt(0));
/** Returns only trusted hub responses; the switch overwrites peers' source addresses. */
export function trapResponse(b: Uint8Array): TrapState | null {
  if (b.length < P.F_DATA+4 || b.length > P.F_DATA+P.NET_MAX_PAYLOAD || b[P.F_KIND] !== P.NET_KIND_UDP ||
      new DataView(b.buffer,b.byteOffset,b.byteLength).getUint32(P.F_SRC) !== P.NET_ROAM_HUB || !marker(b,'STP1')) return null;
  try {
    const s = JSON.parse(decoder.decode(b.subarray(P.F_DATA+4)));
    const num = (x: unknown) => typeof x === 'number' && Number.isFinite(x);
    const score = (x: unknown) => num(x) && Number.isInteger(x) && Number(x) >= 0 && Number(x) <= 5040;
    const id = (x: unknown) => typeof x === 'string' && /^[a-z0-9_-]{1,32}$/.test(x);
    if (!id(s.id) || !Number.isSafeInteger(s.sequence) || s.sequence < 0 ||
        !Array.isArray(s.nearby) || s.nearby.length > 4 || s.nearby.some((r: {id: unknown; distance: unknown; bearing: unknown}) =>
          !r || !id(r.id) || !num(r.distance) || Number(r.distance) < 0 || Number(r.distance) > 600 || !num(r.bearing)) ||
        new Set(s.nearby.map((r: {id: string}) => r.id)).size !== s.nearby.length ||
        (s.event !== null && (!s.event || !id(s.event.id) || typeof s.event.name !== 'string' || s.event.name.length > 64 ||
          !score(s.event.speed) || typeof s.event.best !== 'boolean')) || s.v !== 1 || typeof s.name !== 'string' || s.name.length > 64 || typeof s.active !== 'boolean' ||
        !num(s.distance) || s.distance < 0 || !num(s.bearing) || !score(s.best) || !score(s.last) ||
        !Number.isSafeInteger(s.passes) || s.passes < 0 || typeof s.session !== 'string' || s.session.length > 80 ||
        !Array.isArray(s.gate) || s.gate.length !== 4 || !s.gate.every(num) ||
        !Array.isArray(s.rows) || s.rows.length > 5 || s.rows.some((r: {name: unknown; speed: unknown; me: unknown}) =>
          !r || typeof r.name !== 'string' || r.name.length > 128 || !score(r.speed) || typeof r.me !== 'boolean')) return null;
    return s as TrapState;
  } catch { return null; }
}
/** Request only after a hub PEERS packet gives this game's ports; never ask arbitrary peers. */
export function trapRequest(b: Uint8Array): Uint8Array<ArrayBuffer> | null {
  if (b.length < P.F_DATA+20 || b[P.F_KIND] !== P.NET_KIND_UDP) return null;
  const d = new DataView(b.buffer,b.byteOffset,b.byteLength);
  if (d.getUint32(P.F_SRC) !== P.NET_ROAM_HUB || b[P.F_DATA+5] !== 2) return null; // RC_MSG_PEERS
  const r = new Uint8Array(P.F_DATA+4), v = new DataView(r.buffer);
  r[P.F_KIND]=P.NET_KIND_UDP;
  v.setUint32(P.F_SRC,d.getUint32(P.F_DST)); v.setUint32(P.F_DST,P.NET_ROAM_HUB);
  v.setUint16(P.F_SPORT,d.getUint16(P.F_DPORT)); v.setUint16(P.F_DPORT,d.getUint16(P.F_SPORT));
  r.set(new TextEncoder().encode('STQ1'),P.F_DATA);
  return r;
}
