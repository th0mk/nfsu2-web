// How Underground 2's cloud saves derive from a keyphrase (web/src/app/vault.ts): as they always did.  No imports.

/** HKDF-SHA-256's info over the keyphrase's 64 PBKDF2 bytes, or null: those bytes are the key and the token */
export const DERIVE_INFO: string | null = null;
/** the AES-GCM additional data a box is sealed with */
export const SEAL_AAD = 'nfsu2-xyz keyphrase saves v1';
