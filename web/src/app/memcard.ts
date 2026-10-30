// The checksum of EA's memory-card container ("20CM"), which both games' saves are (Underground 2's: profile.ts,
// Most Wanted's: games/mw/profile.ts): CRC-32 with the polynomial 0x04C11DB7, most significant bit first, the message
// shifted into the register without appended zeros (docs/mw/save-format.md section 4; OpenUG2's SPEC section 3.4).
// Pure, no imports.

const T = new Uint32Array(256);                      // MSB-first CRC table, polynomial 0x04C11DB7
for (let i = 0; i < 256; i++) {
  let r = i << 24;
  for (let k = 0; k < 8; k++) r = r & 0x80000000 ? (r << 1) ^ 0x04C11DB7 : r << 1;
  T[i] = r >>> 0;
}

/** The game's checksum of a block: 0 below 4 bytes. */
export function checksum(d: Uint8Array): number {
  if (d.length < 4) return 0;
  let r = ~((d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3]) >>> 0;
  for (let i = 4; i < d.length; i++) r = (((r << 8) | d[i]) ^ T[r >>> 24]) >>> 0;
  return ~r >>> 0;
}

/** A file that starts as the games' save files do ("20CM"): a profile of either game, or one of another version. */
export const isMemcardFile = (file: Uint8Array) => file.length >= 4 && file[0] === 0x32 && file[1] === 0x30 && file[2] === 0x43 && file[3] === 0x4D;
