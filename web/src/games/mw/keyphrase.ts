// How Most Wanted's cloud saves derive from a keyphrase (web/src/app/vault.ts): the same keyphrase as Underground
// 2's, and its own key and token from the same 64 PBKDF2 bytes, through HKDF-SHA-256 with info of its own, so neither
// game's box is the other's.  Fixed before Most Wanted's first stored key: never to change.  No imports.

/** HKDF-SHA-256's info over the keyphrase's 64 PBKDF2 bytes (no salt): 64 bytes, the key and the token */
export const DERIVE_INFO: string | null = 'nfsmw keyphrase saves v1: key and token';
/** the AES-GCM additional data a box is sealed with */
export const SEAL_AAD = 'nfsmw keyphrase saves v1';
