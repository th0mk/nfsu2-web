// The lobby protocol's framing (EA DirtySock "ProtoAries", as SPEED2.EXE's
// lobby client and EA's server.dll speak it):
//   [0..3]   kind: four ASCII characters ("auth", "+rom", "@dir", ...)
//   [4..7]   code: four bytes; 0 in a request and in a reply that succeeded,
//            an error tag ("imst", "pass", ...) or a subkind ("new7") otherwise
//   [8..11]  u32 big-endian: the whole message's length, these 12 bytes included
//   [12..]   the body: KEY=VALUE fields, NUL-terminated
// The body is DirtySock's "TagField" text.  The client finds a field by its
// key (case-insensitive) after any byte <= 0x20 (so tabs, newlines and spaces
// all separate fields) and writes a value (0x741A50) with every byte below
// 0x20, 0x7F and = " : % ^ as %xx (lowercase hex), in double quotes when the
// value has a space.  This module writes exactly that and reads both that and
// plain values.
//
// Secrets: the login messages carry PASS (the account password, XOR-masked or
// encrypted, both reversible: as good as plain text), REGKEY (the CD key) and
// PSES (a checksum of the player's game files).  parseFields() takes a list of
// keys to skip.  The lobby skips these values except PASS on game creation
// and joining, where it means the room password rather than an account password.
// redactBody() is for the dev-only frame trace (trace.mjs).

/** keys skipped by default: the account password, the CD key, the file checksum */
export const SECRET_KEYS = Object.freeze(['PASS', 'REGKEY', 'PSES']);
/** keys a trace must not show (the secrets, a game's own password, and the key of a persona's Messenger login) */
const REDACT_RE = /^(PASS\w*|REGKEY|PSES|LKEY)$/i;

export const HDR = 12;
/** the client's receive buffer is 0x8000 bytes; nothing it sends or needs is near this */
export const MAX_FRAME = 8192;

const SPECIAL = new Set([0x3d, 0x22, 0x3a, 0x25, 0x5e, 0x7f]);   // = " : % ^ DEL
const HEX = '0123456789abcdef';

/** a value as the client writes one */
export function encodeValue(v) {
  const b = Buffer.from(String(v), 'latin1');
  let s = '';
  for (const c of b) s += c < 0x20 || SPECIAL.has(c) ? '%' + HEX[c >> 4] + HEX[c & 15] : String.fromCharCode(c);
  return b.includes(0x20) ? `"${s}"` : s;
}

/** a body: [[key, value], ...] or { key: value } -> "K=V<sep>K=V<sep>" text (no NUL) */
export function encodeFields(fields, sep = '\n') {
  const list = Array.isArray(fields) ? fields : Object.entries(fields);
  let s = '';
  for (const [k, v] of list) {
    if (v === undefined || v === null) continue;
    if (!/^[A-Za-z0-9_+@$~]{1,32}$/.test(k)) throw new Error(`bad field name ${JSON.stringify(k)}`);
    s += `${k}=${encodeValue(v)}${sep}`;
  }
  return s;
}

/** a 4-character kind or code: a string (padded with NUL) or 0 */
function tag4(t, what) {
  const b = Buffer.alloc(4);
  if (t === 0 || t === undefined || t === null || t === '') return b;
  const s = Buffer.from(String(t), 'latin1');
  if (s.length > 4) throw new Error(`${what} ${JSON.stringify(t)} is longer than 4 characters`);
  s.copy(b);
  return b;
}

/** one message: kind, code (0 = success), fields ([[k, v]] or {k: v}) or a ready body string */
export function encodeFrame(kind, code, fields = [], sep = '\n') {
  const text = typeof fields === 'string' ? fields : encodeFields(fields, sep);
  const body = Buffer.from(text + '\0', 'latin1');
  const f = Buffer.alloc(HDR + body.length);
  tag4(kind, 'kind').copy(f, 0); tag4(code, 'code').copy(f, 4);
  f.writeUInt32BE(f.length, 8);
  body.copy(f, HDR);
  return f;
}

/** a 4-byte tag as text: printable characters kept, NULs dropped, anything else as \xNN */
export function tagText(b) {
  let s = '';
  for (const c of b) { if (c === 0) continue; s += c >= 0x20 && c < 0x7f ? String.fromCharCode(c) : '\\x' + HEX[c >> 4] + HEX[c & 15]; }
  return s;
}

/**
 * The messages in a byte stream.  push() returns the complete messages so
 * far as { kind, code, body } (body: the bytes after the header, NUL
 * included); a length below 12 or above max makes the stream bad (error set,
 * nothing more is returned: the caller drops the connection).
 */
export class FrameReader {
  constructor(max = MAX_FRAME) { this.max = max; this.buf = Buffer.alloc(0); this.error = null; }
  push(chunk) {
    if (this.error) return [];
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : Buffer.from(chunk);
    const out = [];
    while (this.buf.length >= HDR) {
      const n = this.buf.readUInt32BE(8);
      if (n < HDR || n > this.max) { this.error = `a message of ${n} bytes (at most ${this.max})`; this.buf = Buffer.alloc(0); break; }
      if (this.buf.length < n) break;
      out.push({ kind: tagText(this.buf.subarray(0, 4)), code: tagText(this.buf.subarray(4, 8)), body: this.buf.subarray(HDR, n) });
      this.buf = this.buf.subarray(n);
    }
    if (!this.buf.length) this.buf = Buffer.alloc(0);
    else if (this.buf.byteOffset > 65536) this.buf = Buffer.from(this.buf);   // let a big consumed prefix go
    return out;
  }
  /** bytes waiting for the rest of a message */
  get pending() { return this.buf.length; }
}

function unescape(s) {
  return s.replace(/%([0-9a-fA-F]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16)));
}

/**
 * The fields of a body, in order: [[KEY, value], ...], keys upper-cased.
 * skip: keys whose values are passed over without being decoded (a list,
 * SECRET_KEYS by default, or a test on the upper-cased key); they appear in
 * the list with the value null, so a caller can tell that one was sent.  Text
 * after the first NUL is ignored.
 */
export function parseFields(body, skip = SECRET_KEYS) {
  const b = Buffer.isBuffer(body) ? body : Buffer.from(String(body), 'latin1');
  let end = b.indexOf(0); if (end < 0) end = b.length;
  const skipped = typeof skip === 'function' ? skip : (set => k => set.has(k))(new Set(skip.map(k => k.toUpperCase())));
  const out = [];
  let i = 0;
  while (i < end) {
    while (i < end && b[i] <= 0x20) i++;
    if (i >= end) break;
    let k = i;
    while (k < end && b[k] > 0x20 && b[k] !== 0x3d) k++;
    const key = b.toString('latin1', i, k).toUpperCase();
    if (k >= end || b[k] !== 0x3d) { i = k; continue; }        // a word without "=": not a field
    let j = k + 1, raw;
    if (b[j] === 0x22) {                                        // "quoted value"
      let q = j + 1; while (q < end && b[q] !== 0x22) q++;
      raw = skipped(key) ? null : b.toString('latin1', j + 1, q);
      i = q + 1;
    } else {
      let q = j; while (q < end && b[q] > 0x20) q++;
      raw = skipped(key) ? null : b.toString('latin1', j, q);
      i = q;
    }
    out.push([key, raw === null ? null : unescape(raw)]);
  }
  return out;
}

/** parseFields as a Map (the first of a repeated key wins, as the client's own search finds the first) */
export function fieldMap(body, skip = SECRET_KEYS) {
  const m = new Map();
  for (const [k, v] of parseFields(body, skip)) if (!m.has(k)) m.set(k, v);
  return m;
}

/** a body as one printable line for the dev-only trace, every secret replaced */
export function redactBody(body) {
  return parseFields(body, k => REDACT_RE.test(k)).map(([k, v]) => `${k}=${v === null ? '<redacted>' : JSON.stringify(v)}`).join(' ');
}
